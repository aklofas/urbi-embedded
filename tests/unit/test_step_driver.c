/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: 5-counter liveness ownership (row 8 §3 Rule X).
   Verifies strand_runnable_count and wakeup_pending_count are maintained
   at their respective push/pop sites, and sched_quiescent integrates them.
   Full scheduler / step-driver tests land at T21. */

#include "utest.h"
#include "sched/usched_cooperative.h"
#include "sched/usched_post_dispatch.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "realm/urealm.h"   /* URealm (case 14 realm-registered stub) */
#include "utest_e2e_helpers.h"
#include <stdint.h>

#define UTEST(name) static void name(void)

/* Mock monotonic clock for the v0.11.4-A lone-sleeper wake test. */
static uint64_t g_mock_now_us = 0;
static uint64_t mock_clock(void *ud) { (void)ud; return g_mock_now_us; }

/* Case 1: strand_runnable_count increments on make_runnable and
   decrements symmetrically on block; multi-strand coverage. */
UTEST(counter_strand_runnable_increments_on_make_runnable)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    UASSERT_EQ(vm.strand_runnable_count, 0U);

    sched_strand_make_runnable(&a);
    UASSERT_EQ(vm.strand_runnable_count, 1U);

    sched_strand_make_runnable(&b);
    UASSERT_EQ(vm.strand_runnable_count, 2U);

    /* Simulate dispatch: dequeue (count-neutral under SCHED-01 — the strand
     * moves READY -> RUNNING inside the counted set), then block (-1). */
    sched_dequeue_ready_head(&vm);   /* a */
    a.state = USTRAND_STATE_RUNNING;
    UASSERT_EQ(vm.strand_runnable_count, 2U);
    sched_strand_block(&a, USTRAND_REASON_EVENT, 0);
    UASSERT_EQ(vm.strand_runnable_count, 1U);

    sched_dequeue_ready_head(&vm);   /* b */
    b.state = USTRAND_STATE_RUNNING;
    UASSERT_EQ(vm.strand_runnable_count, 1U);
    sched_strand_block(&b, USTRAND_REASON_EVENT, 0);
    UASSERT_EQ(vm.strand_runnable_count, 0U);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 2: wakeup_pending_count tracks sleep_q_insert and sleep_q_remove. */
