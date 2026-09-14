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

#include "rt/ustdlib_glue.h"
#include "stdlib/primitives.h"

#if defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 1)
#  include <time.h>
#endif

/* Install a default slot on a prototype.  URBI_OK / URBI_ERR_OOM. */
static int
install_default_slot(UVM *vm, UObject *proto, const char *name, UValue value)
{
    USym *sym = usym_cstr(vm, name);
    if (sym == NULL || proto == NULL) return URBI_ERR_OOM;
    if (uobj_set_local(vm, proto, sym, value, 0) < 0) return URBI_ERR_OOM;
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
    const USym *sym = usym_cstr(vm, name);
    UObjSlotRef ref;
    if (sym == NULL) return -1;
    /* Resolve THROUGH the chain: instance state lives on the clone, but
     * the prototype carries the default, so an un-cloned receiver still
     * reads a sane value. */
    if (!uobj_resolve(vm, o, sym, &ref)) { *out = uv_nil(); return 0; }
    *out = uobj_slot_value(&ref);
    return 0;
}

static int
write_local_slot(UVM *vm, UObject *o, const char *name, UValue value)
{
    USym *sym = usym_cstr(vm, name);
    if (sym == NULL) return -1;
    return uobj_set_local(vm, o, sym, value, 0) < 0 ? -1 : 0;
}

/* === Mutex ===============================================================
 *
 * Single-VM cooperative-only contract: lock/unlock/tryLock are
 * non-blocking flag flips on a hidden `_locked` UV_BOOL slot of the
 * instance UObject.  A later `.u` overlay may grow Mutex.synchronized
 * via `waituntil m.locked() == false` for cooperative wait semantics. */

static int
mutex_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    (void)args;
    if (self.kind != UV_OBJ) return urbi_raise_type(vm, "Mutex.new: receiver must be an Object", out);

    UObject *m = urbi_object_clone(vm, (UObject *)self.v.p);
    if (m == NULL) return urbi_raise_oom(vm, out);

    if (write_local_slot(vm, m, "_locked", uv_bool(0)) != 0)
        return urbi_raise_oom(vm, out);

    *out = uv_obj(m);
    return UEXEC_OK;
}

static int
mutex_locked(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    (void)args;
    if (self.kind != UV_OBJ) return urbi_raise_type(vm, "Mutex.locked: receiver must be a Mutex", out);

    UValue v;
    if (read_local_slot(vm, (UObject *)self.v.p, "_locked", &v) != 0)
        return urbi_raise_oom(vm, out);
    /* Coerce UV_NIL (proto unread) to false. */
    if (v.kind == UV_BOOL) {
        *out = v;
    } else {
        *out = uv_bool(0);
    }
    return UEXEC_OK;
}

static int
mutex_lock(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    (void)args;
    if (self.kind != UV_OBJ) return urbi_raise_type(vm, "Mutex.lock: receiver must be a Mutex", out);

    if (write_local_slot(vm, (UObject *)self.v.p, "_locked", uv_bool(1)) != 0)
        return urbi_raise_oom(vm, out);

    *out = uv_nil();
    return UEXEC_OK;
}

static int
mutex_unlock(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    (void)args;
    if (self.kind != UV_OBJ) return urbi_raise_type(vm, "Mutex.unlock: receiver must be a Mutex", out);

    if (write_local_slot(vm, (UObject *)self.v.p, "_locked", uv_bool(0)) != 0)
        return urbi_raise_oom(vm, out);

    *out = uv_nil();
    return UEXEC_OK;
}

static int
mutex_trylock(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    (void)args;
    if (self.kind != UV_OBJ) return urbi_raise_type(vm, "Mutex.tryLock: receiver must be a Mutex", out);

    UObject *m = (UObject *)self.v.p;
    UValue v;
    if (read_local_slot(vm, m, "_locked", &v) != 0)
        return urbi_raise_oom(vm, out);

    int already = (v.kind == UV_BOOL && v.v.i != 0);
    if (already) {
        *out = uv_bool(0);
        return UEXEC_OK;
    }

    if (write_local_slot(vm, m, "_locked", uv_bool(1)) != 0)
        return urbi_raise_oom(vm, out);

    *out = uv_bool(1);
    return UEXEC_OK;
}


