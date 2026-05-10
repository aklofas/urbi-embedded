/* SPDX-License-Identifier: BSD-3-Clause */
/* primitives.c — M6 Phase 9: C-native primitives (Mutex, Date, Duration).
 *
 * Mutex / Date / Duration — see banner in primitives.h.  Phase 9 grows
 * incrementally task-by-task: T93 shell, T94 Mutex, T95 Date, T96
 * Duration, T97 Date.plus(Duration) seam.
 *
 * Allocation pattern mirrors namespaces.c / runtime_types.c: a vanilla
 * URBI_ATOM_OBJECT-family UObject per primitive proto, methods installed
 * via a per-primitive method table walked by install_methods.  GC
 * reachability comes from object_roots_walker (uobject.c) which shades
 * each vm->*_proto field during MARK_ROOTS. */

#include "stdlib/primitives.h"
#include "stdlib/object_root.h"        /* urbi_native_closure_create + raise helpers */

#include "module/umodule.h"            /* UValue / UVAL_* */
#include "object/uobject.h"            /* urbi_object_alloc / clone / set_local_slot */
#include "object/ushape.h"             /* urbi_shape_find_slot */
#include "realm/urealm.h"              /* URealm */
#include "runtime/uclosure.h"          /* urbi_native_method_fn */
#include "runtime/umacros.h"           /* urbi_strlen */
#include "sched/ustrand.h"             /* UEXEC_OK / UEXEC_THROW */
#include "urbi/object.h"               /* URBI_ATOM_OBJECT */
#include "urbi/types.h"                /* urbi_value_nil */
#include "urbi/urbi.h"                 /* URBI_OK / URBI_ERR_* / urbi_realm_set_global */
#include "value/uintern.h"             /* ustr_intern + USymbol */
#include "vm/uvm.h"                    /* UVM */

#include <stdint.h>
#include <stddef.h>

/* === UValue construction helpers ========================================= */

static UValue
val_bool(int b)
{
    UValue v = urbi_value_nil();
    v.kind = (uint8_t)UVAL_BOOL;
    v.v.i  = b ? 1 : 0;
    return v;
}

static UValue
val_obj(UObject *o)
{
    UValue v = urbi_value_nil();
    v.kind = (uint8_t)UVAL_OBJECT;
    v.v.p  = o;
    return v;
}

/* === Method-table install helper ========================================= */

typedef struct {
    const char           *name;
    urbi_native_method_fn fn;
} PMethodEntry;

static int
install_methods(UVM *vm, UObject *proto,
                const PMethodEntry *table, size_t count)
{
    if (proto == NULL) return URBI_ERR_OOM;
    size_t i;
    for (i = 0U; i < count; i++) {
        UClosure *cl = urbi_native_closure_create(vm, table[i].fn);
        if (cl == NULL) return URBI_ERR_OOM;

        USymbol *sym = (USymbol *)ustr_intern(vm, table[i].name,
                                              urbi_strlen(table[i].name));
        if (sym == NULL) return URBI_ERR_OOM;

        UValue v = urbi_value_nil();
        v.kind = (uint8_t)UVAL_CLOSURE;
        v.v.p  = cl;
        if (urbi_object_set_local_slot(vm, proto, sym, v) != 0)
            return URBI_ERR_OOM;
    }
    return URBI_OK;
}

/* Install a default slot (UValue) on proto.  Returns URBI_OK / URBI_ERR_OOM. */
static int
install_default_slot(UVM *vm, UObject *proto, const char *name, UValue value)
{
    USymbol *sym = (USymbol *)ustr_intern(vm, name, urbi_strlen(name));
    if (sym == NULL) return URBI_ERR_OOM;
    if (urbi_object_set_local_slot(vm, proto, sym, value) != 0)
        return URBI_ERR_OOM;
    return URBI_OK;
}

/* === Slot read/write helpers (for instance state stored on UObject) ======
 *
 * Mutex / Date / Duration store their per-instance state on hidden slots
 * (`_locked`, `seconds`, `microseconds`) of the cloned proto.  These
 * helpers route through ustr_intern + urbi_shape_find_slot to read /
 * write the slot value as a UValue. */

static int
read_local_slot(UVM *vm, UObject *o, const char *name, UValue *out)
{
    USymbol *sym = (USymbol *)ustr_intern(vm, name, urbi_strlen(name));
    if (sym == NULL) return -1;
    int32_t idx = urbi_shape_find_slot(o->shape, sym);
    if (idx < 0 || o->slots == NULL) {
        *out = urbi_value_nil();
        return 0;
    }
    *out = o->slots[idx];
    return 0;
}

static int
write_local_slot(UVM *vm, UObject *o, const char *name, UValue value)
{
    USymbol *sym = (USymbol *)ustr_intern(vm, name, urbi_strlen(name));
    if (sym == NULL) return -1;
    if (urbi_object_set_local_slot(vm, o, sym, value) != 0)
        return -1;
    return 0;
}

