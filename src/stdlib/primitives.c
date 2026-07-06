/* SPDX-License-Identifier: BSD-3-Clause */
/* primitives.c — C-native primitives (Mutex, Date, Duration).
 *
 * Mutex / Date / Duration — see banner in primitives.h.  Sections:
 * Mutex, Date, Duration, Date.plus(Duration) seam.
 *
 * Allocation pattern mirrors namespaces.c / runtime_types.c: a vanilla
 * URBI_ATOM_OBJECT-family UObject per primitive proto, methods installed
 * via UNativeMethodDef tables with URBI_REGISTER_METHODS.  GC
 * reachability comes from object_roots_walker (uobject.c) which shades
 * each vm->*_proto field during MARK_ROOTS. */

/* gmtime_r is POSIX.1-2001 / _POSIX_C_SOURCE >= 1.  Define before any
 * libc header to ensure the prototype is visible on stricter glibc
 * builds (notably GCC -std=c99 on Linux, which defaults to a strict
 * conforming mode that hides POSIX symbols). */
#if defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 1)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "stdlib/primitives.h"
#include "stdlib/object_root.h"        /* urbi_native_closure_create + raise helpers */

#include "chunk/uchunk.h"            /* UValue / UVAL_* */
#include "object/uobject.h"            /* urbi_object_alloc / clone / set_local_slot */
#include "object/ushape.h"             /* urbi_shape_find_slot */
#include "realm/urealm.h"              /* URealm */
#include "runtime/uclosure.h"          /* urbi_native_method_fn */
#include "runtime/umacros.h"           /* urbi_strlen */
#include "sched/ustrand.h"             /* UEXEC_OK / UEXEC_THROW */
#include "urbi/object.h"               /* URBI_ATOM_OBJECT */
#include "urbi/types.h"                /* urbi_make_nil */
#include "urbi/urbi.h"                 /* URBI_OK / URBI_ERR_* / urbi_realm_set_global */
#include "value/uintern.h"             /* ustr_intern + USymbol */
#include "vm/uvm.h"                    /* UVM */

#include <stdint.h>
#include <stddef.h>
#if defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 1)
#  include <time.h>                    /* time_t, time(), gmtime_r, strftime */
#endif

/* === Method-table install helper ========================================= */

/* Method tables use UNativeMethodDef from stdlib/object_root.h;
 * URBI_REGISTER_METHODS does the install loop. */

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
    const USymbol *sym = (const USymbol *)ustr_intern(vm, name, urbi_strlen(name));
    if (sym == NULL) return -1;
    int32_t idx = urbi_shape_find_slot(o->shape, sym);
    if (idx < 0 || o->slots == NULL) {
        *out = urbi_make_nil();
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

/* === Mutex ===============================================================
 *
 * v1.0 single-VM cooperative-only contract: lock/unlock/tryLock are
 * non-blocking flag flips on a hidden `_locked` UVAL_BOOL slot of the
 * instance UObject.  Phase 10's `.u` overlay grows Mutex.synchronized
 * via `waituntil m.locked() == false` for cooperative wait semantics. */

static int
mutex_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Mutex.new", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_OBJECT, "Mutex.new: receiver must be an Object", out);

    UObject *m = urbi_object_clone(vm, (UObject *)self.v.p);
    if (m == NULL) return urbi_raise_oom(vm, out);

    if (write_local_slot(vm, m, "_locked", urbi_make_bool(0)) != 0)
        return urbi_raise_oom(vm, out);

    *out = urbi_make_object(m);
    return UEXEC_OK;
}

static int
mutex_locked(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Mutex.locked", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_OBJECT, "Mutex.locked: receiver must be a Mutex", out);

    UValue v;
    if (read_local_slot(vm, (UObject *)self.v.p, "_locked", &v) != 0)
        return urbi_raise_oom(vm, out);
    /* Coerce UVAL_NIL (proto unread) to false. */
    if (v.kind == (uint8_t)UVAL_BOOL) {
        *out = v;
    } else {
        *out = urbi_make_bool(0);
    }
    return UEXEC_OK;
}

static int
mutex_lock(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Mutex.lock", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_OBJECT, "Mutex.lock: receiver must be a Mutex", out);

    if (write_local_slot(vm, (UObject *)self.v.p, "_locked", urbi_make_bool(1)) != 0)
        return urbi_raise_oom(vm, out);

    *out = urbi_make_nil();
    return UEXEC_OK;
}