UTEST(counter_wakeup_pending_tracks_sleep_q)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b, c;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);
    ustrand_init(&c, &vm);

    UASSERT_EQ(vm.wakeup_pending_count, 0U);

    /* Block via SLEEP: sleep_q_insert called internally by sched_strand_block.
     * SCHED-01: a RUNNING strand is in the counted set, so seed the count
     * before each hand-built RUNNING -> block transition. */
    a.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&a, USTRAND_REASON_SLEEP, 1000U);
    UASSERT_EQ(vm.wakeup_pending_count, 1U);

    b.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&b, USTRAND_REASON_SLEEP, 2000U);
    UASSERT_EQ(vm.wakeup_pending_count, 2U);

    c.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&c, USTRAND_REASON_SLEEP, 500U);
    UASSERT_EQ(vm.wakeup_pending_count, 3U);

    /* Unblock (removes from sleep queue): wakeup_pending_count decrements. */
    sched_strand_unblock(&c);
    UASSERT_EQ(vm.wakeup_pending_count, 2U);

    sched_strand_unblock(&a);
    UASSERT_EQ(vm.wakeup_pending_count, 1U);

    sched_strand_unblock(&b);
    UASSERT_EQ(vm.wakeup_pending_count, 0U);

    /* runnable_count picked up the unblocked strands. */
    UASSERT_EQ(vm.strand_runnable_count, 3U);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    ustrand_destroy(&c, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 3: sched_quiescent returns true only when all active counters are zero. */
UTEST(quiescent_when_all_counters_zero)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UASSERT(sched_quiescent(&vm));

    UStrand a;
    ustrand_init(&a, &vm);

    sched_strand_make_runnable(&a);
    UASSERT(!sched_quiescent(&vm));

    /* Remove from ready queue manually (simulating dispatch dequeue). */
    vm.ready_head = a.ready_next;
    if (vm.ready_head != NULL)
        vm.ready_head->ready_prev = NULL;
    else
        vm.ready_tail = NULL;
    a.ready_next = NULL;
    a.ready_prev = NULL;
    if (vm.strand_runnable_count > 0) vm.strand_runnable_count--;

    UASSERT(sched_quiescent(&vm));

    ustrand_destroy(&a, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 4: armed work is excluded from quiescence (refactor-3 SCHED-13,
   owner decision 2026-06-11).  DORMANT strands never count anywhere, and
   SUSPENDED/WAITING strands are `armed` — counted (vm_liveness) and
   reported (urbi_vm_has_live_work) but not quiescence-blocking. */
UTEST(suspended_count_excluded_from_quiescence)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    /* Both strands dormant (initialized, not yet made runnable). */
    UASSERT_EQ(a.state, USTRAND_STATE_DORMANT);
    UASSERT_EQ(b.state, USTRAND_STATE_DORMANT);

    /* VM should still be quiescent: dormant strands don't count. */
    UASSERT(sched_quiescent(&vm));
    UASSERT_EQ(vm.strand_runnable_count, 0U);
    UASSERT_EQ(vm.wakeup_pending_count,  0U);
    UASSERT_EQ(vm.host_call_pending_count, 0U);
    UASSERT_EQ(vm.watchers->active_count,  0U);

    /* SCHED-13: an armed (WAITING) strand still does not block quiescence —
     * it is external-input work.  Park a, confirm counted-but-quiescent,
     * then wake it back out (count-symmetric). */
    a.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;       /* satisfy block's RUNNING-decrement */
    sched_strand_block(&a, USTRAND_REASON_EVENT, 0);
    UASSERT_EQ(vm.strand_waiting_count, 1U);
    UASSERT(sched_quiescent(&vm));      /* armed-only: still quiescent */
    sched_strand_make_runnable(&a);
    UASSERT_EQ(vm.strand_waiting_count, 0U);
    UASSERT(!sched_quiescent(&vm));     /* runnable again: not quiescent */
    sched_strand_unbind_from_ready_queue(&a);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 5: wakeup_pending_count does not underflow on double-remove.
   If a strand is not on the sleep queue, sleep_q_remove is a no-op on the counter. */
UTEST(wakeup_pending_no_underflow_on_strand_not_on_queue)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    /* Put only a on the sleep queue.  SCHED-01: seed the count for the
     * hand-built RUNNING strand so block's decrement balances. */
    a.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&a, USTRAND_REASON_SLEEP, 1000U);
    UASSERT_EQ(vm.wakeup_pending_count, 1U);

    /* Attempt to remove b (never inserted): counter must not underflow. */
    sched_strand_unblock(&b);   /* b is DORMANT, not on sleep queue */
    /* wakeup_pending_count unchanged: b wasn't in sleep queue */
    UASSERT_EQ(vm.wakeup_pending_count, 1U);

    /* Now properly remove a. */
    sched_strand_unblock(&a);
    UASSERT_EQ(vm.wakeup_pending_count, 0U);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 6: runnable_count is stable across a full yield cycle.
 *
 * SCHED-01 (v0.13.3) semantics: count == |READY| + |RUNNING|.  The dispatch
 * dequeue is count-NEUTRAL (READY -> RUNNING stays inside the counted set)
 * and yield is count-NEUTRAL too (RUNNING -> READY re-enqueue: dec + inc).
 * Pre-refactor this test pinned the old rule (dequeue -1, yield +1). */
UTEST(runnable_count_stable_across_yield_cycle)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a;
    ustrand_init(&a, &vm);

    sched_strand_make_runnable(&a);
    UASSERT_EQ(vm.strand_runnable_count, 1U);

    /* Simulate dispatch: dequeue + set RUNNING — count unchanged. */
    sched_dequeue_ready_head(&vm);
    a.state = USTRAND_STATE_RUNNING;
    UASSERT_EQ(vm.strand_runnable_count, 1U);

    /* Strand yields: re-enqueued at the tail — count still unchanged. */
    sched_strand_yield(&a);
    UASSERT_EQ(vm.strand_runnable_count, 1U);
    UASSERT_EQ(a.state, USTRAND_STATE_READY);
    UASSERT(vm.ready_head == &a);

    /* Cleanup: unbind decrements (READY strand leaves the counted set). */
    sched_strand_unbind_from_ready_queue(&a);
    UASSERT_EQ(vm.strand_runnable_count, 0U);

    ustrand_destroy(&a, &vm);
    urbi_vm_destroy(&vm);
}

