/* SPDX-License-Identifier: BSD-3-Clause */
/* debug_namespace.c — see stdlib/debug_namespace.h. */

#include "stdlib/debug_namespace.h"

#if __STDC_HOSTED__
#  include <stdio.h>    /* snprintf — the only hosted call in this file */
#endif
#include <string.h>

#if __STDC_HOSTED__
/* One appender with an overflow latch.  `at` is the bytes written so far;
 * once a write would not fit, `*ok` goes false and every later call is a
 * no-op, so the caller checks once at the end instead of at each step.
 *
 * Not variadic: each caller formats into its own scratch first.  A
 * va_list threaded through a helper is the shape the static analyser
 * cannot follow, and there are two call sites. */
static size_t dbg_add(char *buf, size_t cap, size_t at, bool *ok, const char *s)
{
    if (!*ok) return at;
    size_t n = strlen(s);
    if (at + n + 1u > cap) { *ok = false; return at; }
    memcpy(buf + at, s, n + 1u);
    return at + n;
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
#endif

int urbi_introspect_coros(UVM *vm, char *buf, size_t cap, size_t *out_n)
{
    if (!vm || !buf || cap == 0 || !out_n) return URBI_ERR_INVALID_ARG;
    *out_n = 0;
    buf[0] = '\0';
#if __STDC_HOSTED__
    bool ok = true;
    size_t at = dbg_add(buf, cap, 0, &ok, "{\"coros\":[");
    unsigned realm_index = 0;
    bool first = true;
    for (URealm *r = vm->realms; r; r = r->next, realm_index++) {
        for (UStrand *s = r->strands; s; s = s->next_in_realm) {
            char entry[128];
            int n = snprintf(entry, sizeof entry,
                             "%s{\"id\":%u,\"state\":\"%s\",\"realm\":%u}",
                             first ? "" : ",", (unsigned)s->id,
                             dbg_state_name(s), realm_index);
            if (n < 0 || (size_t)n >= sizeof entry) { ok = false; break; }
            at = dbg_add(buf, cap, at, &ok, entry);
            first = false;
        }
    }
    at = dbg_add(buf, cap, at, &ok, "]}");
    if (!ok) { buf[0] = '\0'; return URBI_ERR_INVALID_ARG; }
    *out_n = at;
    return URBI_OK;
#else
    /* No formatter without a hosted libc: the Debug namespace reports an
     * empty listing rather than linking snprintf into a freestanding
     * archive. */
    (void)vm;
    return URBI_ERR_INVALID_ARG;
#endif
}

/* Debug.coros() -> String.  The same bytes the NDJSON introspect op
 * returns, handed to script as a String so a client can print it or
 * feed it to whatever JSON reader it already has. */
static int debug_coros(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)args; (void)nargs;
    char json[4096];
    size_t n = 0;
    /* cppcheck evaluates an undefined __STDC_HOSTED__ as 0 in #if, so it
     * only sees the freestanding branch of urbi_introspect_coros (always
     * URBI_ERR_INVALID_ARG) and calls this check always-true; the hosted
     * branch, which real hosted builds take, can return URBI_OK. */
    /* cppcheck-suppress knownConditionTrueFalse */
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
