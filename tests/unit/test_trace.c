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

UTEST(trace_init_all_off)
{
    UVM vm;
    uint8_t c;
    UTraceStats st;
    urbi_vm_init(&vm, NULL, NULL);
    for (c = 0; c < URBI_TRACE_CHANNEL_MAX; c++)
        UASSERT_EQ(urbi_trace_get_level(&vm, c), URBI_TRACE_OFF);
    urbi_trace_stats(&vm, &st);
    UASSERT_EQ(st.ring_depth, (uint32_t)URBI_TRACE_RING_DEPTH);
    UASSERT_EQ(st.emitted, 0u);
    urbi_vm_destroy(&vm);
}

UTEST(trace_ring_overflow_and_seq)
{
    UVM vm;
    UTraceRecord out[URBI_TRACE_RING_DEPTH];
    uint32_t i, dropped = 0;
    size_t n;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_trace_set_level(&vm, URBI_TRACE_USER, URBI_LOG_DEBUG);

    /* Emit depth+5 records; ring keeps the newest depth, drops 5. */
    for (i = 0; i < (uint32_t)URBI_TRACE_RING_DEPTH + 5u; i++)
        urbi_trace_emit(&vm, URBI_TRACE_USER, URBI_LOG_DEBUG,
                        URBI_TP_USER_MARKER, i, 0);

    n = urbi_trace_snapshot(&vm, out, URBI_TRACE_RING_DEPTH, &dropped);
    UASSERT_EQ(n, (size_t)URBI_TRACE_RING_DEPTH);
    UASSERT_EQ(dropped, 5u);
    /* Oldest surviving record is i==5; seq strictly increases by 1. */
    UASSERT_EQ(out[0].payload.words.a, 5u);
    UASSERT_EQ(out[0].seq + (uint32_t)(URBI_TRACE_RING_DEPTH - 1), out[n - 1].seq);

    /* Snapshot cleared the ring. */
    n = urbi_trace_snapshot(&vm, out, URBI_TRACE_RING_DEPTH, &dropped);
    UASSERT_EQ(n, (size_t)0);
    UASSERT_EQ(dropped, 0u);
    urbi_vm_destroy(&vm);
}

UTEST(trace_channel_and_level_gating)
{
    UVM vm;
    UTraceRecord out[8];
    uint32_t d;
    size_t n;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_trace_set_level(&vm, URBI_TRACE_SCHED, URBI_LOG_INFO);  /* INFO+ only */

    URBI_TP(&vm, URBI_TRACE_SCHED, URBI_LOG_DEBUG, URBI_TP_SCHED_YIELD, 1, 0); /* filtered */
    URBI_TP(&vm, URBI_TRACE_SCHED, URBI_LOG_INFO,  URBI_TP_SCHED_START, 2, 0); /* kept */
    URBI_TP(&vm, URBI_TRACE_GC,    URBI_LOG_ERROR, URBI_TP_GC_PHASE,    3, 0); /* GC OFF */

    n = urbi_trace_snapshot(&vm, out, 8, &d);
    UASSERT_EQ(n, (size_t)1);
    UASSERT_EQ(out[0].schema_id, (uint16_t)URBI_TP_SCHED_START);
    urbi_vm_destroy(&vm);
}

UTEST(trace_threshold_and_first_n)
{
    UVM vm;
    uint32_t iter, i;
    UTraceStats st;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_trace_set_level(&vm, URBI_TRACE_USER, URBI_LOG_DEBUG);

    for (iter = 0; iter < 10; iter++)
        URBI_TP_THRESHOLD(&vm, URBI_TRACE_USER, URBI_LOG_DEBUG, iter, 8,
                          URBI_TP_USER_MARKER, iter, 0);   /* fires iter>=8 ⇒ 2 */
    for (i = 0; i < 10; i++)
        URBI_TP_FIRST_N(&vm, URBI_TRACE_USER, URBI_LOG_DEBUG, 3,
                        URBI_TP_USER_MARKER, i, 0);          /* fires 3 */

    urbi_trace_stats(&vm, &st);
    UASSERT_EQ(st.emitted, 5u);   /* 2 + 3 */
    urbi_vm_destroy(&vm);
}

#endif /* URBI_TRACE */

void test_trace_suite(void)
{
    utest_run("trace_disabled_is_noop", trace_disabled_is_noop);
    utest_run("trace_enums_defined", trace_enums_defined);
#if URBI_TRACE
    utest_run("trace_init_all_off", trace_init_all_off);
    utest_run("trace_ring_overflow_and_seq", trace_ring_overflow_and_seq);
    utest_run("trace_channel_and_level_gating", trace_channel_and_level_gating);
    utest_run("trace_threshold_and_first_n", trace_threshold_and_first_n);
#endif
}