/* ===== T16 urbi_step driver tests ===== */

#include "urbi/urbi.h"   /* UStepResult, urbi_step */

/* Case 7: urbi_step on a freshly initialised VM returns QUIESCENT.
   All 5 liveness counters are zero; no strands, no watchers. */
UTEST(step_quiescent_on_empty_vm)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    uint64_t wake_us = 0xdeadbeef;
    UStepResult r = urbi_step(&vm, 1000, &wake_us);

    UASSERT_EQ((int)r, (int)URBI_STEP_QUIESCENT);
    /* out_next_wake_us should be untouched for QUIESCENT. */
    UASSERT_EQ(wake_us, (uint64_t)0xdeadbeef);

    urbi_vm_destroy(&vm);
}

/* Case 8: urbi_step returns FATAL when vm->fatal_strand is non-NULL.
   Uses a stack-allocated stub strand; no dispatch occurs. */
UTEST(step_fatal_when_fatal_strand_set)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand stub;
    ustrand_init(&stub, &vm);
    stub.fatal_status = UEXEC_THROW;  /* simulate a prior fatal event */

    /* Wire the fatal pointer directly — simulates what dispatch sets. */
    vm.fatal_strand = &stub;

    UStepResult r = urbi_step(&vm, 1000, NULL);
    UASSERT_EQ((int)r, (int)URBI_STEP_FATAL);

    /* Clear so destroy/cleanup doesn't see it. */
    vm.fatal_strand = NULL;
    ustrand_destroy(&stub, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 9: urbi_step returns WAKE_AT (not QUIESCENT) when one strand is sleeping.
   No runnable strands exist, but wakeup_pending_count > 0. */
UTEST(step_wake_at_with_wakeup_pending)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    /* Install a controlled clock at t=0 so the wake at 999999 stays in the
       future.  v0.11.4-A added a pre-dispatch sleep-queue pump to urbi_step;
       without a mocked clock the default real wall-clock would read far past
       999999, spuriously waking (and dispatching) this bytecode-less stub. */
    vm.host_time_us = mock_clock;
    g_mock_now_us   = 0;

    UStrand a;
    ustrand_init(&a, &vm);

    /* Block strand on a sleep wake in the future.  SCHED-01: seed the
     * count for the hand-built RUNNING strand; block decrements it. */
    a.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&a, USTRAND_REASON_SLEEP, 999999U);

    UASSERT_EQ(vm.strand_runnable_count, 0U);
    UASSERT_EQ(vm.wakeup_pending_count,  1U);

    uint64_t wake_us = 0;
    UStepResult r = urbi_step(&vm, 1000, &wake_us);

    UASSERT_EQ((int)r, (int)URBI_STEP_WAKE_AT);
    UASSERT_EQ(wake_us, (uint64_t)999999U);

    /* Cleanup: unblock then destroy. */
    sched_strand_unblock(&a);
    /* Drain the runnable entry manually to not trip counters on destroy. */
    sched_dequeue_ready_head(&vm);

    ustrand_destroy(&a, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 10: sched_dequeue_ready_head is idempotent on an empty queue (no crash). */
UTEST(dequeue_ready_head_noop_on_empty_queue)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    /* Should not crash / underflow. */
    sched_dequeue_ready_head(&vm);
    UASSERT_EQ(vm.strand_runnable_count, 0U);
    UASSERT(vm.ready_head == NULL);

    urbi_vm_destroy(&vm);
}

/* Case 11: sched_dequeue_ready_head from a two-strand queue leaves the
   second strand as the new head.  SCHED-01 (v0.13.3): the dequeue is
   count-NEUTRAL — the dequeued strand is about to become RUNNING and the
   count covers |READY| + |RUNNING| (pre-refactor this test pinned the old
   decrement-at-dequeue rule). */