/* === Date ================================================================
 *
 * Wall-clock access via libc time().  Each Date instance carries a
 * `seconds` slot holding the Unix epoch seconds as a UV_INT.  asString
 * formats UTC as "YYYY-MM-DD HH:MM:SS" via gmtime_r + strftime on hosted
 * builds; freestanding builds return "" since neither time() nor
 * strftime are available outside the hosted environment.
 */

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
    (void)nargs;
    (void)args;
    if (self.kind != UV_OBJ) return urbi_raise_type(vm, "Date.now: receiver must be an Object", out);

    UObject *d = urbi_object_clone(vm, (UObject *)self.v.p);
    if (d == NULL) return urbi_raise_oom(vm, out);

    if (write_local_slot(vm, d, "_seconds", uv_int(host_time_seconds())) != 0)
        return urbi_raise_oom(vm, out);

    *out = uv_obj(d);
    return UEXEC_OK;
}

static int
date_from_seconds(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (self.kind != UV_OBJ) return urbi_raise_type(vm, "Date.fromSeconds: receiver must be an Object", out);
    if (args[0].kind != UV_INT)
        return urbi_raise_type(vm, "Date.fromSeconds: seconds must be Integer", out);

    UObject *d = urbi_object_clone(vm, (UObject *)self.v.p);
    if (d == NULL) return urbi_raise_oom(vm, out);

    if (write_local_slot(vm, d, "_seconds", args[0]) != 0)
        return urbi_raise_oom(vm, out);

    *out = uv_obj(d);
    return UEXEC_OK;
}

static int
date_seconds(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    (void)args;
    if (self.kind != UV_OBJ) return urbi_raise_type(vm, "Date.seconds: receiver must be a Date", out);

    UValue v;
    if (read_local_slot(vm, (UObject *)self.v.p, "_seconds", &v) != 0)
        return urbi_raise_oom(vm, out);
    if (v.kind != UV_INT) {
        *out = uv_int(0);
    } else {
        *out = v;
    }
    return UEXEC_OK;
}

