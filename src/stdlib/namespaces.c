/* SPDX-License-Identifier: BSD-3-Clause */
/* namespaces.c — C-native namespace globals.
 *
 * Math / System / System.Platform / Global / CallMessage — see banner in
 * namespaces.h.
 *
 * Allocation pattern mirrors runtime_types.c (Exception primitive proto):
 * a vanilla URBI_ATOM_OBJECT-family UObject per namespace, methods
 * installed via UNativeMethodDef tables with URBI_REGISTER_METHODS.
 * GC reachability comes from object_roots_walker (uobject.c) which
 * shades each vm->*_proto field during MARK_ROOTS. */

#include "rt/ustdlib_glue.h"
#include "stdlib/namespaces.h"

#if __STDC_HOSTED__
#  include <math.h>     /* NAN / INFINITY */
#  include <stdlib.h>   /* getenv */
#endif

/* Install a constant slot (UValue) on proto, looking up the symbol via
 * ustr_intern.  Returns URBI_OK / URBI_ERR_OOM. */
static int
install_const_slot(UVM *vm, UObject *proto, const char *name, UValue value)
{
    USym *sym = usym_cstr(vm, name);
    if (sym == NULL || proto == NULL) return URBI_ERR_OOM;
    if (uobj_set_local(vm, proto, sym, value, USLOT_CONSTANT) < 0) return URBI_ERR_OOM;
    return URBI_OK;
}

/* === Compile-time platform kind ==========================================
 *
 * Set at compile-time via #ifdef cascade.  The freestanding fallback uses
 * "freertos" because the cross-arm baseline is a freestanding Cortex-M7
 * with the FreeRTOS BSP target as the canonical embedded host; non-
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

/* Method tables use UNativeMethodDef from stdlib/object_root.h;
 * URBI_REGISTER_METHODS does the install loop. */

/* === System.time =========================================================
 *
 * Returns monotonic microseconds since VM start as a Float (seconds).
 * Uses vm->host_time_us — the per-VM monotonic-microseconds hook (default
 * clock_gettime(CLOCK_MONOTONIC) on POSIX hosts, host-supplied BSP shim
 * on freestanding targets per uvm_init.c).
 *
 * Returns 0.0 on freestanding builds whose default_host_time_us_stub
 * returns 0; embedded callers MUST override host_time_us at boot.
 */

static int
sys_time(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "System.time", 0, nargs, out);
    uint64_t us = vm->clock_us ? vm->clock_us(vm->clock_ud) : 0U;
    *out = uv_float((double)us / 1000000.0);
    return UEXEC_OK;
}

/* === System.time_us ======================================================
 *
 * Returns monotonic microseconds since VM start as an Integer — the raw
 * vm->host_time_us reading with no scaling.  Companion to System.time
 * (which returns the same reading scaled to seconds as Float).
 *
 * Use when sub-millisecond precision matters (control loops, frame-timing
 * deltas, deadline arithmetic) or when integer arithmetic is preferred
 * over Float for ordering / monotonicity checks. */

static int
sys_time_us(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "System.time_us", 0, nargs, out);
    uint64_t us = vm->clock_us ? vm->clock_us(vm->clock_ud) : 0U;
    *out = uv_int((int64_t)us);
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
    *out = uv_int((int64_t)vm->objstats.visit_stamp);
    return UEXEC_OK;
}

/* === System.getenv(name) =================================================
 *
 * Hosted: libc getenv() shim.  Returns the value as a UV_STR (interned)
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
    if (!urbi_is_str(args[0]))
        return urbi_raise_type(vm, "System.getenv: name must be String", out);

#if defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 1)
    /* UV_STR.v.p is the NUL-terminated `const char *` returned by
     * ustr_intern (per atoms.c §"String basic methods" banner). */
    const char *v = getenv(urbi_str_cstr(args[0]));
    if (v == NULL) { *out = uv_nil(); return UEXEC_OK; }
    UValue sv = urbi_make_str(vm, v, urbi_strlen(v));
    if (sv.kind == UV_NIL) return urbi_raise_oom(vm, out);
    *out = sv;
    return UEXEC_OK;
#else
    (void)args;
    *out = uv_nil();
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
    *out = uv_nil();
    return UEXEC_OK;
}

/* === Global.length =======================================================
 *
 * Returns the number of slots currently installed on the active realm's
 * global_object as an Integer.  Reflective access surface — at v1.0 the
 * count is the only slot exposed; v1.x can grow Global.names() / .at()
 * etc. via the same hook once String/List shapes solidify. */

static int
global_length(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Global.length", 0, nargs, out);

    /* Everything the realm resolves by bare name: its own slots plus the
     * shared built-ins it inherits.  Counting only the realm's own would
     * report 1 (the Realm self-reference), which is not what "the
     * bindings in scope" means to a script. */
    URealm *r = uvm_current_realm(vm);
    int64_t n = (r && r->globals) ? (int64_t)r->globals->count : 0;
    if (vm->root_globals) n += (int64_t)vm->root_globals->count;
    *out = uv_int(n);
    return UEXEC_OK;
}

/* === the tables ==========================================================
 *
 * Math carries constants and no methods, so it has no table at all: its
 * whole content is installed by urbi_namespaces_init below. */

const UMethodDef k_system_methods[K_SYSTEM_NMETHODS] = {
    { "time",    sys_time,    0, 0 },
    { "time_us", sys_time_us, 0, 0 },
    { "cycle",   sys_cycle,   0, 0 },
    { "getenv",  sys_getenv,  1, 1 },
    { "gc",      sys_gc,      0, 0 }
};

const UMethodDef k_global_methods[K_GLOBAL_NMETHODS] = {
    { "length", global_length, 0, 0 }
};

/* === constants ===========================================================
 *
 * The boot table describes protos and methods; a handful of built-ins
 * also need constant SLOTS, and this is where those go.  Math is all
 * constants; System owns the nested Platform object, which is a slot on
 * System rather than a global of its own. */

int urbi_namespaces_init(UVM *vm)
{
    int rc = install_const_slot(vm, vm->protos[UP_MATH], "pi", uv_float(3.141592653589793));
    if (rc != URBI_OK) return rc;
    rc = install_const_slot(vm, vm->protos[UP_MATH], "e", uv_float(2.718281828459045));
    if (rc != URBI_OK) return rc;
#if __STDC_HOSTED__
    /* IEEE-754 sentinels come from <math.h>; a freestanding target has no
     * libm contract and code that needs them builds them from bits. */
    rc = install_const_slot(vm, vm->protos[UP_MATH], "nan", uv_float((double)NAN));
    if (rc != URBI_OK) return rc;
    rc = install_const_slot(vm, vm->protos[UP_MATH], "infinity", uv_float((double)INFINITY));
    if (rc != URBI_OK) return rc;
#endif

    UObject *platform = uobj_new(vm, vm->protos[UP_OBJECT]);
    if (platform == NULL) return URBI_ERR_OOM;
    UValue pv = uv_obj(platform);
    URBI_ROOT(vm, pv);
    UValue kind = urbi_make_str_interned(vm, URBI_PLATFORM_KIND, urbi_strlen(URBI_PLATFORM_KIND));
    rc = (kind.kind == UV_NIL) ? URBI_ERR_OOM : install_const_slot(vm, platform, "kind", kind);
    URBI_UNROOT(vm, pv);
    if (rc != URBI_OK) return rc;
    return install_const_slot(vm, vm->protos[UP_SYSTEM], "Platform", pv);
}
