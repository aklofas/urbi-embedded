/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi_step: 4-state cooperative scheduler driver (row 8 §6 / T16).
   Freestanding-safe: only <stdbool.h> and <stdint.h>. */

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"
#include "event/uevent_ring.h"
#include "runtime/umacros.h"   /* URBI_INTERNAL_ASSERT (CHSTR-025 wait_payload arm guard) */
#include <stddef.h>
#include <stdint.h>

/* v0.9.1 — REPL service step-driver hook.  Defined in src/repl/
 * urepl_dispatch.c when URBI_ENABLE_REPL=1, weak/no-op otherwise so
 * the default build links cleanly without the REPL TUs.  The hook
 * reads vm->repl_server: if non-NULL, drains the per-server job queue
 * and signals reader subthreads to flush output.  Called BEFORE any
 * other step work so REPL commands submitted between steps are
 * visible to bytecode that runs this tick. */
__attribute__((weak)) void urepl_dispatch_drain_if_active(struct UVM *vm);
__attribute__((weak)) void
urepl_dispatch_drain_if_active(struct UVM *vm) { (void)vm; }

UStepResult
urbi_step(UVM *vm, uint64_t budget_instructions, uint64_t *out_next_wake_us)
{
    URBI_ASSERT_NOT_ISR(vm);

    /* v0.9.1 REPL drain hook (weak symbol; no-op outside URBI_ENABLE_REPL). */
    urepl_dispatch_drain_if_active(vm);

    /* Fast-path: previous call left a fatal strand wired; caller must inspect
     * and reset (via urbi_strand_reset) or shut down before calling again. */
    if (vm->fatal_strand) return URBI_STEP_FATAL;

    /* Gap R: URBI_DEBUG watchdog for open atomic sections.
     * If the embedder called urbi_atomic_begin and hasn't called urbi_atomic_end
     * before the next urbi_step, check whether the section has been held longer
     * than URBI_ATOMIC_MAX_US microseconds.  Requires host_time_us. */
#ifdef URBI_DEBUG
    if (vm->atomic_active && vm->host_time_us != NULL) {
        uint64_t now     = vm->host_time_us();
        uint64_t elapsed = now - vm->atomic_begin_us;
        if (elapsed > (uint64_t)URBI_ATOMIC_MAX_US) {
            urbi_panic("atomic section exceeded URBI_ATOMIC_MAX_US");
        }
    }
#endif

    /* Drain any ISR-injected events before running bytecode. */
    if (vm->event_ring && uevent_ring_has_pending(vm->event_ring))
        uevent_ring_drain(vm);

    vm->step_budget_remaining = budget_instructions;

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
        vm->cur_strand = s;   /* spec #3 §7.1: expose running strand for c_event_waituntil */

        uint64_t consumed = dispatch_loop_until_yield(s, vm->step_budget_remaining);
        vm->cur_strand = NULL;
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

        /* T20 (v0.7.x): eager DEAD-strand reap.  Heap-allocated strands
         * (`urbi_strand_create` — used for watcher bodies, fork children, and
         * any script-spawned strand) sit on `realm->strands_head` until
         * `urbi_realm_destroy` would otherwise reap them at VM teardown.
         * Without an eager reap during normal dispatch, DEAD strands
         * accumulate indefinitely; each carries a register stack of
         * `UVM_STACK_CAP * sizeof(UValue)` (32 KB at default) so the leak
         * climbs into the multi-MB range at moderate event rates.  Surfaced
         * on the ESP-IDF eye_demo as a hard wedge at ~200 body completions:
         * register-stack alloc failed (`watcher body spawn: out of memory
         * (stack alloc)` URBI_LOG_WARN), `body_strand` stayed NULL, and the
         * at-handler dispatch went silent.
         *
         * Safe to reap here because: `watcher_body_owner` was cleared by
         * `urbi_watcher_body_completed` in `exit_strand:` above; joiners
         * were woken by `fork_wake_joiners`; ready/sleep queues were
         * already unbound; `vm->cur_strand` was cleared after dispatch
         * returned.  The FATAL path returned `URBI_STEP_FATAL` above so
         * we never reap a strand the host still wants to inspect.
         *
         * `urbi_strand_destroy` is the canonical full teardown — unlinks
         * from `realm->strands_head`, unbinds queues, frees cleanup
         * stack + register stack + the strand struct itself. */
        if (s->state == USTRAND_STATE_DEAD) {
            urbi_strand_destroy(s);
            /* s is freed; do not dereference past this point. */
        }

        /* Wake any sleep-queue strands whose wake_us has passed.
         * CHSTR-025: every node on sleep_q is REASON_SLEEP, so wake_us is the
         * active union arm; assert at the loop head to surface a queue-invariant
         * break in -DURBI_DEBUG builds. */
        {
            uint64_t now = vm->host_time_us();
            while (vm->sleep_q_head) {
                URBI_INTERNAL_ASSERT(
                    USTRAND_GET_REASON(vm->sleep_q_head) == USTRAND_REASON_SLEEP);
                if (vm->sleep_q_head->wait_payload.wake_us > now) break;
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
