/* SPDX-License-Identifier: BSD-3-Clause */
/* Watcher eval pass: urbi_vm_watcher_eval_dirty, urbi_watcher_invoke_condition_closure.
 * Reactive runtime landed in M5 (see docs/milestones/m5-reactive.md).
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * All allocation goes through vm->alloc_fn.
 *
 * Architectural notes:
 *   urbi_watcher_invoke_condition_closure: test hook short-circuits; otherwise routes to
 *     urbi_run_closure_on_scratch (spec §6.4 + §6.8).  Eval-time throws
 *     fail-soft as nil.
 *   urbi_watcher_spawn_body_coroutine: lives in uwatcher_spawn.c. */

#include "uwatcher.h"
#include "runtime/uscratch.h"
#include "vm/uvm.h"
#include "value/uvalue.h"        /* uvalue_truthy */
#include "urbi/urbi.h"          /* URBI_ASSERT_NOT_ISR */
#include "runtime/umacros.h" /* URBI_INTERNAL_ASSERT */
#include "sched/usched_cooperative.h" /* urbi_sched_strand_make_runnable */
#include <stddef.h>
#include <stdint.h>

/* === urbi_watcher_invoke_condition_closure ===
 *
 * Evaluate watcher w's condition and return the result.  Test hook short-
 * circuits the dispatch path so existing fire-path tests can inject
 * specific values; otherwise routes to urbi_run_closure_on_scratch
 * (src/runtime/uscratch.c).  Eval-time throws fail-soft as nil — watcher does
 * not fire this pass; caller (urbi_vm_watcher_eval_dirty) does not propagate.
 *
 * Returns UVAL_NIL when w->condition is NULL (no-condition watchers fire
 * on dirty-mark only, not on cond eval). */
UValue
urbi_watcher_invoke_condition_closure(struct UVM *vm, struct UWatcher *w)
{
    UValue nil = {0};  /* kind = UVAL_NIL, payload zeroed */

    URBI_ASSERT_NOT_ISR(vm);

    if (w->condition == NULL) return nil;

    if (vm->test_hooks != NULL && vm->test_hooks->watcher_condition != NULL) {
        return vm->test_hooks->watcher_condition(vm, w);
    }

    /* Real bytecode dispatch on the scratch frame.  Eval-time throws
     * fail-soft as nil — watcher does not fire this pass; caller
     * (urbi_vm_watcher_eval_dirty) does not propagate. */
    UValue out = {0};
    int    threw = 0;
    (void)urbi_run_closure_on_scratch(vm, w->condition, &out, &threw);
    if (threw) return nil;
    return out;
}

/* === invoke_body_inline ===
 *
 * Run w->body synchronously on the VM scratch frame (AT_SYNC mode).
 * Test hook short-circuits the dispatch path; otherwise routes to
 * urbi_run_closure_on_scratch.  Throws are suppressed (watcher does
 * not propagate per spec §6.4 no-yield contract).
 *
 * No-yield contract (WATCH-031):
 *   AT_SYNC bodies MUST NOT yield — sync bodies run to completion
 *   inside the eval pass.  A body that yields (calls into a host
 *   function that waits, hits a sleep, etc.) is a contract violation;
 *   the runtime currently suppresses any throw with a warn ("at
 *   sync(cond) body threw; suppressed") but does NOT auto-degrade
 *   the body to async re-spawn — that "degrade-to-async" policy is
 *   a v1.x design item.  No-yield is a policy contract, not a static
 *   guarantee — the runtime can't prove a body is yield-free at
 *   install time, so user-authored AT_SYNC bodies must respect the
 *   contract.
 *
 * Preconditions (URBI_DEBUG asserted):
 *   - vm->watchers->in_eval == 1 (we are inside urbi_vm_watcher_eval_dirty).
 *   - w->mode == UWATCHER_AT_SYNC.
 *   - w->body != NULL.                                             */
static void
invoke_body_inline(struct UVM *vm, struct UWatcher *w)
{
#ifdef URBI_DEBUG
    URBI_INTERNAL_ASSERT(vm->watchers->in_eval == 1);
    URBI_INTERNAL_ASSERT(w->mode == UWATCHER_AT_SYNC);
    URBI_INTERNAL_ASSERT(w->body != NULL);
#endif

    if (vm->test_hooks != NULL && vm->test_hooks->watcher_fire != NULL) {
        vm->test_hooks->watcher_fire(vm, w);
        return;
    }

    /* Real bytecode dispatch on the scratch frame.  Body runs synchronously;
     * throws are suppressed (watcher does not propagate per spec §6.4 — eval
     * pass cannot raise to its caller). */
    UValue out = {0};
    int    threw = 0;
    (void)urbi_run_closure_on_scratch(vm, w->body, &out, &threw);
    if (threw && vm->host_log_fn) {
        vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
            "at sync(cond) body threw; suppressed");
    }
}

