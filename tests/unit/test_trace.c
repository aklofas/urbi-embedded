/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: trace subsystem (v0.11.0).
 *
 * make test       — default build (URBI_TRACE undefined): smoke + no-op only.
 * make test-trace — URBI_TRACE=1: full ring / gating / primitive / tap tests.
 *
 * The URBI_TRACE-gated tests use a stack UVM via urbi_vm_init/urbi_vm_destroy
 * (internal-header pattern, mirrors test_at_scripted_e2e.c). */

#include "utest.h"
#include "utest_e2e_helpers.h"
#include "urbi/urbi.h"
#include "urbi/trace.h"
#include "vm/uvm.h"
#include "realm/urealm.h"

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
    (void)&trace_bump;  /* referenced even when URBI_TP strips the call (OFF build) */
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

UTEST(trace_format_record)
{
    UTraceRecord r;
    char buf[128];
    size_t n;
    memset(&r, 0, sizeof r);
    r.ts_us = 1234567; r.seq = 42; r.strand_id = 3;
    r.channel = URBI_TRACE_SCHED; r.level = URBI_LOG_INFO;
    r.schema_id = URBI_TP_SCHED_BLOCK; r.payload.words.a = 6; r.payload.words.b = 0;

    n = utrace_format(buf, sizeof buf, &r);
    UASSERT(n > 0 && n < sizeof buf);
    UASSERT(strstr(buf, "seq=42") != NULL);
    UASSERT(strstr(buf, "sched/INFO") != NULL);
    UASSERT(strstr(buf, "sched_block") != NULL);
    UASSERT(strstr(buf, "a=6") != NULL);
}

UTEST(trace_format_marker_string)
{
    UTraceRecord r;
    char buf[128];
    memset(&r, 0, sizeof r);
    r.channel = URBI_TRACE_USER; r.level = URBI_LOG_INFO;
    r.schema_id = URBI_TP_USER_MARKER;
    /* bounded 8-byte copy semantics: "phase1\0\0" */
    r.payload.str[0]='p'; r.payload.str[1]='h'; r.payload.str[2]='a';
    r.payload.str[3]='s'; r.payload.str[4]='e'; r.payload.str[5]='1';
    (void)utrace_format(buf, sizeof buf, &r);
    UASSERT(strstr(buf, "user_marker \"phase1\"") != NULL);
}

/* Scan a drained record set for a given schema id. */
static int trace_saw_schema(const UTraceRecord *recs, size_t n, uint16_t schema)
{
    size_t i;
    for (i = 0; i < n; i++) if (recs[i].schema_id == schema) return 1;
    return 0;
}

UTEST(trace_sched_records_on_fork)
{
    UVM vm;
    UTraceRecord out[128];
    uint32_t d;
    size_t n;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_trace_set_level(&vm, URBI_TRACE_SCHED, URBI_LOG_DEBUG);
    /* Chunk-top comma fork spawns a child strand (v0.8.0 loader-strand). */
    utest_e2e_compile_and_run(&vm, "1 , 2", NULL);
    utest_e2e_run_to_no_runnable(&vm);

    n = urbi_trace_snapshot(&vm, out, 128, &d);
    UASSERT(trace_saw_schema(out, n, URBI_TP_SCHED_START));
    UASSERT(trace_saw_schema(out, n, URBI_TP_SCHED_EXIT));
    urbi_vm_destroy(&vm);
}

UTEST(trace_gc_phase_transitions)
{
    UVM vm;
    UTraceRecord out[256];
    uint32_t d;
    size_t n, i;
    int phases = 0;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_trace_set_level(&vm, URBI_TRACE_GC, URBI_LOG_DEBUG);
    /* Allocate some cells, then force a full GC cycle to completion. */
    utest_e2e_compile_and_run(&vm, "1 + 2", NULL);
    urbi_gc_force_full(&vm);
    n = urbi_trace_snapshot(&vm, out, 256, &d);
    for (i = 0; i < n; i++) if (out[i].schema_id == URBI_TP_GC_PHASE) phases++;
    UASSERT(phases >= 2);   /* at least MARK_ROOTS and IDLE */
    urbi_vm_destroy(&vm);
}

UTEST(trace_event_emit_and_watcher_fire)
{
    UVM vm;
    UTraceRecord out[128];
    uint32_t d;
    size_t n;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_trace_set_level(&vm, URBI_TRACE_EVENT, URBI_LOG_DEBUG);
    urbi_trace_set_level(&vm, URBI_TRACE_WATCHER, URBI_LOG_DEBUG);
    utest_e2e_compile_and_run(&vm,
        "var e = Event.new(); at (e?) Realm.x = 1; e!(1)", NULL);
    utest_e2e_run_to_no_runnable(&vm);
    n = urbi_trace_snapshot(&vm, out, 128, &d);
    UASSERT(trace_saw_schema(out, n, URBI_TP_EVENT_EMIT));
    UASSERT(trace_saw_schema(out, n, URBI_TP_WATCHER_FIRE));
    urbi_vm_destroy(&vm);
}

