/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: trace subsystem (v0.11.0).
 *
 * make test       — default build (URBI_TRACE undefined): smoke + no-op only.
 * make test-trace — URBI_TRACE=1: full ring / gating / primitive / tap tests.
 *
 * The URBI_TRACE-gated tests use a stack UVM via urbi_vm_init/urbi_vm_destroy
 * (internal-header pattern, mirrors test_at_scripted_e2e.c). */

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/trace.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* ---- Always-built tests (both URBI_TRACE on and off) ---------------------- */

static int g_side_effect;
static int trace_bump(void) { g_side_effect++; return 7; }

UTEST(trace_disabled_is_noop)
{
    /* vm==NULL: in BOTH build modes the tracepoint must not evaluate its
     * payload args and must not crash. (URBI_TRACE=0: preprocessor-stripped.
     * URBI_TRACE=1: NULL vm ⇒ channel level OFF ⇒ emit short-circuits.) */
    g_side_effect = 0;
    URBI_TP(NULL, URBI_TRACE_SCHED, URBI_LOG_DEBUG, URBI_TP_SCHED_YIELD,
            trace_bump(), 0);
    UASSERT_EQ(g_side_effect, 0);
}

UTEST(trace_enums_defined)
{
    UASSERT_EQ((int)URBI_TRACE_CHANNEL_MAX, 8);
    UASSERT_EQ((int)URBI_TRACE_OFF, -1);
    UASSERT_EQ((int)URBI_TP_USER_MARKER, 18);
}

/* ---- URBI_TRACE-only tests ------------------------------------------------ */

#if URBI_TRACE
/* (added incrementally by Tasks 2-10) */
#endif /* URBI_TRACE */

void test_trace_suite(void)
{
    utest_run("trace_disabled_is_noop", trace_disabled_is_noop);
    utest_run("trace_enums_defined", trace_enums_defined);
}
