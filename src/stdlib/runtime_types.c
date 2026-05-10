/* SPDX-License-Identifier: BSD-3-Clause */
/* runtime_types.c — M6 Phase 7: C-native runtime-type protos.
 *
 * Phase 7 ships the Exception primitive root.  See runtime_types.h banner
 * for what is intentionally deferred (Code reflection, Tag.new scripted
 * constructor, Event.* — already in Wave 1).
 *
 * Exception ABI:
 *   var e = Exception.new("boom")  → fresh clone of Exception proto with
 *                                    a `message` slot bound to args[0].
 *   e.message                       → returns the bound message UValue.
 *   e.raise                         → deposit pending_unwind = UEXEC_THROW
 *                                    with unwind_value = self, so an
 *                                    enclosing try/catch handler binds the
 *                                    Exception object as its catch var.
 *
 * The raise mechanic depends on a v0.6.1 VM-internal change in the OP_CALL
 * native arm: when a native_fn returns UEXEC_THROW, the dispatch loop now
 * routes through pending_unwind / safepoint instead of HALTing with a fatal
 * TypeError (the pre-Phase-7 baseline).  See src/vm/uvm.c "M6 Phase 7" for
 * the gated change. */

#include "stdlib/runtime_types.h"
#include "stdlib/object_root.h"        /* urbi_native_closure_create + raise helpers */

#include "module/umodule.h"            /* UValue, UVAL_* */
#include "object/uobject.h"            /* urbi_object_alloc / clone / set_local_slot */
#include "realm/urealm.h"              /* URealm */
#include "runtime/uclosure.h"          /* urbi_native_method_fn */
#include "runtime/umacros.h"           /* urbi_strlen */
#include "sched/ustrand.h"             /* UEXEC_OK, UEXEC_THROW, UStrand */
#include "urbi/object.h"               /* URBI_ATOM_OBJECT */
#include "urbi/types.h"                /* urbi_value_nil */
#include "urbi/urbi.h"                 /* URBI_OK, URBI_ERR_*, urbi_throw */
#include "value/uintern.h"             /* ustr_intern, USymbol */
#include "vm/uvm.h"                    /* UVM, vm->exception_proto */

#include <stdint.h>
#include <stddef.h>

/* === UValue construction helpers ========================================= */

static UValue
val_obj(UObject *o)
{
    UValue v = urbi_value_nil();
    v.kind = (uint8_t)UVAL_OBJECT;
    v.v.p  = o;
    return v;
}

/* === Exception.new(message) =============================================
 *
 * Clone the Exception proto and install args[0] as the local `message`
 * slot.  Returns the fresh clone wrapped in a UVAL_OBJECT.
 *
 * Receiver routing: `self` is the Exception proto (or a subclass clone)
 * supplied by the OP_CALL native arm via vm->last_recv.  Cloning the
 * receiver — not the proto-singleton — lets future scripted subclasses
 * (`class TypeError : Exception { ... }`) flow through `.new` correctly
 * when Phase 10 stdlib overlays land. */

static int
exc_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "Exception.new", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "Exception.new: receiver must be an Object", out);

    UObject *e = urbi_object_clone(vm, (UObject *)self.v.p);
    if (e == NULL) return urbi_raise_oom(vm, out);

    USymbol *sym_message = (USymbol *)ustr_intern(vm, "message", 7);
    if (sym_message == NULL) return urbi_raise_oom(vm, out);

    if (urbi_object_set_local_slot(vm, e, sym_message, args[0]) != 0)
        return urbi_raise_oom(vm, out);

    *out = val_obj(e);
    return UEXEC_OK;
}