UTEST(trace_watcher_install)
{
    UVM vm;
    UTraceRecord out[128];
    uint32_t d;
    size_t n;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_trace_set_level(&vm, URBI_TRACE_WATCHER, URBI_LOG_INFO);
    /* Value watcher routes through urbi_watcher_install_watcher_runtime (INSTALL tap). */
    utest_e2e_compile_and_run(&vm, "var x = 0; at (x > 5) Realm.y = 1", NULL);
    utest_e2e_run_to_no_runnable(&vm);
    n = urbi_trace_snapshot(&vm, out, 128, &d);
    UASSERT(trace_saw_schema(out, n, URBI_TP_WATCHER_INSTALL));
    urbi_vm_destroy(&vm);
}

UTEST(trace_tag_op)
{
    UVM vm;
    UTraceRecord out[32];
    uint32_t d;
    size_t n;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_trace_set_level(&vm, URBI_TRACE_TAG, URBI_LOG_INFO);
    /* freeze on a member-less tag sets the flag and returns OK (TAG_OP tap). */
    utest_e2e_compile_and_run(&vm, "var t = Tag.new(); t.freeze()", NULL);
    utest_e2e_run_to_no_runnable(&vm);
    n = urbi_trace_snapshot(&vm, out, 32, &d);
    UASSERT(trace_saw_schema(out, n, URBI_TP_TAG_OP));
    urbi_vm_destroy(&vm);
}

/* REPL eval tap: urbi_repl_eval is available in the full (non-bytecode-only)
 * host build, so this needs URBI_TRACE only (not URBI_ENABLE_REPL). The
 * SESSION taps live in the REPL-server layer (urepl_dispatch.c) and are
 * exercised by the REPL suite under a URBI_ENABLE_REPL build. */
UTEST(trace_repl_eval)
{
    UVM vm;
    URealm *r;
    char buf[256];
    UTraceRecord out[32];
    uint32_t d;
    size_t n;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_trace_set_level(&vm, URBI_TRACE_REPL, URBI_LOG_INFO);
    r = urbi_realm_create_repl(&vm);
    UASSERT(r != NULL);
    (void)urbi_repl_eval(&vm, r, "1+2", 3, buf, sizeof buf);
    n = urbi_trace_snapshot(&vm, out, 32, &d);
    UASSERT(trace_saw_schema(out, n, URBI_TP_REPL_EVAL));
    urbi_vm_destroy(&vm);
}

#if defined(URBI_ENABLE_REPL)
/* Debug.trace() lives in the URBI_ENABLE_REPL-gated Debug namespace; this
 * end-to-end script test therefore needs both flags. */
UTEST(trace_debug_marker)
{
    UVM vm;
    URealm *r;
    char out[256];
    UTraceRecord rec[16];
    uint32_t d;
    size_t n, i;
    int found = 0;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_trace_set_level(&vm, URBI_TRACE_USER, URBI_LOG_INFO);
    r = urbi_realm_global(&vm);
    (void)urbi_repl_eval(&vm, r, "Debug.trace(\"phase1\")", 21, out, sizeof(out));
    n = urbi_trace_snapshot(&vm, rec, 16, &d);
    for (i = 0; i < n; i++)
        if (rec[i].schema_id == URBI_TP_USER_MARKER && rec[i].payload.str[0] == 'p')
            found = 1;
    UASSERT(found);
    urbi_vm_destroy(&vm);
}
#endif /* URBI_ENABLE_REPL */

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
    utest_run("trace_format_record", trace_format_record);
    utest_run("trace_format_marker_string", trace_format_marker_string);
    utest_run("trace_sched_records_on_fork", trace_sched_records_on_fork);
    utest_run("trace_gc_phase_transitions", trace_gc_phase_transitions);
    utest_run("trace_event_emit_and_watcher_fire", trace_event_emit_and_watcher_fire);
    utest_run("trace_watcher_install", trace_watcher_install);
    utest_run("trace_tag_op", trace_tag_op);
    utest_run("trace_repl_eval", trace_repl_eval);
#if defined(URBI_ENABLE_REPL)
    utest_run("trace_debug_marker", trace_debug_marker);
#endif
#endif
}
