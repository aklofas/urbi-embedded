/* SPDX-License-Identifier: BSD-3-Clause */
/* namespaces.c — M6 Phase 8: C-native namespace globals.
 *
 * Math / System / System.Platform / Global / CallMessage — see banner in
 * namespaces.h.  Phase 8 grows incrementally task-by-task: T85 shell,
 * T86 Math constants, T87 System primitives, T88 System.Platform.kind,
 * T90 Global.length, T91 CallMessage stub.
 *
 * Allocation pattern mirrors runtime_types.c (Exception primitive proto):
 * a vanilla URBI_ATOM_OBJECT-family UObject per namespace, methods
 * installed via a per-namespace method table walked by install_methods.
 * GC reachability comes from object_roots_walker (uobject.c) which
 * shades each vm->*_proto field during MARK_ROOTS. */

#include "stdlib/namespaces.h"
#include "stdlib/object_root.h"        /* urbi_native_closure_create + raise helpers */

#include "gc/ugc.h"                    /* urbi_gc_collect */
#include "module/umodule.h"            /* UValue / UVAL_* */
#include "object/uobject.h"            /* urbi_object_alloc + set_local_slot */
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
#include <stdlib.h>                    /* getenv */

/* === UValue construction helpers ========================================= */

static UValue
val_int(int64_t i)
{
    UValue v = urbi_value_nil();
    v.kind = (uint8_t)UVAL_INT;
    v.v.i  = i;
    return v;
}

static UValue
val_float(double d)
{
    UValue v = urbi_value_nil();
    v.kind = (uint8_t)UVAL_FLOAT;
    v.v.f  = d;
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

static UValue
val_str_intern(UVM *vm, const char *s, size_t n, int *oom)
{
    UValue v = urbi_value_nil();
    USymbol *sym = (USymbol *)ustr_intern(vm, s, n);
    if (sym == NULL) {
        if (oom != NULL) *oom = 1;
        return v;
    }
    v.kind = (uint8_t)UVAL_STR;
    v.v.p  = sym;
    return v;
}

/* Install a constant slot (UValue) on proto, looking up the symbol via
 * ustr_intern.  Returns URBI_OK / URBI_ERR_OOM. */
static int
install_const_slot(UVM *vm, UObject *proto, const char *name, UValue value)
{
    USymbol *sym = (USymbol *)ustr_intern(vm, name, urbi_strlen(name));
    if (sym == NULL) return URBI_ERR_OOM;
    if (urbi_object_set_local_slot(vm, proto, sym, value) != 0)
        return URBI_ERR_OOM;
    return URBI_OK;
}

/* === Compile-time platform kind ==========================================
 *
 * Set at compile-time via #ifdef cascade.  The freestanding fallback uses
 * "freertos" because the M0/M1 cross-arm baseline is freestanding-Cortex-
 * M7 with the FreeRTOS BSP target as the canonical embedded host; non-
 * FreeRTOS freestanding hosts can override in a v1.x BSP integration. */

#if defined(__linux__)
#  define URBI_PLATFORM_KIND "linux"
#elif defined(__APPLE__)
#  define URBI_PLATFORM_KIND "darwin"
#elif defined(_WIN32)
#  define URBI_PLATFORM_KIND "windows"
#elif !defined(__STDC_HOSTED__) || (__STDC_HOSTED__ == 0)
#  define URBI_PLATFORM_KIND "freertos"
#else
#  define URBI_PLATFORM_KIND "unknown"
#endif

/* === Method-table install helper ========================================= */

typedef struct {
    const char           *name;
    urbi_native_method_fn fn;
} NsMethodEntry;

static int
install_methods(UVM *vm, UObject *proto,
                const NsMethodEntry *table, size_t count)
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

/* === System.time =========================================================
 *
 * Returns monotonic microseconds since VM start as a Float (seconds).
 * Uses vm->host_time_us — the per-VM monotonic-microseconds hook (default
 * clock_gettime(CLOCK_MONOTONIC) on POSIX hosts, host-supplied BSP shim
 * on freestanding targets per uvm_init.c).
 *
 * Returns 0.0 on freestanding builds whose default_host_time_us_stub
 * returns 0; embedded callers MUST override host_time_us at boot.
 *
 * Semantic note: legacy urbi 2.x System.time returned wall-clock seconds-
 * since-epoch.  v1.0 narrows to the per-VM monotonic clock to avoid a
 * libc time() dependency on freestanding targets.  Wall-clock access
 * lands later via the Date primitive (Phase 9). */

static int
sys_time(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "System.time", 0, nargs, out);
    uint64_t us = (vm->host_time_us != NULL) ? vm->host_time_us() : 0U;
    *out = val_float((double)us / 1000000.0);
    return UEXEC_OK;
}

/* === System.cycle ========================================================
 *
 * Returns the per-VM monotonic lookup-id counter as an Integer.  This
 * counter increments on every prototype-chain walk, so it grows with VM
 * activity — a coarse "cycle count" useful for monotonicity assertions
 * in test fixtures.  Not a CPU cycle counter (which would require
 * platform-specific hooks). */

static int
sys_cycle(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "System.cycle", 0, nargs, out);
    *out = val_int((int64_t)vm->lookup_id);
    return UEXEC_OK;
}

/* === System.getenv(name) =================================================
 *
 * Hosted: libc getenv() shim.  Returns the value as a UVAL_STR (interned)
 * or nil if the variable is unset.
 *
 * Freestanding: always returns nil.  No libc getenv on freestanding
 * targets, and an embedded BSP would expose configuration through a
 * different surface. */