static int
mutex_unlock(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Mutex.unlock", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_OBJECT, "Mutex.unlock: receiver must be a Mutex", out);

    if (write_local_slot(vm, (UObject *)self.v.p, "_locked", urbi_make_bool(0)) != 0)
        return urbi_raise_oom(vm, out);

    *out = urbi_make_nil();
    return UEXEC_OK;
}

static int
mutex_trylock(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Mutex.tryLock", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_OBJECT, "Mutex.tryLock: receiver must be a Mutex", out);

    UObject *m = (UObject *)self.v.p;
    UValue v;
    if (read_local_slot(vm, m, "_locked", &v) != 0)
        return urbi_raise_oom(vm, out);

    int already = (v.kind == (uint8_t)UVAL_BOOL && v.v.i != 0);
    if (already) {
        *out = urbi_make_bool(0);
        return UEXEC_OK;
    }

    if (write_local_slot(vm, m, "_locked", urbi_make_bool(1)) != 0)
        return urbi_raise_oom(vm, out);

    *out = urbi_make_bool(1);
    return UEXEC_OK;
}

static const UNativeMethodDef MUTEX_METHODS[] = {
    { "new",     mutex_new     },
    { "locked",  mutex_locked  },
    { "lock",    mutex_lock    },
    { "unlock",  mutex_unlock  },
    { "tryLock", mutex_trylock }
};



/* === Date ================================================================
 *
 * Wall-clock access via libc time().  Each Date instance carries a
 * `seconds` slot holding the Unix epoch seconds as a UVAL_INT.  asString
 * formats UTC as "YYYY-MM-DD HH:MM:SS" via gmtime_r + strftime on hosted
 * builds; freestanding builds return "" since neither time() nor
 * strftime are available outside the hosted environment.
 *
 * Phase 10's `.u` overlay can grow Date.toIso8601 / Date.fromString /
 * arithmetic-via-operator surface on top of this primitive. */

static int64_t
host_time_seconds(void)
{
#if defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 1)
    return (int64_t)time(NULL);
#else
    return 0;
#endif
}

static int
date_now(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Date.now", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_OBJECT, "Date.now: receiver must be an Object", out);

    UObject *d = urbi_object_clone(vm, (UObject *)self.v.p);
    if (d == NULL) return urbi_raise_oom(vm, out);

    if (write_local_slot(vm, d, "_seconds", urbi_make_int(host_time_seconds())) != 0)
        return urbi_raise_oom(vm, out);

    *out = urbi_make_object(d);
    return UEXEC_OK;
}

static int
date_from_seconds(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    URBI_CHECK_ARITY(vm, "Date.fromSeconds", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_OBJECT, "Date.fromSeconds: receiver must be an Object", out);
    if (args[0].kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm, "Date.fromSeconds: seconds must be Integer", out);

    UObject *d = urbi_object_clone(vm, (UObject *)self.v.p);
    if (d == NULL) return urbi_raise_oom(vm, out);

    if (write_local_slot(vm, d, "_seconds", args[0]) != 0)
        return urbi_raise_oom(vm, out);

    *out = urbi_make_object(d);
    return UEXEC_OK;
}

static int
date_seconds(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Date.seconds", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_OBJECT, "Date.seconds: receiver must be a Date", out);

    UValue v;
    if (read_local_slot(vm, (UObject *)self.v.p, "_seconds", &v) != 0)
        return urbi_raise_oom(vm, out);
    if (v.kind != (uint8_t)UVAL_INT) {
        *out = urbi_make_int(0);
    } else {
        *out = v;
    }
    return UEXEC_OK;
}

static int
date_as_string(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Date.asString", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_OBJECT, "Date.asString: receiver must be a Date", out);

    UValue v;
    if (read_local_slot(vm, (UObject *)self.v.p, "_seconds", &v) != 0)
        return urbi_raise_oom(vm, out);
    int64_t s = (v.kind == (uint8_t)UVAL_INT) ? v.v.i : 0;

#if defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 1)
    time_t t = (time_t)s;
    struct tm tmv;
    /* gmtime_r is POSIX; on Windows hosts the call should be _gmtime64_s
     * — out of scope for v1.0 (the embedded targets use the freestanding
     * branch and Linux/macOS hosts have gmtime_r). */
    if (gmtime_r(&t, &tmv) == NULL) {
        int oom = 0;
        UValue sv = urbi_val_str_intern(vm, "", 0U, &oom);
        if (oom) return urbi_raise_oom(vm, out);
        *out = sv;
        return UEXEC_OK;
    }
    char buf[32];
    size_t n = strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
    int oom = 0;
    UValue sv = urbi_val_str_intern(vm, buf, n, &oom);
    if (oom) return urbi_raise_oom(vm, out);
    *out = sv;
    return UEXEC_OK;
#else
    (void)s;
    int oom = 0;
    UValue sv = urbi_val_str_intern(vm, "", 0U, &oom);
    if (oom) return urbi_raise_oom(vm, out);
    *out = sv;
    return UEXEC_OK;
#endif
}

