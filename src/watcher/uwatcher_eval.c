/* SPDX-License-Identifier: BSD-3-Clause */
/* Watcher eval pass: watcher_eval_dirty, invoke_condition_closure.
 * Row 11.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * All allocation goes through vm->alloc_fn.
 *
 * Architectural notes:
 *   invoke_condition_closure: test hook short-circuits; otherwise routes to
 *     urbi_run_closure_on_scratch (spec §6.4 + §6.8).  Eval-time throws
 *     fail-soft as nil.
 *   spawn_body_coroutine: lives in uwatcher_spawn.c (Row 11). */

#include "uwatcher.h"
#include "uvm.h"
#include "value/uvalue.h"        /* uvalue_truthy */
#include "urbi/urbi.h"          /* URBI_ASSERT_NOT_ISR */
#include "umacros.h" /* URBI_INTERNAL_ASSERT */
#include "sched/usched_cooperative.h" /* sched_strand_make_runnable */

/* === invoke_condition_closure ===
 *
 * Evaluate watcher w's condition and return the result.  Test hook short-
 * circuits the dispatch path so existing fire-path tests can inject
 * specific values; otherwise routes to urbi_run_closure_on_scratch
 * (uwatcher_scratch.c).  Eval-time throws fail-soft as nil — watcher does
 * not fire this pass; caller (watcher_eval_dirty) does not propagate.
 *
 * Returns UVAL_NIL when w->condition is NULL (no-condition watchers fire
 * on dirty-mark only, not on cond eval). */
UValue
invoke_condition_closure(struct UVM *vm, struct UWatcher *w)
{
    UValue nil = {0};  /* kind = UVAL_NIL, payload zeroed */

    URBI_ASSERT_NOT_ISR(vm);

    if (w->condition == NULL) return nil;

    if (vm->test_watcher_condition_hook != NULL) {
        return vm->test_watcher_condition_hook(vm, w);
    }

    /* Real bytecode dispatch on the scratch frame.  Eval-time throws
     * fail-soft as nil — watcher does not fire this pass; caller
     * (watcher_eval_dirty) does not propagate. */
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
 * Preconditions (URBI_DEBUG asserted):
 *   - vm->in_watcher_eval == 1 (we are inside watcher_eval_dirty).
 *   - w->mode == UWATCHER_AT_SYNC.
 *   - w->body != NULL.                                             */
static void
invoke_body_inline(struct UVM *vm, struct UWatcher *w)
{
#ifdef URBI_DEBUG
    URBI_INTERNAL_ASSERT(vm->in_watcher_eval == 1);
    URBI_INTERNAL_ASSERT(w->mode == UWATCHER_AT_SYNC);
    URBI_INTERNAL_ASSERT(w->body != NULL);
#endif

    if (vm->test_watcher_fire_hook != NULL) {
        vm->test_watcher_fire_hook(vm, w);
        return;
    }

    /* Real bytecode dispatch on the scratch frame.  Body runs synchronously;
     * throws are suppressed (watcher does not propagate per spec §6.4 — eval
     * pass cannot raise to its caller). */
    UValue out = {0};
    int    threw = 0;
    (void)urbi_run_closure_on_scratch(vm, w->body, &out, &threw);
    if (threw && vm->host_log_fn) {
        vm->host_log_fn(vm, URBI_LOG_WARN,
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
 *   - vm->in_watcher_eval == 1.
 *   - w->onleave != NULL.                                         */
static void
invoke_onleave_inline(struct UVM *vm, struct UWatcher *w)
{
#ifdef URBI_DEBUG
    URBI_INTERNAL_ASSERT(vm->in_watcher_eval == 1);
    URBI_INTERNAL_ASSERT(w->onleave != NULL);
#endif

    if (vm->test_watcher_onleave_hook != NULL) {
        vm->test_watcher_onleave_hook(vm, w);
        return;
    }

    /* Real bytecode dispatch on the scratch frame.  Throws are suppressed
     * (watcher does not propagate per spec §6.4). */
    UValue out = {0};
    int    threw = 0;
    (void)urbi_run_closure_on_scratch(vm, w->onleave, &out, &threw);
    if (threw && vm->host_log_fn) {
        vm->host_log_fn(vm, URBI_LOG_WARN,
            "at(cond) onleave threw; suppressed");
    }
}

/* === watcher_eval_dirty ===
 *
 * Walk active_watchers_head, evaluate each watcher's condition, and fire
 * the appropriate action based on mode and edge detection:
 *
 *   AT        rising edge  → spawn_body_coroutine (async)
 *             falling edge + BODY_FIRED_SINCE_ONLEAVE + onleave != NULL
 *                          → invoke_onleave_inline; clear flag
 *   AT_SYNC   rising edge  → invoke_body_inline (synchronous, no yield)
 *   WHENEVER  level true   → spawn_body_coroutine each pass
 *   WAITUNTIL rising edge  → wake waiter_strand, unregister self
 *
 * Per spec #2 §8.3:
 *   - Early-exit if watcher_dirty_count == 0.
 *   - Reset watcher_dirty_count BEFORE the loop so any condition that
 *     re-triggers the dirty bit during eval is caught on the next safepoint.
 *   - in_watcher_eval reentrancy guard prevents recursive install/eval.
 *   - Watchers with URBI_WATCHER_PENDING_UNREGISTER are skipped.
 *   - last_value_cache updated after firing decision.
 *   - WAITUNTIL: capture next_active before calling unregister_internal
 *     (unregister frees the slot; the pointer is stale after the call). */
void
watcher_eval_dirty(struct UVM *vm)
{
    struct UWatcher *w;
    struct UWatcher *next;

    URBI_ASSERT_NOT_ISR(vm);

    if (vm->watcher_dirty_count == 0) return;

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

    vm->watcher_dirty_count = 0;

    URBI_INTERNAL_ASSERT(!vm->in_watcher_eval);
    vm->in_watcher_eval = 1;

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

        new_val = invoke_condition_closure(vm, w);
        old_val = w->last_value_cache;

        rising  = uvalue_truthy(&new_val) && !uvalue_truthy(&old_val);
        falling = !uvalue_truthy(&new_val) && uvalue_truthy(&old_val);

        switch (w->mode) {
            case UWATCHER_AT:
                if (rising) {
                    if (w->body != NULL) {
                        spawn_body_coroutine(vm, w);
                    } else if (vm->test_watcher_fire_hook != NULL) {
                        vm->test_watcher_fire_hook(vm, w);
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
                    } else if (vm->test_watcher_fire_hook != NULL) {
                        vm->test_watcher_fire_hook(vm, w);
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
                /* Level-triggered: fires every dirty pass while condition truthy. */
                if (uvalue_truthy(&new_val)) {
                    if (w->body != NULL) {
                        spawn_body_coroutine(vm, w);
                    } else if (vm->test_watcher_fire_hook != NULL) {
                        vm->test_watcher_fire_hook(vm, w);
                    }
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
                    sched_strand_make_runnable(waiter);
                    /* w is freed; skip the post-switch last_value_cache update.
                     * Advance directly to next. */
                    w = next;
                    continue;
                }
                w->last_value_cache = new_val;
                break;

            default:
                /* Unknown mode — update cache and skip. */
                w->last_value_cache = new_val;
                break;
        }

        w = next;
    }

    vm->in_watcher_eval = 0;
}

/* spawn_body_coroutine lives in uwatcher_spawn.c (Row 11).
 * Declaration is in uwatcher.h; watcher_eval_dirty calls it above. */
