/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: 5-counter liveness ownership (row 8 §3 Rule X).
   Verifies strand_runnable_count and wakeup_pending_count are maintained
   at their respective push/pop sites, and sched_quiescent integrates them.
   Full scheduler / step-driver tests land at T21. */

#include "utest.h"
#include "usched_cooperative.h"
#include "uvm.h"
#include "ustrand.h"
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

    UASSERT_EQ(vm.strand_runnable_count, 0u);

    sched_strand_make_runnable(&a);
    UASSERT_EQ(vm.strand_runnable_count, 1u);

    sched_strand_make_runnable(&b);
    UASSERT_EQ(vm.strand_runnable_count, 2u);

    /* Simulate dispatch: strand becomes RUNNING, then blocks. */
    a.state = USTRAND_STATE_RUNNING;
    sched_strand_block(&a, USTRAND_REASON_EVENT, 0);
    UASSERT_EQ(vm.strand_runnable_count, 1u);

    b.state = USTRAND_STATE_RUNNING;
    sched_strand_block(&b, USTRAND_REASON_EVENT, 0);
    UASSERT_EQ(vm.strand_runnable_count, 0u);

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

    UASSERT_EQ(vm.wakeup_pending_count, 0u);

    /* Block via SLEEP: sleep_q_insert called internally by sched_strand_block. */
    a.state = USTRAND_STATE_RUNNING;
    sched_strand_block(&a, USTRAND_REASON_SLEEP, 1000u);
    UASSERT_EQ(vm.wakeup_pending_count, 1u);

    b.state = USTRAND_STATE_RUNNING;
    sched_strand_block(&b, USTRAND_REASON_SLEEP, 2000u);
    UASSERT_EQ(vm.wakeup_pending_count, 2u);

    c.state = USTRAND_STATE_RUNNING;
    sched_strand_block(&c, USTRAND_REASON_SLEEP, 500u);
    UASSERT_EQ(vm.wakeup_pending_count, 3u);

    /* Unblock (removes from sleep queue): wakeup_pending_count decrements. */
    sched_strand_unblock(&c);
    UASSERT_EQ(vm.wakeup_pending_count, 2u);

    sched_strand_unblock(&a);
    UASSERT_EQ(vm.wakeup_pending_count, 1u);

    sched_strand_unblock(&b);
    UASSERT_EQ(vm.wakeup_pending_count, 0u);

    /* runnable_count picked up the unblocked strands. */
    UASSERT_EQ(vm.strand_runnable_count, 3u);

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
    UASSERT_EQ(vm.strand_runnable_count, 0u);
    UASSERT_EQ(vm.wakeup_pending_count,  0u);
    UASSERT_EQ(vm.host_call_pending_count, 0u);
    UASSERT_EQ(vm.watcher_active_count,  0u);
    UASSERT_EQ(vm.event_queue_count,     0u);

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
    sched_strand_block(&a, USTRAND_REASON_SLEEP, 1000u);
    UASSERT_EQ(vm.wakeup_pending_count, 1u);

    /* Attempt to remove b (never inserted): counter must not underflow. */
    sched_strand_unblock(&b);   /* b is DORMANT, not on sleep queue */
    /* wakeup_pending_count unchanged: b wasn't in sleep queue */
    UASSERT_EQ(vm.wakeup_pending_count, 1u);

    /* Now properly remove a. */
    sched_strand_unblock(&a);
    UASSERT_EQ(vm.wakeup_pending_count, 0u);

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
    UASSERT_EQ(vm.strand_runnable_count, 1u);

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
    UASSERT_EQ(vm.strand_runnable_count, 0u);

    /* Strand yields: increments back to 1. */
    sched_strand_yield(&a);
    UASSERT_EQ(vm.strand_runnable_count, 1u);
    UASSERT_EQ(a.state, USTRAND_STATE_READY);

    ustrand_destroy(&a, &vm);
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
}