/* === invoke_onleave_inline ===
 *
 * Run w->onleave synchronously on the VM scratch frame (falling-edge path).
 * Test hook short-circuits the dispatch path; otherwise routes to
 * urbi_run_closure_on_scratch.  Throws are suppressed (watcher does
 * not propagate per spec §6.4 no-yield contract).
 *
 * Preconditions (URBI_DEBUG asserted):
 *   - vm->watchers->in_eval == 1.
 *   - w->onleave != NULL.                                         */
static void
invoke_onleave_inline(struct UVM *vm, struct UWatcher *w)
{
#ifdef URBI_DEBUG
    URBI_INTERNAL_ASSERT(vm->watchers->in_eval == 1);
    URBI_INTERNAL_ASSERT(w->onleave != NULL);
#endif

    if (vm->test_hooks != NULL && vm->test_hooks->watcher_onleave != NULL) {
        vm->test_hooks->watcher_onleave(vm, w);
        return;
    }

    /* Real bytecode dispatch on the scratch frame.  Throws are suppressed
     * (watcher does not propagate per spec §6.4). */
    UValue out = {0};
    int    threw = 0;
    (void)urbi_run_closure_on_scratch(vm, w->onleave, &out, &threw);
    if (threw && vm->host_log_fn) {
        vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
            "at(cond) onleave threw; suppressed");
    }
}

/* === urbi_vm_watcher_eval_dirty ===
 *
 * Walk active_watchers_head, evaluate each watcher's condition, and fire
 * the appropriate action based on mode and edge detection:
 *
 *   AT        rising edge  → urbi_watcher_spawn_body_coroutine (async)
 *             falling edge + BODY_FIRED_SINCE_ONLEAVE + onleave != NULL
 *                          → invoke_onleave_inline; clear flag
 *   AT_SYNC   rising edge  → invoke_body_inline (synchronous, no yield)
 *   WHENEVER  level true   → urbi_watcher_spawn_body_coroutine each pass
 *   WAITUNTIL rising edge  → wake waiter_strand, unregister self
 *
 * Per spec #2 §8.3:
 *   - Early-exit if vm->watchers->dirty_count == 0.
 *   - Reset vm->watchers->dirty_count BEFORE the loop so any condition
 *     that re-triggers the dirty bit during eval is caught on the next
 *     safepoint.
 *   - vm->watchers->in_eval reentrancy guard prevents recursive install/eval.
 *   - Watchers with URBI_WATCHER_PENDING_UNREGISTER are skipped.
 *   - last_value_cache updated after firing decision.
 *   - WAITUNTIL: capture next_active before calling unregister_internal
 *     (unregister frees the slot; the pointer is stale after the call). */
