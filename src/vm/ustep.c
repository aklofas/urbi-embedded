/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi_step: 4-state cooperative scheduler driver (row 8 §6 / T16).
   Freestanding-safe: only <stdbool.h> and <stdint.h>. */

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"
#include "sched/usched_post_dispatch.h"  /* sched_post_dispatch (scheduler F3) */
#include "event/uevent_ring.h"
#include "stdlib/temporal.h"   /* v0.9.4: urbi_periodic_pump (pre-loop) + urbi_periodic_earliest_wake_us (quiescence) */
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
        uint64_t now     = vm->host_time_us(vm->host_time_ud);
        uint64_t elapsed = now - vm->atomic_begin_us;
        if (elapsed > (uint64_t)URBI_ATOMIC_MAX_US) {
            urbi_panic("atomic section exceeded URBI_ATOMIC_MAX_US");
        }
    }
#endif

    /* Drain any ISR-injected events before running bytecode. */
    if (vm->event_ring && uevent_ring_has_pending(vm->event_ring))
        uevent_ring_drain(vm);

    /* v0.9.4: pump periodics before the dispatch loop so newly-due fires
     * become READY strands BEFORE we test strand_runnable_count.  Without
     * this pre-pump, a `every(P) body` call followed by a quiescent VM
     * would never see the first body spawn (the outer loop only runs
     * when strand_runnable_count > 0, but the loader strand that just
     * ran every_native has already finished and the periodic's first
     * fire happens lazily). */
    (void)urbi_periodic_pump(vm);

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

        /* Post-dispatch fix-ups — scheduler F3.
         *
         * sched_post_dispatch runs four bookkeeping steps that must execute after
         * every dispatch-loop iteration:
         *   1. Re-increment strand_runnable_count if the strand is now WAITING
         *      (corrects the double-decrement from sched_dequeue_ready_head +
         *      sched_strand_block both decrementing for state==RUNNING).
         *   2. Eager DEAD-strand reap via urbi_strand_destroy (prevents the
         *      register-stack accumulation wedge first seen at ~200 eye_demo body
         *      completions on ESP32, v0.7.x).
         *   3. Sleep-queue wake for any strand whose wake_us <= now.
         *   4. Periodic pump so a just-completed every()-body can re-arm within
         *      this urbi_step call.
         *
         * After step 2, s may be freed.  Do NOT dereference s after this call. */
        sched_post_dispatch(vm, s);
    }

    /* If any strand is still READY or RUNNING, the budget ran out. */
    if (vm->strand_runnable_count > 0) return URBI_STEP_RUNNING;

    /* No runnable strands.  Check other liveness sources per row 8 §3 Rule X. */
    if (vm->watchers->active_count   > 0 ||
        vm->event_queue_count      > 0 ||
        vm->host_call_pending_count > 0) {
        /* Watchers or pending events can make strands runnable on the next tick. */
        return URBI_STEP_RUNNING;
    }

    /* v0.9.4: live periodics keep the VM non-quiescent.  Pump again here
     * because a body strand may have died during this step and re-armed
     * the periodic; the next fire becomes the earliest_wake gate. */
    uint64_t periodic_next = urbi_periodic_earliest_wake_us(vm);

    /* Only sleeping strands remain — nothing can run until the earliest wake. */
    if (vm->wakeup_pending_count > 0 || periodic_next != UINT64_MAX) {
        if (out_next_wake_us) {
            uint64_t earliest = (vm->wakeup_pending_count > 0)
                              ? sched_earliest_wake_us(vm)
                              : UINT64_MAX;
            if (periodic_next < earliest) earliest = periodic_next;
            *out_next_wake_us = earliest;
        }
        return URBI_STEP_WAKE_AT;
    }

    /* All five counters are zero (or irrelevant): fully quiescent. */
    return URBI_STEP_QUIESCENT;
}