static int
sys_getenv(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self;
    if (nargs != 1) return urbi_raise_arity(vm, "System.getenv", 1, nargs, out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "System.getenv: name must be String", out);

#if defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 1)
    /* UVAL_STR.v.p is the NUL-terminated `const char *` returned by
     * ustr_intern (per atoms.c §"String basic methods" banner). */
    const char *name = (const char *)args[0].v.p;
    if (name == NULL) return urbi_raise_type(vm, "System.getenv: NULL String", out);
    const char *v = getenv(name);
    if (v == NULL) {
        *out = urbi_value_nil();
        return UEXEC_OK;
    }
    int oom = 0;
    *out = val_str_intern(vm, v, urbi_strlen(v), &oom);
    if (oom) return urbi_raise_oom(vm, out);
    return UEXEC_OK;
#else
    (void)args;
    *out = urbi_value_nil();
    return UEXEC_OK;
#endif
}

/* === System.gc ===========================================================
 *
 * Force a full GC pass.  Returns nil. */

static int
sys_gc(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "System.gc", 0, nargs, out);
    urbi_gc_collect(vm);
    *out = urbi_value_nil();
    return UEXEC_OK;
}

static const NsMethodEntry SYSTEM_METHODS[] = {
    { "time",   sys_time   },
    { "cycle",  sys_cycle  },
    { "getenv", sys_getenv },
    { "gc",     sys_gc     }
};

#define SYSTEM_METHODS_COUNT (sizeof(SYSTEM_METHODS) / sizeof(SYSTEM_METHODS[0]))

/* === urbi_stdlib_register_namespaces ====================================
 *
 * Allocates Math / System / Global / CallMessage proto UObjects per task.
 * T86: Math with pi / e / nan / infinity constants.  GC reachability via
 * object_roots_walker shading vm->math_proto.
 *
 * Idempotent: re-allocates each proto only when its vm field is NULL. */

int
urbi_stdlib_register_namespaces(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    int rc;

    /* --- T86 Math: pi / e / nan / infinity --- */
    if (vm->math_proto == NULL) {
        UObject *m = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
        if (m == NULL) return URBI_ERR_OOM;
        vm->math_proto = m;
    }
    rc = install_const_slot(vm, vm->math_proto, "pi",       val_float(3.141592653589793));
    if (rc != URBI_OK) return rc;
    rc = install_const_slot(vm, vm->math_proto, "e",        val_float(2.718281828459045));
    if (rc != URBI_OK) return rc;
    /* IEEE-754 NaN / +Inf via 0.0/0.0 / 1.0/0.0.  Compilers fold these at
     * compile time per IEEE arithmetic; if a target's compiler refuses,
     * switch to (double)NAN / (double)INFINITY from <math.h>. */
    rc = install_const_slot(vm, vm->math_proto, "nan",      val_float(0.0 / 0.0));
    if (rc != URBI_OK) return rc;
    rc = install_const_slot(vm, vm->math_proto, "infinity", val_float(1.0 / 0.0));
    if (rc != URBI_OK) return rc;

    /* --- T87 System: time / cycle / getenv / gc --- */
    if (vm->system_proto == NULL) {
        UObject *s = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
        if (s == NULL) return URBI_ERR_OOM;
        vm->system_proto = s;
    }
    rc = install_methods(vm, vm->system_proto, SYSTEM_METHODS, SYSTEM_METHODS_COUNT);
    if (rc != URBI_OK) return rc;

    /* --- T88 System.Platform: kind constant ---
     *
     * Platform is nested as a slot on System (System.Platform.kind) — not a
     * top-level realm global.  The proto is allocated as a sibling singleton
     * and shaded directly by the GC walker for uniformity even though the
     * System slot already keeps it reachable transitively. */
    if (vm->platform_proto == NULL) {
        UObject *p = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
        if (p == NULL) return URBI_ERR_OOM;
        vm->platform_proto = p;
    }
    {
        int oom = 0;
        UValue kind = val_str_intern(vm, URBI_PLATFORM_KIND,
                                     urbi_strlen(URBI_PLATFORM_KIND), &oom);
        if (oom) return URBI_ERR_OOM;
        rc = install_const_slot(vm, vm->platform_proto, "kind", kind);
        if (rc != URBI_OK) return rc;
    }
    rc = install_const_slot(vm, vm->system_proto, "Platform",
                            val_obj(vm->platform_proto));
    if (rc != URBI_OK) return rc;

    return URBI_OK;
}

/* === urbi_stdlib_register_namespace_globals =============================
 *
 * Post-registry hook: bind namespaces as realm globals on `realm`.  Lands
 * at slots 15+, past the v1.0 packed-flag CONSTANT enforcement range
 * (slots 0..7).  Mirrors urbi_stdlib_register_runtime_globals. */

int
urbi_stdlib_register_namespace_globals(UVM *vm, URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;

    int rc;
    if (vm->math_proto != NULL) {
        rc = urbi_realm_set_global(vm, realm, "Math", 4, val_obj(vm->math_proto));
        if (rc != URBI_OK) return rc;
    }
    if (vm->system_proto != NULL) {
        rc = urbi_realm_set_global(vm, realm, "System", 6, val_obj(vm->system_proto));
        if (rc != URBI_OK) return rc;
    }
    return URBI_OK;
}