UTEST(dequeue_ready_head_advances_queue)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    sched_strand_make_runnable(&a);
    sched_strand_make_runnable(&b);
    UASSERT_EQ(vm.strand_runnable_count, 2U);
    UASSERT(vm.ready_head == &a);

    sched_dequeue_ready_head(&vm);
    a.state = USTRAND_STATE_RUNNING;
    UASSERT_EQ(vm.strand_runnable_count, 2U);   /* count-neutral */
    UASSERT(vm.ready_head == &b);
    UASSERT(a.ready_next == NULL);
    UASSERT(a.ready_prev == NULL);

    /* a parks: the RUNNING -> WAITING transition is the decrement site. */
    sched_strand_block(&a, USTRAND_REASON_EVENT, 0);
    UASSERT_EQ(vm.strand_runnable_count, 1U);

    sched_dequeue_ready_head(&vm);
    b.state = USTRAND_STATE_RUNNING;
    UASSERT_EQ(vm.strand_runnable_count, 1U);   /* count-neutral */
    sched_strand_block(&b, USTRAND_REASON_EVENT, 0);
    UASSERT_EQ(vm.strand_runnable_count, 0U);
    UASSERT(vm.ready_head == NULL);
    UASSERT(vm.ready_tail == NULL);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 12 (v0.11.4-A): a lone expired sleeper is woken by urbi_step's
   pre-dispatch sleep-queue pump, even when no other strand drives the loop.
   Before the fix, the sleep wake lived only inside the post-dispatch fix-up,
   which a VM with no runnable strand never reached — urbi_step would spin on
   WAKE_AT forever and the sleeper never resumed.  Budget 0 isolates the
   pre-loop wake from dispatch (the hand-built strand carries no bytecode). */
UTEST(step_pre_loop_wakes_lone_sleeper)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);
    vm.host_time_us = mock_clock;
    g_mock_now_us   = 0;

    UStrand a;
    ustrand_init(&a, &vm);
    /* SCHED-01: seed the count for the hand-built RUNNING strand. */
    a.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&a, USTRAND_REASON_SLEEP, 100000U);  /* wake at 100 ms */
    UASSERT_EQ(vm.wakeup_pending_count,  1U);
    UASSERT_EQ(vm.strand_runnable_count, 0U);

    /* Step before the timer elapses (now=0 < 100000): sleeper stays parked. */
    (void)urbi_step(&vm, 0, NULL);
    UASSERT_EQ(vm.wakeup_pending_count,  1U);
    UASSERT_EQ(vm.strand_runnable_count, 0U);

    /* Advance past the wake.  The pre-loop pump must move the sleeper from the
       sleep queue to the ready queue with no other strand to drive dispatch. */
    g_mock_now_us = 200000;
    (void)urbi_step(&vm, 0, NULL);
    UASSERT_EQ(vm.wakeup_pending_count,  0U);   /* woken: off the sleep queue */
    UASSERT_EQ(vm.strand_runnable_count, 1U);   /* moved to ready */

    sched_dequeue_ready_head(&vm);
    ustrand_destroy(&a, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 13 (refactor-3 SCHED-01/B10): the runnable count reaches 0 when the
   last runnable strand parks itself.  Pre-fix leak shape: dequeue takes the
   last runnable strand (count -> 0), the strand parks (sched_strand_block's
   guarded decrement no-ops at 0), sched_post_dispatch step 1 re-increments
   -> permanent +1 after the strand dies (urbi_step returns RUNNING forever,
   host busy-spin).  Invariant under the single-writer scheme:
   count == |READY| + |RUNNING| at every observation point; WAITING strands
   are NOT counted. */
UTEST(runnable_count_zero_after_last_strand_parks)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    vm.host_time_us = mock_clock;
    g_mock_now_us   = 0;

    /* Run a strand that parks itself on the sleep queue.  The persistent
       loader strand (urbi_run_chunk) drives until the strand parks, then
       returns: READY is empty, nothing is RUNNING — only a parked sleeper
       remains. */
    UASSERT_EQ(utest_e2e_compile_and_run(&vm, "sleep(10s)", NULL), URBI_OK);

    UASSERT_EQ(vm.wakeup_pending_count, 1U);           /* parked sleeper */
    UASSERT_EQ(vm.strand_runnable_count, 0u);          /* pre-fix: 1 */

    urbi_vm_destroy(&vm);
}

