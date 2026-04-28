/* SPDX-License-Identifier: BSD-3-Clause */
/* Watcher eval pass: watcher_eval_dirty, invoke_condition_closure.
 * Row 11.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * All allocation goes through vm->alloc_fn.
 *
 * M3 architectural notes:
 *   invoke_condition_closure: calls vm->test_watcher_condition_hook if
 *     non-NULL; otherwise returns UVAL_NIL.  M5 will wire real bytecode
 *     execution via urbi_run_closure_on_scratch (spec §6.4 + §6.8).
 *   spawn_body_coroutine: lives in uwatcher_spawn.c (Row 11).
 *     M3 stub delegates to vm->test_watcher_fire_hook if non-NULL; otherwise
 *     no-op.  M5 owns the real strand-pool spawn path. */

#include "uwatcher.h"
#include "uvm.h"
#include "uvalue.h"        /* uvalue_truthy */
#include "urbi.h"          /* URBI_ASSERT_NOT_ISR */
#include "urbi_internal.h" /* URBI_INTERNAL_ASSERT */

/* === invoke_condition_closure ===
 *
 * Evaluate watcher w's condition and return the result.
 *
 * Per spec §6.4 M3-stub contract:
 *   1. If w->condition == NULL: return UVAL_NIL.
 *   2. Else if vm->test_watcher_condition_hook != NULL: delegate to hook.
 *   3. Else: return UVAL_NIL (graceful degradation; watcher installed with a
 *      real condition closure but no test hook simply yields nil — never fires
 *      by edge semantics, and only fires by level if the hook returns truthy). */
UValue
invoke_condition_closure(struct UVM *vm, struct UWatcher *w)
{
    UValue nil = {0};  /* kind = UVAL_NIL, payload zeroed */

    URBI_ASSERT_NOT_ISR(vm);

    if (w->condition == NULL) return nil;

    if (vm->test_watcher_condition_hook != NULL) {
        return vm->test_watcher_condition_hook(vm, w);
    }

    return nil;
}

/* === watcher_eval_dirty ===
 *
 * Walk active_watchers_head, evaluate each watcher's condition, and fire
 * spawn_body_coroutine on rising-edge (AT/AT_SYNC) or level (WHENEVER).
 *
 * Per spec §6.2:
 *   - Early-exit if watcher_dirty_count == 0.
 *   - Reset watcher_dirty_count BEFORE the loop so any condition that
 *     re-triggers the dirty bit during eval is caught on the next safepoint.
 *   - in_watcher_eval reentrancy guard prevents recursive install/eval.
 *   - Watchers with URBI_WATCHER_PENDING_UNREGISTER are skipped. */
void
watcher_eval_dirty(struct UVM *vm)
{
    struct UWatcher *w;

    URBI_ASSERT_NOT_ISR(vm);

    if (vm->watcher_dirty_count == 0) return;
    vm->watcher_dirty_count = 0;

    URBI_INTERNAL_ASSERT(!vm->in_watcher_eval);
    vm->in_watcher_eval = 1;

    for (w = vm->active_watchers_head; w != NULL; w = w->next_active) {
        UValue new_val;
        UValue old_val;
        int fire;

        if (w->flags & URBI_WATCHER_PENDING_UNREGISTER) continue;

        new_val = invoke_condition_closure(vm, w);
        old_val = w->last_value_cache;
        w->last_value_cache = new_val;

        fire = 0;
        switch (w->mode) {
            case UWATCHER_AT:
            case UWATCHER_AT_SYNC:
                /* Rising-edge: fires only on false→true transition. */
                fire = uvalue_truthy(&new_val) && !uvalue_truthy(&old_val);
                break;
            case UWATCHER_WHENEVER:
                /* Level: fires every dirty pass while condition is truthy. */
                fire = uvalue_truthy(&new_val);
                break;
            default:
                break;
        }

        if (fire) spawn_body_coroutine(vm, w);
    }

    vm->in_watcher_eval = 0;
}

/* spawn_body_coroutine lives in uwatcher_spawn.c (Row 11).
 * Declaration is in uwatcher.h; watcher_eval_dirty calls it above. */
