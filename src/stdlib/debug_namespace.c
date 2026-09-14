/* SPDX-License-Identifier: BSD-3-Clause */
/* debug_namespace.c — see stdlib/debug_namespace.h. */

#include "stdlib/debug_namespace.h"

#include <stdarg.h>
#include <stdio.h>    /* vsnprintf — src/stdlib is hosted, unlike src/rt */

/* One appender with an overflow latch.  `at` is the bytes written so far;
 * once a write would not fit, `*ok` goes false and every later call is a
 * no-op, so the caller checks once at the end instead of at each step. */
static size_t dbg_add(char *buf, size_t cap, size_t at, bool *ok, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

static size_t dbg_add(char *buf, size_t cap, size_t at, bool *ok, const char *fmt, ...)
{
    if (!*ok) return at;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + at, cap - at, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - at) { *ok = false; return at; }
    return at + (size_t)n;
}

/* The four states a strand can be in, plus the two gates that suspend a
 * READY one.  The gate wins in the report because it is the reason the
 * strand is not running. */
static const char *dbg_state_name(const UStrand *s)
{
    if (s->gates & USTRAND_GATE_FROZEN)  return "frozen";
    if (s->gates & USTRAND_GATE_BLOCKED) return "blocked";
    switch (s->state) {
    case USTRAND_READY:   return "ready";
    case USTRAND_RUNNING: return "running";
    case USTRAND_PARKED:  return "parked";
    default:              return "dead";
    }
}

int urbi_introspect_coros(UVM *vm, char *buf, size_t cap, size_t *out_n)
{
    if (!vm || !buf || cap == 0 || !out_n) return URBI_ERR_INVALID_ARG;
    *out_n = 0;
    buf[0] = '\0';

    bool ok = true;
    size_t at = dbg_add(buf, cap, 0, &ok, "{\"coros\":[");
    unsigned realm_index = 0;
    bool first = true;
    for (URealm *r = vm->realms; r; r = r->next, realm_index++) {
        for (UStrand *s = r->strands; s; s = s->next_in_realm) {
            at = dbg_add(buf, cap, at, &ok, "%s{\"id\":%u,\"state\":\"%s\",\"realm\":%u}",
                         first ? "" : ",", (unsigned)s->id, dbg_state_name(s), realm_index);
            first = false;
        }
    }
    at = dbg_add(buf, cap, at, &ok, "]}");
    if (!ok) { buf[0] = '\0'; return URBI_ERR_INVALID_ARG; }
    *out_n = at;
    return URBI_OK;
}

/* Debug.coros() -> String.  The same bytes the NDJSON introspect op
 * returns, handed to script as a String so a client can print it or
 * feed it to whatever JSON reader it already has. */
static int debug_coros(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)args; (void)nargs;
    char json[4096];
    size_t n = 0;
    if (urbi_introspect_coros(vm, json, sizeof json, &n) != URBI_OK)
        return urbi_raise_range(vm, "Debug.coros: too many strands to report", out);
    UValue v = urbi_make_str(vm, json, n);
    if (v.kind == UV_NIL) return urbi_raise_oom(vm, out);
    *out = v;
    return UEXEC_OK;
}

const UMethodDef ustdlib_debug_methods[USTDLIB_DEBUG_NMETHODS] = {
    { "coros", debug_coros, 0, 0 }
};