/* === Date.plus(Duration) =================================================
 *
 * Phase 9 / Phase 10 seam.  Returns a fresh Date with seconds advanced
 * by the Duration's microseconds-to-seconds quotient (sub-second
 * precision truncated).  Negative Durations subtract.
 *
 * Implementation note: the new Date instance clones vm->date_proto
 * directly (NOT the receiver `self.v.p`).  Cloning a clone-of-proto
 * exposes a chain-of-clone access pattern where slot lookups on the
 * second-generation clone can fail to walk past the user-instance
 * intermediary.  Cloning the proto keeps the chain at depth 1, matching
 * the Date.fromSeconds / Date.now patterns which work correctly at v1.0
 * baseline.  This is tracked in docs/urbi-embedded-design-risks.md as a
 * v1.x chain-of-clone proto-walk audit.
 *
 * Phase 10's `.u` overlay can promote this to the operator form `d + dur`
 * once the arithmetic-operator dispatch for non-atom receivers lands. */

static int
date_plus(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    URBI_CHECK_ARITY(vm, "Date.plus", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_OBJECT, "Date.plus: receiver must be a Date", out);
    if (args[0].kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "Date.plus: argument must be a Duration", out);
    if (vm->date_proto == NULL)
        return urbi_raise_type(vm, "Date.plus: Date proto missing", out);

    UValue base_v;
    if (read_local_slot(vm, (UObject *)self.v.p, "_seconds", &base_v) != 0)
        return urbi_raise_oom(vm, out);
    int64_t base_s = (base_v.kind == (uint8_t)UVAL_INT) ? base_v.v.i : 0;

    UValue dur_v;
    if (read_local_slot(vm, (UObject *)args[0].v.p, "_microseconds", &dur_v) != 0)
        return urbi_raise_oom(vm, out);
    int64_t dur_us = (dur_v.kind == (uint8_t)UVAL_INT) ? dur_v.v.i : 0;
    int64_t dur_s  = dur_us / 1000000;

    UObject *d = urbi_object_clone(vm, vm->date_proto);
    if (d == NULL) return urbi_raise_oom(vm, out);
    if (write_local_slot(vm, d, "_seconds", urbi_make_int(base_s + dur_s)) != 0)
        return urbi_raise_oom(vm, out);

    *out = urbi_make_object(d);
    return UEXEC_OK;
}

static const UNativeMethodDef DATE_METHODS[] = {
    { "now",         date_now           },
    { "fromSeconds", date_from_seconds  },
    { "seconds",     date_seconds       },
    { "asString",    date_as_string     },
    { "plus",        date_plus          }
};



/* === Duration ============================================================
 *
 * Thin wrapper over integer microseconds.  Time literals (100ms / 2s /
 * 1d) lex to integer microseconds at v0.2.0; Duration.fromMicroseconds wraps
 * such an integer in a typed Duration UObject for dispatch.  The backing
 * value lives on a hidden `_microseconds` UVAL_INT slot; named accessors
 * expose conversions to milliseconds / seconds / minutes / hours / days.
 *
 * v1.0 keeps Duration arithmetic plain integer arithmetic on the
 * microseconds value; Phase 10's `.u` overlay can grow Duration.+ /
 * Duration.- as operator overrides on top of this primitive. */

static int
duration_from_micros(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    URBI_CHECK_ARITY(vm, "Duration.fromMicroseconds", 1, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_OBJECT,
                    "Duration.fromMicroseconds: receiver must be an Object", out);
    if (args[0].kind != (uint8_t)UVAL_INT)
        return urbi_raise_type(vm,
            "Duration.fromMicroseconds: argument must be Integer", out);

    UObject *d = urbi_object_clone(vm, (UObject *)self.v.p);
    if (d == NULL) return urbi_raise_oom(vm, out);

    if (write_local_slot(vm, d, "_microseconds", args[0]) != 0)
        return urbi_raise_oom(vm, out);

    *out = urbi_make_object(d);
    return UEXEC_OK;
}