/* === Exception.raise ====================================================
 *
 * Deposit pending_unwind = UEXEC_THROW with unwind_value = self.  The
 * dispatch loop's safepoint observes pending_unwind on next entry and
 * walks the cleanup stack via urbi_unwind, which routes a UCLEANUP_TRY_-
 * FRAME match to its handler PC binding the unwind value as the catch
 * variable.
 *
 * The native function MUST return UEXEC_OK so the OP_CALL native arm
 * doesn't HALT.  The Phase-7 OP_CALL native arm sees pending_unwind
 * non-OK after the call and jumps to safepoint.
 *
 * `self` is the Exception instance (a clone with .message bound) — that
 * is exactly the value `catch (e) { e.message }` expects to bind. */

static int
exc_raise(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Exception.raise", 0, nargs, out);
    if (vm->cur_strand == NULL) {
        /* Defensive: no active strand (shouldn't occur from script).
         * Fall back to the legacy raise helper which prints + UEXEC_THROW. */
        return urbi_raise_type(vm, "Exception.raise: no active strand", out);
    }

    urbi_throw(vm->cur_strand, self);
    *out = urbi_value_nil();
    /* Return UEXEC_OK so OP_CALL doesn't fatal-halt; safepoint picks up
     * the deposited pending_unwind. */
    return UEXEC_OK;
}

/* === Method-table install helper ========================================= */

typedef struct {
    const char           *name;
    urbi_native_method_fn fn;
} RtMethodEntry;

static int
install_methods(UVM *vm, UObject *proto,
                const RtMethodEntry *table, size_t count)
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

/* === Method tables ======================================================= */

static const RtMethodEntry EXCEPTION_METHODS[] = {
    { "new",   exc_new   },
    { "raise", exc_raise }
};

#define EXCEPTION_METHODS_COUNT (sizeof(EXCEPTION_METHODS) / sizeof(EXCEPTION_METHODS[0]))

/* === urbi_stdlib_register_runtime_types =================================
 *
 * Allocates vm->exception_proto (a vanilla URBI_ATOM_OBJECT-family UObject)
 * and installs Exception.new / Exception.raise as native methods.  GC
 * reachability is via the same object_roots_walker path that shades
 * vm->container_*_proto — extending uobject.c's MARK_ROOTS list to include
 * exception_proto.
 *
 * Idempotent: re-allocates only when vm->exception_proto is NULL. */

int
urbi_stdlib_register_runtime_types(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;

    if (vm->exception_proto == NULL) {
        UObject *p = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
        if (p == NULL) return URBI_ERR_OOM;
        vm->exception_proto = p;
    }

    int rc = install_methods(vm, vm->exception_proto,
                             EXCEPTION_METHODS, EXCEPTION_METHODS_COUNT);
    if (rc != URBI_OK) return rc;

    /* Also install a default `message` slot on the proto itself so
     * `Exception.message` (without a clone) reads as nil rather than
     * raising a missing-slot error.  Per-instance .new() overwrites
     * this with the user-supplied value. */
    USymbol *sym_message = (USymbol *)ustr_intern(vm, "message", 7);
    if (sym_message == NULL) return URBI_ERR_OOM;
    if (urbi_object_set_local_slot(vm, vm->exception_proto, sym_message,
                                   urbi_value_nil()) != 0) {
        return URBI_ERR_OOM;
    }

    return URBI_OK;
}

/* === urbi_stdlib_register_runtime_globals ===============================
 *
 * Post-registry hook: installs Exception as a realm global on `realm`.
 * Mirrors urbi_stdlib_register_container_globals — lands at slots 15+,
 * past the v1.0 packed-flag CONSTANT enforcement range (slots 0..7).
 *
 * Called by urbi_populate_realm_globals AFTER the 15-row registry loop
 * AND after urbi_stdlib_register_container_globals. */

int
urbi_stdlib_register_runtime_globals(UVM *vm, URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;

    if (vm->exception_proto != NULL) {
        int rc = urbi_realm_set_global(vm, realm, "Exception", 9,
                                       val_obj(vm->exception_proto));
        if (rc != URBI_OK) return rc;
    }
    return URBI_OK;
}