void
urbi_vm_watcher_eval_dirty(struct UVM *vm)
{
    struct UWatcher *w;
    struct UWatcher *next;

    URBI_ASSERT_NOT_ISR(vm);

    if (vm->watchers->dirty_count == 0) return;

#ifdef URBI_DEBUG
    urbi_watcher_check_invariants(vm);

    /* Spec #2 §8.7: WAITUNTIL invariant — bidirectional contract.
     * Every WAITUNTIL watcher must have waiter_strand != NULL.
     * Every non-NULL waiter_strand must be in USTRAND_WAIT_WATCHER state. */
    for (w = vm->active_watchers_head; w != NULL; w = w->next_active) {
        URBI_INTERNAL_ASSERT(
            w->mode != UWATCHER_WAITUNTIL || w->waiter_strand != NULL);
        URBI_INTERNAL_ASSERT(
            w->waiter_strand == NULL ||
            w->waiter_strand->state == USTRAND_WAIT_WATCHER);
    }
#endif

    vm->watchers->dirty_count = 0;
    /* Advance pass generation for rescan-idempotency stamp.  Skip zero so
     * that freshly allocated (or recycled) watchers — whose eval_pass_gen
     * is reset to 0 by uwatcher_pool_alloc — are never falsely considered
     * already-visited on their first pass.  Wrap-around safe: comparison
     * uses ==; one increment per urbi_vm_watcher_eval_dirty call. */
    vm->watchers->eval_pass_gen = (uint8_t)(vm->watchers->eval_pass_gen + 1u);
    if (vm->watchers->eval_pass_gen == 0) vm->watchers->eval_pass_gen = 1;
    uint8_t cur_pass_gen = vm->watchers->eval_pass_gen;

    /* Save/restore in_eval (SCHED-12): in normal flow vm_reactive_drain's
     * guard ensures in_eval=0 on entry; the ASSERT pins that contract.
     * Restore on exit rather than hard-set-to-0 for defensive symmetry with
     * urbi_watcher_drain_pending_onleave_queue's save/restore. */
    uint8_t saved_eval = vm->watchers->in_eval;
    URBI_INTERNAL_ASSERT(!vm->watchers->in_eval);
    vm->watchers->in_eval = 1;

    w = vm->active_watchers_head;
    while (w != NULL) {
        UValue new_val;
        UValue old_val;
        int rising;
        int falling;

        /* Capture next before any potential unregister (WAITUNTIL path frees w). */
        next = w->next_active;

        if (w->flags & URBI_WATCHER_PENDING_UNREGISTER) {
            w = next;
            continue;
        }

        /* Idempotency guard (rescan after cascade): skip watchers already
         * evaluated in this pass.  The rescan at the bottom of the loop
         * restarts from the head when a pre-captured successor carries
         * PENDING_UNREGISTER; without the stamp a level-triggered WHENEVER
         * fires a second body on the re-visit. */
        if (w->eval_pass_gen == cur_pass_gen) {
            w = next;
            continue;
        }
        w->eval_pass_gen = cur_pass_gen;

        new_val = urbi_watcher_invoke_condition_closure(vm, w);
        old_val = w->last_value_cache;

        rising  = uvalue_truthy(&new_val) && !uvalue_truthy(&old_val);
        falling = !uvalue_truthy(&new_val) && uvalue_truthy(&old_val);

        switch (w->mode) {
            case UWATCHER_AT:
                if (rising) {
                    if (w->body != NULL) {
                        urbi_watcher_spawn_body_coroutine(vm, w);
                    } else if (vm->test_hooks != NULL
                               && vm->test_hooks->watcher_fire != NULL) {
                        vm->test_hooks->watcher_fire(vm, w);
                    }
                    w->flags |= URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE;
                }
                if (falling &&
                    (w->flags & URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE) &&
                    w->onleave != NULL) {
                    invoke_onleave_inline(vm, w);
                    w->flags &= (uint8_t)~(uint8_t)URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE;
                }
                w->last_value_cache = new_val;
                break;

            case UWATCHER_AT_SYNC:
                if (rising) {
                    if (w->body != NULL) {
                        invoke_body_inline(vm, w);
                    } else if (vm->test_hooks != NULL
                               && vm->test_hooks->watcher_fire != NULL) {
                        vm->test_hooks->watcher_fire(vm, w);
                    }
                    w->flags |= URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE;
                }
                if (falling &&
                    (w->flags & URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE) &&
                    w->onleave != NULL) {
                    invoke_onleave_inline(vm, w);
                    w->flags &= (uint8_t)~(uint8_t)URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE;
                }
                w->last_value_cache = new_val;
                break;

            case UWATCHER_WHENEVER:
                /* Level vs edge gate (SCHED-02 storm fix).
                 *
                 * Active-dispatch path (whenever_edge_only == 0, the default —
                 * dispatcher safepoint / post-native / operator-fallback drains):
                 * level-triggered, fires every dirty pass while truthy.  The
                 * number of fires is bounded by the number of safepoints during
                 * active execution, which is finite.
                 *
                 * Idle / boundary path (whenever_edge_only == 1 — urbi_step's
                 * pre-loop idle drain and post-loop Step-4b drain): fire only on
                 * the rising edge (false->true).  urbi_watcher_observer_dirty is cell-agnostic
                 * (uwatcher.c), so a whenever body that writes ANY slot on an
                 * object whose cell carries the OBSERVER bit re-dirties the global
                 * dirty_count — including the body's write to its OWN observed
                 * object.  Under the idle/boundary drain (which re-runs whenever
                 * the VM is otherwise quiescent), level-firing would let such a
                 * self-re-dirty spin unboundedly (the reverted-S46 storm; see
                 * tests/unit/test_whenever_double_fire.c).  Gating on the rising
                 * edge breaks the loop: once the whenever fires, last_value_cache
                 * becomes truthy, so no subsequent idle pass sees an edge.
                 *
                 * Termination argument: urbi_vm_watcher_eval_dirty resets dirty_count to 0
                 * on entry.  In the idle/boundary drain a whenever fires at most
                 * once per rising edge; after firing, old==truthy so `rising` is
                 * false on every later pass.  With no fire, no body spawns, so
                 * nothing re-dirties; dirty_count stays 0 and the VM quiesces.
                 * Total idle fires are bounded by the number of genuine
                 * false->true cond transitions, which is finite for any script. */
                if (vm->watchers->whenever_edge_only ? rising
                                                     : uvalue_truthy(&new_val)) {
                    if (w->body != NULL) {
                        urbi_watcher_spawn_body_coroutine(vm, w);
                    } else if (vm->test_hooks != NULL
                               && vm->test_hooks->watcher_fire != NULL) {
                        vm->test_hooks->watcher_fire(vm, w);
                    }
                    w->flags |= URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE;
                }
                /* W9: falling-edge else_body / onleave dispatch.
                 * The `onleave` slot doubles as the else_body closure when the
                 * parser compiles `whenever (cond) body else else_body` —
                 * urbi_emit_watcher_arm stores else_body in register C of
                 * OP_WHENEVER_INSTALL, which is the onleave slot.
                 * URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE gate prevents the
                 * else from firing at watcher install time when the condition
                 * starts false (mirrors AT's onleave guard). */
                if (falling
                    && (w->flags & URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE)
                    && w->onleave != NULL) {
                    invoke_onleave_inline(vm, w);
                    w->flags &= (uint8_t)~(uint8_t)URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE;
                }
                w->last_value_cache = new_val;
                break;

            case UWATCHER_WAITUNTIL:
                if (rising) {
                    struct UStrand *waiter = w->waiter_strand;
                    /* Clear waiter pointer BEFORE unregister so the unregister
                     * scan does not observe a stale pointer. */
                    w->waiter_strand = NULL;
                    /* urbi_watcher_unregister_internal unlinks from active list
                     * and returns w to the pool — do NOT touch w after this. */
                    urbi_watcher_unregister_internal(vm, w);
                    /* Wake the blocked strand: WAIT_WATCHER → READY. */
                    urbi_sched_strand_make_runnable(waiter);
                    /* w is freed; skip the post-switch last_value_cache update.
                     * Advance directly to next. */
                    w = next;
                    continue;
                }
                w->last_value_cache = new_val;
                break;

            default:
                /* WATCH-016 (v0.5.7): unknown mode is a structural invariant
                 * violation — pool_alloc + install paths only ever set mode to
                 * one of UWATCHER_AT / _WHENEVER / _AT_SYNC / _WAITUNTIL /
                 * _AT_EVENT / _AT_EVENT_SYNC.  An out-of-range value at this
                 * point indicates memory corruption or a future caller
                 * forgetting to gate a new mode value through the eval switch.
                 * URBI_INTERNAL_ASSERT aborts in URBI_DEBUG; release builds
                 * fall through to update the cache (legacy soft-fail behavior)
                 * so production stays running. */
                URBI_INTERNAL_ASSERT(0 && "urbi_vm_watcher_eval_dirty: unknown watcher mode");
                w->last_value_cache = new_val;
                break;
        }

        /* SCH4-04: if the body just cascaded our pre-captured successor
         * (e.g. an AT_SYNC body called tag.stop() on the tag that owns
         * the next watcher), that successor got PENDING_UNREGISTER set
         * and its next_active cleared.  Following the stale pointer would
         * silently truncate the pass — every watcher after it misses a
         * same-tick fire.  Restart from the head instead.
         *
         * Safety: the next->flags read is safe because
         * urbi_watcher_drain_pending_onleave_queue runs at the dispatcher safepoint
         * BEFORE urbi_vm_watcher_eval_dirty (uwatcher_drain.c:33-36) — nothing
         * pushed during a body is freed until after the eval loop exits.
         *
         * Termination: PENDING is a one-way transition, so rescans are
         * bounded by the watcher count; the eval-pass stamp prevents
         * double-fire on re-visited watchers. */
        if (next != NULL &&
                (next->flags & URBI_WATCHER_PENDING_UNREGISTER) != 0U) {
            next = vm->active_watchers_head;
        }

        w = next;
    }

    vm->watchers->in_eval = saved_eval;
}

/* urbi_watcher_spawn_body_coroutine lives in uwatcher_spawn.c.
 * Declaration is in uwatcher.h; urbi_vm_watcher_eval_dirty calls it above. */
