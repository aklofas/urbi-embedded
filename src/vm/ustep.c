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
#include "urbi/ros.h"          /* urbi_ros_pump (self-gating; no-op without URBI_ENABLE_ROS2) */
#include "vm/uvm_reactive_drain.h"  /* vm_reactive_drain — idle-VM pump (SCHED-02) */
#include <stddef.h>
#include <stdint.h>

/* v0.9.1 — REPL service step-driver hook.  Defined in src/repl/
 * urepl_dispatch.c when URBI_ENABLE_REPL=1, weak/no-op otherwise so
 * the default build links cleanly without the REPL TUs.  The hook
 * reads vm->repl->server: if non-NULL, drains the per-server job queue
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

#ifdef URBI_ENABLE_ROS2
    /* Drain incoming ROS messages once per step (same safepoint as the ISR
     * event ring) so any `at (sub?(var m))` watcher fires this tick. */
    urbi_ros_pump(vm);
#endif

    /* v0.9.4: pump periodics before the dispatch loop so newly-due fires
     * become READY strands BEFORE we test strand_runnable_count.  Without
     * this pre-pump, a `every(P) body` call followed by a quiescent VM
     * would never see the first body spawn (the outer loop only runs
     * when strand_runnable_count > 0, but the loader strand that just
     * ran every_native has already finished and the periodic's first
     * fire happens lazily). */
    (void)urbi_periodic_pump(vm);

    /* v0.11.4-A: wake any sleeper whose timer has elapsed BEFORE the dispatch
     * loop tests strand_runnable_count.  Without this, a lone expired sleeper is
     * never woken — the post-dispatch sleep wake (sched_post_dispatch step 3)
     * only runs after a dispatch, but a VM whose sole live strand is sleeping
     * never dispatches (ready_head is NULL while the loop breaks on a NULL
     * sched_pick_next), so urbi_step would spin on RUNNING/WAKE_AT forever.
     * Mirrors the periodic pre-pump rationale above. */
    sched_wake_due_sleepers(vm);

    /* Idle-VM reactive pump (SCHED-02 / B10): drain slot-change dirty marks
     * and fire watcher-eval BEFORE the dispatch loop tests runnable count,
     * but ONLY when no strands are currently runnable.
     *
     * Rationale for the runnable guard: when strands ARE runnable (normal
     * execution), the dispatch-loop safepoints already call vm_reactive_drain
     * at every instruction boundary.  A pre-loop drain is only needed when the
     * VM is otherwise idle.
     *
     * SCHED-02 case: when the sole live strand is parked in waituntil(cond),
     * strand_runnable_count == 0.  A host-C slot write between urbi_step calls
     * bumps dirty_count, but the dispatch loop never runs (no READY strand) so
     * no safepoint drain fires.  The drain here wakes the parked strand (makes
     * it READY) before the dispatch loop test below.
     *
     * bounded_whenever = 1 (edge-only): a level-whenever whose body re-dirties
     * its own observed object would otherwise self-feed an unbounded re-fire on
     * each idle step (observer_dirty is cell-agnostic — see uwatcher_eval.c).
     * Firing the whenever only on its rising edge here breaks that loop while
     * still letting a parked waituntil/at (both rising-edge) wake. */
    if (vm->strand_runnable_count == 0) {
        vm_reactive_drain(vm, /*bounded_whenever=*/1);
    }

    vm->step_budget_remaining = budget_instructions;

    /* Dispatch loop + post-loop reactive drain (Step 4b).
     *
     * The inner while round-robins READY strands until the budget is exhausted
     * or the run-queue empties.  After it drains, the post-loop drain absorbs
     * dirty marks left by FLAT strands that completed without ever hitting a
     * safepoint (a one-statement at-handler body never reaches the dispatcher
     * `safepoint:` label), so a parked waituntil/at wakes and cascades resolve
     * within THIS step rather than one step late.
     *
     * Re-entry condition (runnable_before_drain): the outer do/while re-enters
     * the dispatch loop ONLY if the drain INCREASED strand_runnable_count — i.e.
     * it woke a parked waituntil/at or spawned a whenever body, both of which
     * tail-insert a READY strand (sched_strand_make_runnable), so sched_pick_next
     * is guaranteed to return dispatchable work that consumes budget.  Gating on
     * an INCREASE rather than on `runnable > 0` keeps re-entry tied to NEW work:
     * a leftover READY strand from this slice (e.g. one re-enqueued at VM-budget
     * exhaustion — refactor-3 VM-04/SCHED-11) must not trigger another drain
     * pass; the `&& step_budget_remaining > 0` clause already blocks that case,
     * and gating on the increase keeps the loop from re-running on a drain that
     * produced nothing.  When no new work is produced the loop exits and the
     * host re-pumps on the next urbi_step, exactly as before this task.
     *
     * bounded_whenever = 1 (edge-only) keeps even the legitimate re-entry finite:
     * a self-re-dirtying level-whenever fires at most once per rising edge (full
     * termination argument in uwatcher_eval.c's WHENEVER branch).
     *
     * FORBIDDEN: draining right after sched_post_dispatch *inside* the inner
     * loop — a per-iteration drain there reintroduces the reverted-S46
     * unbounded level-whenever re-spawn (see test_whenever_double_fire.c). */
    uint32_t runnable_before_drain;
    do {
        while (vm->step_budget_remaining > 0 && vm->strand_runnable_count > 0) {
            UStrand *s = sched_pick_next(vm);
            if (!s) break;

            /* W3a (v0.10.9): defence-in-depth skip for SUSPENDED strands.
             * urbi_strand_suspend always splices SUSPENDED strands out of the
             * ready queue via sched_strand_unbind_from_ready_queue, so this
             * branch should be unreachable today.  Guard kept so a future caller
             * that transitions a strand to SUSPENDED without unbinding cannot
             * crash the dispatcher by dispatching into a suspended strand.
             * SCHED-01: route through unbind (which decrements) rather than the
             * now count-neutral dequeue — a SUSPENDED strand leaves the counted
             * set; resume() puts it back via sched_strand_make_runnable. */
            if (USTRAND_IS_SUSPENDED(s)) {
                sched_strand_unbind_from_ready_queue(s);
                continue;
            }

            /* Remove the strand from the ready queue.  Count-neutral
             * (SCHED-01): READY → RUNNING keeps the strand in the counted set.
             * If the strand yields mid-run, dispatch_loop_until_yield calls
             * sched_strand_yield which re-enqueues count-neutrally. */
            sched_dequeue_ready_head(vm);
            s->state = USTRAND_STATE_RUNNING;
            /* refactor-3 VM-04/SCHED-11 (v0.13.1-E): re-arm per-slice safepoint budget.
             * Without this, a strand that exhausted its per-lifetime budget
             * (safepoint_budget_remaining==0 after URBI_STRAND_BUDGET_MAX safepoints)
             * yields at every subsequent safepoint before the GC-slice/drain section,
             * causing GC starvation in long-lived loops.  Re-arm here gives every
             * fresh dispatch slice a full URBI_STRAND_BUDGET_MAX window. */
            s->safepoint_budget_remaining = (uint16_t)URBI_STRAND_BUDGET_MAX;
            URBI_PERF_INC(vm, ctx_switches);   /* v0.11.1: strand go-live */
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
                /* SCHED-01: a fatal strand is DEAD (every fatal_status writer
                 * pairs it with state = DEAD) and was RUNNING (counted) when it
                 * died.  sched_post_dispatch — which owns the clean-death
                 * decrement — is deliberately NOT called on this path (it would
                 * eager-reap the strand the host wants to inspect), so the
                 * counted-set exit happens here via the owning helper.
                 * urbi_strand_reset (DEAD -> DORMANT) + urbi_strand_start
                 * (make_runnable, +1) re-balance if the host revives it. */
                urbi_sched_runnable_dec(vm, s);
                vm->fatal_strand = s;
                return URBI_STEP_FATAL;
            }

            /* Post-dispatch fix-ups — scheduler F3.
             *
             * sched_post_dispatch runs four bookkeeping steps that must execute after
             * every dispatch-loop iteration:
             *   1. Decrement strand_runnable_count if the strand died (SCHED-01:
             *      a DEAD strand was RUNNING and counted; parking transitions
             *      already decremented for WAITING/SUSPENDED).
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

        /* Step 4b: post-loop reactive drain (bounded/edge whenever).  Absorbs
         * dirty marks from flat strands that completed without a safepoint, so
         * a parked waituntil/at wakes and cascades resolve within this step. */
        runnable_before_drain = vm->strand_runnable_count;
        vm_reactive_drain(vm, /*bounded_whenever=*/1);
    } while (vm->strand_runnable_count > runnable_before_drain
             && vm->step_budget_remaining > 0);

    /* Post-loop verdict ladder (refactor-3 SCHED-13): one liveness formula.
     *
     * RUNNING   — runnable strands remain (budget ran out) OR pending
     *             internal work exists (ISR-ring events, host-injected
     *             cross-strand stops, dirty watcher evals, queued onleave
     *             drains) that the next call will perform (dirty/onleave
     *             drains run at dispatch safepoints until Task 5's idle
     *             pump — see usched_liveness.c).
     * WAKE_AT   — nothing runnable/pending now; a sleeper or live periodic
     *             has a future deadline (*out_next_wake_us).
     * QUIESCENT — no internal work at all.  lv.armed may be > 0: armed
     *             watchers and SUSPENDED/WAITING strands do NOT prevent
     *             QUIESCENT (owner decision 2026-06-11) — host slot writes,
     *             injected events, or tag unblock/unfreeze re-arm the VM.
     *             Use urbi_vm_has_live_work to distinguish a fully-dead VM
     *             from an armed-but-idle one. */
    UVmLiveness lv;
    urbi_vm_liveness(vm, &lv);
    if (lv.runnable > 0 || lv.pending > 0) return URBI_STEP_RUNNING;
    if (lv.timed) {
        if (out_next_wake_us) *out_next_wake_us = lv.next_wake_us;
        return URBI_STEP_WAKE_AT;
    }
    return URBI_STEP_QUIESCENT;   /* lv.armed may be > 0 — documented contract */
}