/* Case 14 (refactor-3 SCHED-01/B10): full dequeue -> RUNNING -> block ->
   post_dispatch cycle on a stub strand, with the count asserted at every
   edge.  Single-writer invariant: count == |READY| + |RUNNING|, so the
   dequeue (READY -> about-to-RUN) is count-neutral and the block
   (RUNNING -> WAITING) is the single decrement; post_dispatch makes no
   further adjustment for a WAITING strand. */
UTEST(counter_full_dequeue_block_post_dispatch_cycle)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    vm.host_time_us = mock_clock;
    g_mock_now_us   = 0;

    /* v0.13.3 (SCHED-13): the strand must be realm-registered — the debug
     * recount oracle in sched_post_dispatch walks realms_head ->
     * strands_head to verify strand_waiting_count, so an off-realm stub
     * (which violates the §6.1 reachability invariant anyway) would count
     * in the counter but not in the walk. */
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UStrand *s = urbi_strand_create(&vm, r, NULL);
    UASSERT(s != NULL);

    sched_strand_make_runnable(s);
    UASSERT_EQ(vm.strand_runnable_count, 1U);          /* enqueued */

    sched_dequeue_ready_head(&vm);
    s->state = USTRAND_STATE_RUNNING;
    UASSERT_EQ(vm.strand_runnable_count, 1U);          /* READY -> RUNNING: neutral */

    sched_strand_block(s, USTRAND_REASON_SLEEP, 999999U);
    UASSERT_EQ(vm.strand_runnable_count, 0U);          /* RUNNING -> WAITING: -1 */
    UASSERT_EQ(vm.strand_waiting_count,  1U);          /* parked: armed work */

    sched_post_dispatch(&vm, s);
    UASSERT_EQ(vm.strand_runnable_count, 0U);          /* no re-increment */

    /* Cleanup: wake the stub through the real funnel (waiting counter
     * exits in sched_strand_make_runnable), then realm-destroy reaps it
     * off the ready queue. */
    sched_strand_unblock(s);
    UASSERT_EQ(vm.strand_waiting_count, 0U);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

void test_step_driver_suite(void) {
    utest_run("counter_strand_runnable_increments_on_make_runnable",
              counter_strand_runnable_increments_on_make_runnable);
    utest_run("step_pre_loop_wakes_lone_sleeper",
              step_pre_loop_wakes_lone_sleeper);
    utest_run("counter_wakeup_pending_tracks_sleep_q",
              counter_wakeup_pending_tracks_sleep_q);
    utest_run("quiescent_when_all_counters_zero",
              quiescent_when_all_counters_zero);
    utest_run("suspended_count_excluded_from_quiescence",
              suspended_count_excluded_from_quiescence);
    utest_run("wakeup_pending_no_underflow_on_strand_not_on_queue",
              wakeup_pending_no_underflow_on_strand_not_on_queue);
    utest_run("runnable_count_stable_across_yield_cycle",
              runnable_count_stable_across_yield_cycle);
    /* T16 step-driver tests */
    utest_run("step_quiescent_on_empty_vm",
              step_quiescent_on_empty_vm);
    utest_run("step_fatal_when_fatal_strand_set",
              step_fatal_when_fatal_strand_set);
    utest_run("step_wake_at_with_wakeup_pending",
              step_wake_at_with_wakeup_pending);
    utest_run("dequeue_ready_head_noop_on_empty_queue",
              dequeue_ready_head_noop_on_empty_queue);
    utest_run("dequeue_ready_head_advances_queue",
              dequeue_ready_head_advances_queue);
    /* refactor-3 SCHED-01/B10 single-writer runnable-count tests */
    utest_run("runnable_count_zero_after_last_strand_parks",
              runnable_count_zero_after_last_strand_parks);
    utest_run("counter_full_dequeue_block_post_dispatch_cycle",
              counter_full_dequeue_block_post_dispatch_cycle);
    /* step_wake_at_with_sleeping_strand_only: deferred to T21.
       Requires urbi_strand_spawn_sleeping which does not exist at T16. */
}