static int
duration_micros(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Duration.asMicroseconds", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_OBJECT,
                    "Duration.asMicroseconds: receiver must be a Duration", out);

    UValue v;
    if (read_local_slot(vm, (UObject *)self.v.p, "_microseconds", &v) != 0)
        return urbi_raise_oom(vm, out);
    *out = (v.kind == (uint8_t)UVAL_INT) ? v : urbi_make_int(0);
    return UEXEC_OK;
}

static int
duration_millis(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Duration.asMilliseconds", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_OBJECT,
                    "Duration.asMilliseconds: receiver must be a Duration", out);

    UValue v;
    if (read_local_slot(vm, (UObject *)self.v.p, "_microseconds", &v) != 0)
        return urbi_raise_oom(vm, out);
    int64_t us = (v.kind == (uint8_t)UVAL_INT) ? v.v.i : 0;
    *out = urbi_make_int(us / 1000);
    return UEXEC_OK;
}

static int
duration_seconds(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    URBI_CHECK_ARITY(vm, "Duration.asSeconds", 0, nargs, out);
    URBI_CHECK_SELF(vm, self, UVAL_OBJECT,
                    "Duration.asSeconds: receiver must be a Duration", out);

    UValue v;
    if (read_local_slot(vm, (UObject *)self.v.p, "_microseconds", &v) != 0)
        return urbi_raise_oom(vm, out);
    int64_t us = (v.kind == (uint8_t)UVAL_INT) ? v.v.i : 0;
    *out = urbi_make_int(us / 1000000);
    return UEXEC_OK;
}

static const UNativeMethodDef DURATION_METHODS[] = {
    { "fromMicroseconds", duration_from_micros },
    { "asMicroseconds",   duration_micros      },
    { "asMilliseconds",   duration_millis      },
    { "asSeconds",        duration_seconds     }
};



/* === urbi_stdlib_register_primitives ====================================
 *
 * Allocates Mutex / Date / Duration proto UObjects per task. */

int
urbi_stdlib_register_primitives(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    int rc;

    /* --- Mutex --- */
    if (vm->mutex_proto == NULL) {
        UObject *p = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
        if (p == NULL) return URBI_ERR_OOM;
        vm->mutex_proto = p;
    }
    rc = URBI_REGISTER_METHODS(vm, vm->mutex_proto, MUTEX_METHODS);
    if (rc != URBI_OK) return rc;
    /* Default the proto's `_locked` slot to false so an un-cloned Mutex
     * also reads as unlocked. */
    rc = install_default_slot(vm, vm->mutex_proto, "_locked", urbi_make_bool(0));
    if (rc != URBI_OK) return rc;

    /* --- Date --- */
    if (vm->date_proto == NULL) {
        UObject *p = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
        if (p == NULL) return URBI_ERR_OOM;
        vm->date_proto = p;
    }
    rc = URBI_REGISTER_METHODS(vm, vm->date_proto, DATE_METHODS);
    if (rc != URBI_OK) return rc;
    /* Default `seconds` slot to 0 so an un-cloned Date proto reads as
     * the Unix epoch. */
    rc = install_default_slot(vm, vm->date_proto, "_seconds", urbi_make_int(0));
    if (rc != URBI_OK) return rc;

    /* --- Duration --- */
    if (vm->duration_proto == NULL) {
        UObject *p = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
        if (p == NULL) return URBI_ERR_OOM;
        vm->duration_proto = p;
    }
    rc = URBI_REGISTER_METHODS(vm, vm->duration_proto, DURATION_METHODS);
    if (rc != URBI_OK) return rc;
    rc = install_default_slot(vm, vm->duration_proto, "_microseconds", urbi_make_int(0));
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
                                   urbi_make_object(vm->mutex_proto));
        if (rc != URBI_OK) return rc;
    }
    if (vm->date_proto != NULL) {
        rc = urbi_realm_set_global(vm, realm, "Date", 4,
                                   urbi_make_object(vm->date_proto));
        if (rc != URBI_OK) return rc;
    }
    if (vm->duration_proto != NULL) {
        rc = urbi_realm_set_global(vm, realm, "Duration", 8,
                                   urbi_make_object(vm->duration_proto));
        if (rc != URBI_OK) return rc;
    }
    return URBI_OK;
}
