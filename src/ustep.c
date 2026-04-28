/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi_step: 4-state cooperative scheduler driver (row 8 §6 / T16).
   Freestanding-safe: only <stdbool.h> and <stdint.h>. */

#include "urbi.h"
#include "uvm.h"
#include "ustrand.h"
#include "usched_cooperative.h"
#include "uevent_ring.h"

UStepResult
urbi_step(UVM *vm, uint64_t budget, uint64_t *out_next_wake_us)
{
    URBI_ASSERT_NOT_ISR(vm);

    /* Fast-path: previous call left a fatal strand wired; caller must inspect
     * and reset (via urbi_strand_reset) or shut down before calling again. */
    if (vm->fatal_strand) return URBI_STEP_FATAL;

    /* Drain any ISR-injected events before running bytecode. */
    if (vm->event_ring && uevent_ring_has_pending(vm->event_ring))
        uevent_ring_drain(vm);

    vm->step_budget_remaining = budget;

    /* Round-robin through all READY strands until the budget is exhausted
     * or the run-queue empties. */
    while (vm->step_budget_remaining > 0 && vm->strand_runnable_count > 0) {
        UStrand *s = sched_pick_next(vm);
        if (!s) break;

        /* Remove the strand from the ready queue and charge the count.
         * sched_dequeue_ready_head decrements strand_runnable_count.
         * If the strand yields mid-run, dispatch_loop_until_yield calls
         * sched_strand_yield which re-enqueues and increments the count. */
        sched_dequeue_ready_head(vm);
        s->state = USTRAND_STATE_RUNNING;

        uint64_t consumed = dispatch_loop_until_yield(s, vm->step_budget_remaining);
        /* Clamp subtraction to avoid unsigned underflow on floating rounding. */
        if (consumed >= vm->step_budget_remaining) {
            vm->step_budget_remaining = 0;
        } else {
            vm->step_budget_remaining -= consumed;
        }

        /* Check for strand-level fatal (unwind the host-visible fatal pointer). */
        if (s->fatal_status != UEXEC_OK) {
            vm->fatal_strand = s;
            return URBI_STEP_FATAL;
        }

        /* If the strand died, strand_runnable_count was NOT decremented by
         * dispatch_loop_until_yield (per T15 Option B contract — the exit_strand:
         * label does not decrement on DEAD).  We decremented it above via
         * sched_dequeue_ready_head before dispatch; no further adjustment needed.
         * DEAD strands are left for T20's strand-lifecycle cleanup.
         *
         * If the strand BLOCKED (WAITING state) via sched_strand_block from within
         * the dispatch loop (e.g. OP_JOIN_WAIT), sched_strand_block decrements
         * strand_runnable_count because it sees state=RUNNING.  But that count was
         * already decremented by sched_dequeue_ready_head above, so the block path
         * would double-decrement and erase a child strand's runnable slot.
         *
         * Re-increment invariant: sched_dequeue_ready_head decremented
         * strand_runnable_count when we picked this strand. If the strand
         * transitioned to WAITING during dispatch (e.g. OP_JOIN_WAIT calls
         * sched_strand_block which decrements for state == RUNNING), the counter
         * is now double-decremented and the scheduler would lose track of the fact
         * that the strand is waiting on something rather than gone entirely.
         *
         * Re-increment to restore symmetry. Any future blocking opcode that calls
         * sched_strand_block from inside the dispatch loop must rely on this
         * re-increment — adding such an opcode without checking this invariant
         * will silently underflow the runnable count. */
        if (USTRAND_IS_WAITING(s)) {
            vm->strand_runnable_count++;
        }

        /* Wake any sleep-queue strands whose wake_us has passed. */
        {
            uint64_t now = vm->host_time_us();
            while (vm->sleep_q_head &&
                   vm->sleep_q_head->wait_payload.wake_us <= now) {
                UStrand *waker = vm->sleep_q_head;
                /* sched_strand_unblock removes from sleep_q (decrementing
                 * wakeup_pending_count) and calls sched_strand_make_runnable. */
                sched_strand_unblock(waker);
            }
        }
    }

    /* If any strand is still READY or RUNNING, the budget ran out. */
    if (vm->strand_runnable_count > 0) return URBI_STEP_RUNNING;

    /* No runnable strands.  Check other liveness sources per row 8 §3 Rule X. */
    if (vm->watcher_active_count   > 0 ||
        vm->event_queue_count      > 0 ||
        vm->host_call_pending_count > 0) {
        /* Watchers or pending events can make strands runnable on the next tick. */
        return URBI_STEP_RUNNING;
    }

    /* Only sleeping strands remain — nothing can run until the earliest wake. */
    if (vm->wakeup_pending_count > 0) {
        if (out_next_wake_us)
            *out_next_wake_us = sched_earliest_wake_us(vm);
        return URBI_STEP_WAKE_AT;
    }

    /* All five counters are zero (or irrelevant): fully quiescent. */
    return URBI_STEP_QUIESCENT;
}