/* === Mutex (T94) =========================================================
 *
 * v1.0 single-VM cooperative-only contract: lock/unlock/tryLock are
 * non-blocking flag flips on a hidden `_locked` UVAL_BOOL slot of the
 * instance UObject.  Phase 10's `.u` overlay grows Mutex.synchronized
 * via `waituntil m.locked() == false` for cooperative wait semantics. */

static int
mutex_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Mutex.new", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "Mutex.new: receiver must be an Object", out);

    UObject *m = urbi_object_clone(vm, (UObject *)self.v.p);
    if (m == NULL) return urbi_raise_oom(vm, out);

    if (write_local_slot(vm, m, "_locked", val_bool(0)) != 0)
        return urbi_raise_oom(vm, out);

    *out = val_obj(m);
    return UEXEC_OK;
}

static int
mutex_locked(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Mutex.locked", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "Mutex.locked: receiver must be a Mutex", out);

    UValue v;
    if (read_local_slot(vm, (UObject *)self.v.p, "_locked", &v) != 0)
        return urbi_raise_oom(vm, out);
    /* Coerce UVAL_NIL (proto unread) to false. */
    if (v.kind == (uint8_t)UVAL_BOOL) {
        *out = v;
    } else {
        *out = val_bool(0);
    }
    return UEXEC_OK;
}

static int
mutex_lock(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Mutex.lock", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "Mutex.lock: receiver must be a Mutex", out);

    if (write_local_slot(vm, (UObject *)self.v.p, "_locked", val_bool(1)) != 0)
        return urbi_raise_oom(vm, out);

    *out = urbi_value_nil();
    return UEXEC_OK;
}

static int
mutex_unlock(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Mutex.unlock", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "Mutex.unlock: receiver must be a Mutex", out);

    if (write_local_slot(vm, (UObject *)self.v.p, "_locked", val_bool(0)) != 0)
        return urbi_raise_oom(vm, out);

    *out = urbi_value_nil();
    return UEXEC_OK;
}

static int
mutex_trylock(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Mutex.tryLock", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "Mutex.tryLock: receiver must be a Mutex", out);

    UObject *m = (UObject *)self.v.p;
    UValue v;
    if (read_local_slot(vm, m, "_locked", &v) != 0)
        return urbi_raise_oom(vm, out);

    int already = (v.kind == (uint8_t)UVAL_BOOL && v.v.i != 0);
    if (already) {
        *out = val_bool(0);
        return UEXEC_OK;
    }

    if (write_local_slot(vm, m, "_locked", val_bool(1)) != 0)
        return urbi_raise_oom(vm, out);

    *out = val_bool(1);
    return UEXEC_OK;
}

static const PMethodEntry MUTEX_METHODS[] = {
    { "new",     mutex_new     },
    { "locked",  mutex_locked  },
    { "lock",    mutex_lock    },
    { "unlock",  mutex_unlock  },
    { "tryLock", mutex_trylock }
};

#define MUTEX_METHODS_COUNT (sizeof(MUTEX_METHODS) / sizeof(MUTEX_METHODS[0]))

/* === urbi_stdlib_register_primitives ====================================
 *
 * Allocates Mutex / Date / Duration proto UObjects per task. */

int
urbi_stdlib_register_primitives(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    int rc;

    /* --- T94 Mutex --- */
    if (vm->mutex_proto == NULL) {
        UObject *p = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
        if (p == NULL) return URBI_ERR_OOM;
        vm->mutex_proto = p;
    }
    rc = install_methods(vm, vm->mutex_proto, MUTEX_METHODS, MUTEX_METHODS_COUNT);
    if (rc != URBI_OK) return rc;
    /* Default the proto's `_locked` slot to false so an un-cloned Mutex
     * also reads as unlocked. */
    rc = install_default_slot(vm, vm->mutex_proto, "_locked", val_bool(0));
    if (rc != URBI_OK) return rc;

    return URBI_OK;
}

/* === urbi_stdlib_register_primitives_globals ============================
 *
 * Post-registry hook: bind primitives as realm globals on `realm`.  Lands
 * at slots 15+, past the v1.0 packed-flag CONSTANT enforcement range
 * (slots 0..7).  Mirrors urbi_stdlib_register_namespace_globals. */

int
urbi_stdlib_register_primitives_globals(UVM *vm, URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;

    int rc;
    if (vm->mutex_proto != NULL) {
        rc = urbi_realm_set_global(vm, realm, "Mutex", 5,
                                   val_obj(vm->mutex_proto));
        if (rc != URBI_OK) return rc;
    }
    if (vm->date_proto != NULL) {
        rc = urbi_realm_set_global(vm, realm, "Date", 4,
                                   val_obj(vm->date_proto));
        if (rc != URBI_OK) return rc;
    }
    if (vm->duration_proto != NULL) {
        rc = urbi_realm_set_global(vm, realm, "Duration", 8,
                                   val_obj(vm->duration_proto));
        if (rc != URBI_OK) return rc;
    }
    return URBI_OK;
}
