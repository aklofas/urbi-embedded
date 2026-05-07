/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: 5-counter liveness ownership (row 8 §3 Rule X).
   Verifies strand_runnable_count and wakeup_pending_count are maintained
   at their respective push/pop sites, and sched_quiescent integrates them.
   Full scheduler / step-driver tests land at T21. */

#include "utest.h"
#include "sched/usched_cooperative.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include <stdint.h>

#define UTEST(name) static void name(void)

/* Case 1: strand_runnable_count increments on make_runnable and
   decrements symmetrically on block; multi-strand coverage. */
UTEST(counter_strand_runnable_increments_on_make_runnable)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    UASSERT_EQ(vm.strand_runnable_count, 0U);

    sched_strand_make_runnable(&a);
    UASSERT_EQ(vm.strand_runnable_count, 1U);

    sched_strand_make_runnable(&b);
    UASSERT_EQ(vm.strand_runnable_count, 2U);

    /* Simulate dispatch: strand becomes RUNNING, then blocks. */
    a.state = USTRAND_STATE_RUNNING;
    sched_strand_block(&a, USTRAND_REASON_EVENT, 0);
    UASSERT_EQ(vm.strand_runnable_count, 1U);

    b.state = USTRAND_STATE_RUNNING;
    sched_strand_block(&b, USTRAND_REASON_EVENT, 0);
    UASSERT_EQ(vm.strand_runnable_count, 0U);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    uvm_destroy(&vm);
}

/* Case 2: wakeup_pending_count tracks sleep_q_insert and sleep_q_remove. */
UTEST(counter_wakeup_pending_tracks_sleep_q)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b, c;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);
    ustrand_init(&c, &vm);

    UASSERT_EQ(vm.wakeup_pending_count, 0U);

    /* Block via SLEEP: sleep_q_insert called internally by sched_strand_block. */
    a.state = USTRAND_STATE_RUNNING;
    sched_strand_block(&a, USTRAND_REASON_SLEEP, 1000U);
    UASSERT_EQ(vm.wakeup_pending_count, 1U);

    b.state = USTRAND_STATE_RUNNING;
    sched_strand_block(&b, USTRAND_REASON_SLEEP, 2000U);
    UASSERT_EQ(vm.wakeup_pending_count, 2U);

    c.state = USTRAND_STATE_RUNNING;
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
    uvm_destroy(&vm);
}

/* Case 3: sched_quiescent returns true only when all active counters are zero. */
UTEST(quiescent_when_all_counters_zero)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
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
    uvm_destroy(&vm);
}

/* Case 4: strand_suspended_count is excluded from quiescence (always 0 at M3).
   Confirmed by sched_quiescent not referencing it; this test documents the
   row 9 §3.2 contract — adding a strand to DORMANT doesn't block quiescence. */
UTEST(suspended_count_excluded_from_quiescence)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
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
    UASSERT_EQ(vm.watcher_active_count,  0U);
    UASSERT_EQ(vm.event_queue_count,     0U);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    uvm_destroy(&vm);
}

/* Case 5: wakeup_pending_count does not underflow on double-remove.
   If a strand is not on the sleep queue, sleep_q_remove is a no-op on the counter. */
UTEST(wakeup_pending_no_underflow_on_strand_not_on_queue)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    /* Put only a on the sleep queue. */
    a.state = USTRAND_STATE_RUNNING;
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
    uvm_destroy(&vm);
}

/* Case 6: runnable_count correctly tracks sched_strand_yield (yield re-enqueues). */
UTEST(runnable_count_stable_across_yield_cycle)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a;
    ustrand_init(&a, &vm);

    sched_strand_make_runnable(&a);
    UASSERT_EQ(vm.strand_runnable_count, 1U);

    /* Simulate dispatch: dequeue (T16 pattern: decrement + set RUNNING). */
    vm.ready_head = a.ready_next;
    if (vm.ready_head != NULL)
        vm.ready_head->ready_prev = NULL;
    else
        vm.ready_tail = NULL;
    a.ready_next = NULL;
    a.ready_prev = NULL;
    if (vm.strand_runnable_count > 0) vm.strand_runnable_count--;
    a.state = USTRAND_STATE_RUNNING;
    UASSERT_EQ(vm.strand_runnable_count, 0U);

    /* Strand yields: increments back to 1. */
    sched_strand_yield(&a);
    UASSERT_EQ(vm.strand_runnable_count, 1U);
    UASSERT_EQ(a.state, USTRAND_STATE_READY);

    ustrand_destroy(&a, &vm);
    uvm_destroy(&vm);
}

/* ===== T16 urbi_step driver tests ===== */

#include "urbi/urbi.h"   /* UStepResult, urbi_step */

/* Case 7: urbi_step on a freshly initialised VM returns QUIESCENT.
   All 5 liveness counters are zero; no strands, no watchers. */
UTEST(step_quiescent_on_empty_vm)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    uint64_t wake_us = 0xdeadbeef;
    UStepResult r = urbi_step(&vm, 1000, &wake_us);

    UASSERT_EQ((int)r, (int)URBI_STEP_QUIESCENT);
    /* out_next_wake_us should be untouched for QUIESCENT. */
    UASSERT_EQ(wake_us, (uint64_t)0xdeadbeef);

    uvm_destroy(&vm);
}

/* Case 8: urbi_step returns FATAL when vm->fatal_strand is non-NULL.
   Uses a stack-allocated stub strand; no dispatch occurs. */
UTEST(step_fatal_when_fatal_strand_set)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
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
    uvm_destroy(&vm);
}

/* Case 9: urbi_step returns WAKE_AT (not QUIESCENT) when one strand is sleeping.
   No runnable strands exist, but wakeup_pending_count > 0. */
UTEST(step_wake_at_with_wakeup_pending)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a;
    ustrand_init(&a, &vm);

    /* Block strand on a sleep wake in the future. */
    a.state = USTRAND_STATE_RUNNING;
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
    uvm_destroy(&vm);
}

/* Case 10: sched_dequeue_ready_head is idempotent on an empty queue (no crash). */
UTEST(dequeue_ready_head_noop_on_empty_queue)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    /* Should not crash / underflow. */
    sched_dequeue_ready_head(&vm);
    UASSERT_EQ(vm.strand_runnable_count, 0U);
    UASSERT(vm.ready_head == NULL);

    uvm_destroy(&vm);
}

/* Case 11: sched_dequeue_ready_head from a two-strand queue leaves the
   second strand as the new head. */
UTEST(dequeue_ready_head_advances_queue)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    sched_strand_make_runnable(&a);
    sched_strand_make_runnable(&b);
    UASSERT_EQ(vm.strand_runnable_count, 2U);
    UASSERT(vm.ready_head == &a);

    sched_dequeue_ready_head(&vm);
    UASSERT_EQ(vm.strand_runnable_count, 1U);
    UASSERT(vm.ready_head == &b);
    UASSERT(a.ready_next == NULL);
    UASSERT(a.ready_prev == NULL);

    sched_dequeue_ready_head(&vm);
    UASSERT_EQ(vm.strand_runnable_count, 0U);
    UASSERT(vm.ready_head == NULL);
    UASSERT(vm.ready_tail == NULL);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    uvm_destroy(&vm);
}

void test_step_driver_suite(void) {
    utest_run("counter_strand_runnable_increments_on_make_runnable",
              counter_strand_runnable_increments_on_make_runnable);
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
    /* step_wake_at_with_sleeping_strand_only: deferred to T21.
       Requires urbi_strand_spawn_sleeping which does not exist at T16. */
}