static int
date_as_string(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    (void)args;
    if (self.kind != UV_OBJ) return urbi_raise_type(vm, "Date.asString: receiver must be a Date", out);

    UValue v;
    if (read_local_slot(vm, (UObject *)self.v.p, "_seconds", &v) != 0)
        return urbi_raise_oom(vm, out);
    int64_t s = (v.kind == UV_INT) ? v.v.i : 0;

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
 */

static int
date_plus(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (self.kind != UV_OBJ) return urbi_raise_type(vm, "Date.plus: receiver must be a Date", out);
    if (args[0].kind != UV_OBJ)
        return urbi_raise_type(vm, "Date.plus: argument must be a Duration", out);

    UValue base_v;
    if (read_local_slot(vm, (UObject *)self.v.p, "_seconds", &base_v) != 0)
        return urbi_raise_oom(vm, out);
    int64_t base_s = (base_v.kind == UV_INT) ? base_v.v.i : 0;

    UValue dur_v;
    if (read_local_slot(vm, (UObject *)args[0].v.p, "_microseconds", &dur_v) != 0)
        return urbi_raise_oom(vm, out);
    int64_t dur_us = (dur_v.kind == UV_INT) ? dur_v.v.i : 0;
    int64_t dur_s  = dur_us / 1000000;

    UObject *d = uobj_new(vm, vm->protos[UP_DATE]);
    if (d == NULL) return urbi_raise_oom(vm, out);
    if (write_local_slot(vm, d, "_seconds", uv_int(base_s + dur_s)) != 0)
        return urbi_raise_oom(vm, out);

    *out = uv_obj(d);
    return UEXEC_OK;
}


/* === Duration ============================================================
 */

static int
duration_from_micros(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    if (self.kind != UV_OBJ) return urbi_raise_type(vm, "Duration.fromMicroseconds: receiver must be an Object", out);
    if (args[0].kind != UV_INT)
        return urbi_raise_type(vm,
            "Duration.fromMicroseconds: argument must be Integer", out);

    UObject *d = urbi_object_clone(vm, (UObject *)self.v.p);
    if (d == NULL) return urbi_raise_oom(vm, out);

    if (write_local_slot(vm, d, "_microseconds", args[0]) != 0)
        return urbi_raise_oom(vm, out);

    *out = uv_obj(d);
    return UEXEC_OK;
}

static int
duration_micros(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    (void)args;
    if (self.kind != UV_OBJ) return urbi_raise_type(vm, "Duration.asMicroseconds: receiver must be a Duration", out);

    UValue v;
    if (read_local_slot(vm, (UObject *)self.v.p, "_microseconds", &v) != 0)
        return urbi_raise_oom(vm, out);
    *out = (v.kind == UV_INT) ? v : uv_int(0);
    return UEXEC_OK;
}

static int
duration_millis(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    (void)args;
    if (self.kind != UV_OBJ) return urbi_raise_type(vm, "Duration.asMilliseconds: receiver must be a Duration", out);

    UValue v;
    if (read_local_slot(vm, (UObject *)self.v.p, "_microseconds", &v) != 0)
        return urbi_raise_oom(vm, out);
    int64_t us = (v.kind == UV_INT) ? v.v.i : 0;
    *out = uv_int(us / 1000);
    return UEXEC_OK;
}

static int
duration_seconds(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)nargs;
    (void)args;
    if (self.kind != UV_OBJ) return urbi_raise_type(vm, "Duration.asSeconds: receiver must be a Duration", out);

    UValue v;
    if (read_local_slot(vm, (UObject *)self.v.p, "_microseconds", &v) != 0)
        return urbi_raise_oom(vm, out);
    int64_t us = (v.kind == UV_INT) ? v.v.i : 0;
    *out = uv_int(us / 1000000);
    return UEXEC_OK;
}

/* === the tables ========================================================== */

const UMethodDef k_mutex_methods[K_MUTEX_NMETHODS] = {
    { "new",     mutex_new,     0, 0 },
    { "locked",  mutex_locked,  0, 0 },
    { "lock",    mutex_lock,    0, 0 },
    { "unlock",  mutex_unlock,  0, 0 },
    { "tryLock", mutex_trylock, 0, 0 }
};
const UMethodDef k_date_methods[K_DATE_NMETHODS] = {
    { "now",         date_now,          0, 0 },
    { "fromSeconds", date_from_seconds, 1, 1 },
    { "seconds",     date_seconds,      0, 0 },
    { "asString",    date_as_string,    0, 0 },
    { "plus",        date_plus,         1, 1 }
};
const UMethodDef k_duration_methods[K_DURATION_NMETHODS] = {
    { "fromMicroseconds", duration_from_micros, 1, 1 },
    { "asMicroseconds",   duration_micros,      0, 0 },
    { "asMilliseconds",   duration_millis,      0, 0 },
    { "asSeconds",        duration_seconds,     0, 0 }
};


/* Each prototype carries the default its instances shadow, so reading
 * state off an un-cloned Mutex / Date / Duration answers sensibly
 * instead of failing to resolve. */
int urbi_primitives_init(UVM *vm)
{
    int rc = install_default_slot(vm, vm->protos[UP_MUTEX], "_locked", uv_bool(0));
    if (rc != URBI_OK) return rc;
    rc = install_default_slot(vm, vm->protos[UP_DATE], "_seconds", uv_int(0));
    if (rc != URBI_OK) return rc;
    return install_default_slot(vm, vm->protos[UP_DURATION], "_microseconds", uv_int(0));
}
