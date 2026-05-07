/* SPDX-License-Identifier: BSD-3-Clause */
/* Watcher pending-onleave queue: push helper, run_watcher_onleave stub,
 * drain_pending_onleave_queue.
 * Row 11.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * All allocation goes through vm->alloc_fn.
 *
 * Queue semantics
 * ---------------
 * pending_onleave_queue is a singly-linked FIFO threaded via next_active.
 * A watcher cannot be on both active_watchers_head and pending_onleave_queue
 * simultaneously — push TRANSFERS the entry: unlinks from active_watchers_head
 * and from owning_tag->member_watchers_head, then appends to the tail of the
 * pending queue.  The URBI_WATCHER_PENDING_UNREGISTER flag is set at push time
 * so any concurrent watcher_eval_dirty pass skips the watcher.
 *
 * watcher_active_count is NOT decremented at push time; it is decremented by
 * urbi_watcher_unregister_internal (called by drain) when the watcher is truly
 * gone from the system.  This keeps quiescence accounting simple: a watcher on
 * the pending queue still counts as "active" until drained.
 *
 * run_watcher_onleave
 * -------------------
 * Routes through urbi_run_closure_on_scratch (uwatcher_scratch.c) for real
 * bytecode dispatch on the scratch frame.  Test hook short-circuits the
 * dispatch path so existing fire-path tests can inject behavior without
 * going through real bytecode.  Throws are suppressed (drain path cannot
 * propagate; the watcher is being torn down).
 *
 * Drain ordering contract
 * -----------------------
 * drain_pending_onleave_queue is called by the dispatcher safepoint BEFORE
 * watcher_eval_dirty.  If an onleave handler mutates a cell that is in another
 * watcher's read-set, the resulting dirty-count increment will be picked up by
 * watcher_eval_dirty in the same safepoint tick.  Per spec §6.5. */

#include "uwatcher.h"
#include "vm/uvm.h"
#include "utag.h"           /* UTag, member_watchers_head */
#include "urbi/urbi.h"           /* URBI_ASSERT_NOT_ISR, URBI_LOG_WARN */
#include "runtime/umacros.h"  /* URBI_INTERNAL_ASSERT */

/* === run_watcher_onleave — file-scope static ===
 *
 * Execute w->onleave on the VM scratch frame via real bytecode dispatch.
 * Test hook short-circuits the dispatch path; otherwise routes to
 * urbi_run_closure_on_scratch (uwatcher_scratch.c).  Throws are suppressed
 * (drain path cannot propagate; the watcher is being torn down).
 * Precondition: w->onleave != NULL (caller must check).
 * run_watcher_onleave inherits the ISR-safety guarantee from drain's guard —
 * no separate URBI_ASSERT_NOT_ISR needed here. */
static void
run_watcher_onleave(UVM *vm, UWatcher *w)
{
    URBI_INTERNAL_ASSERT(w != NULL);
    URBI_INTERNAL_ASSERT(w->onleave != NULL);

    if (vm->test_watcher_onleave_hook != NULL) {
        vm->test_watcher_onleave_hook(vm, w);
        return;
    }

    /* Real bytecode dispatch on the scratch frame.  Throws are suppressed
     * (drain path cannot propagate; the watcher is being torn down). */
    UValue out = {0};
    int    threw = 0;
    (void)urbi_run_closure_on_scratch(vm, w->onleave, &out, &threw);
    if (threw && vm->host_log_fn) {
        vm->host_log_fn(vm, URBI_LOG_WARN,
            "tag-stop drain onleave threw; suppressed");
    }
}

/* === pending_onleave_queue_push ===
 *
 * Transfer watcher w from the active lists into the pending-onleave FIFO.
 *
 * Steps (see module header for rationale):
 *   1. Set URBI_WATCHER_PENDING_UNREGISTER so eval pass skips.
 *   2. Unlink from vm->active_watchers_head (pointer-to-pointer walk).
 *   3. Unlink from w->owning_tag->member_watchers_head (NULL-guarded).
 *   4. Append to pending_onleave_queue tail (set next_active = NULL). */
void
pending_onleave_queue_push(UVM *vm, UWatcher *w)
{
    UWatcher **pp;

    URBI_ASSERT_NOT_ISR(vm);
    URBI_INTERNAL_ASSERT(w != NULL);

    /* Step 1: mark pending so eval pass skips during drain. */
    w->flags |= URBI_WATCHER_PENDING_UNREGISTER;

    /* Step 2: unlink from active_watchers_head. */
    pp = &vm->active_watchers_head;
    while (*pp != NULL) {
        if (*pp == w) {
            *pp = w->next_active;
            break;
        }
        pp = &(*pp)->next_active;
    }

    /* Step 3: unlink from owning_tag->member_watchers_head.
     * This satisfies utag_destroy's precondition that member_watchers_head
     * is empty — the push removes the watcher before the tag is destroyed. */
    if (w->owning_tag != NULL) {
        UWatcher **prev = &w->owning_tag->member_watchers_head;
        while (*prev != NULL && *prev != w) prev = &(*prev)->next_in_tag;
        if (*prev == w) *prev = w->next_in_tag;
    }

    /* Step 4: append to FIFO tail (next_active becomes the queue threading
     * field while the watcher is in the pending queue). */
    w->next_active = NULL;
    if (vm->pending_onleave_tail != NULL) {
        vm->pending_onleave_tail->next_active = w;
        vm->pending_onleave_tail = w;
    } else {
        vm->pending_onleave_head = w;
        vm->pending_onleave_tail = w;
    }
    /* watcher_active_count is NOT decremented here; urbi_watcher_unregister_internal
     * (called by drain) performs the single decrement when the watcher is truly gone.
     * This keeps quiescence semantics simple: pending entries still count as active. */
}

/* === drain_pending_onleave_queue ===
 *
 * Drain the entire pending-onleave FIFO in FIFO order.  For each watcher:
 *   1. Pop from queue head.
 *   2. If w->onleave != NULL: run_watcher_onleave.
 *   3. urbi_watcher_unregister_internal: clears bit-6 on read-set cells
 *      (scan-on-unregister skips w because it was already removed from
 *      active_watchers_head by pending_onleave_queue_push — the (o == w)
 *      guard in unregister is a no-op), then pool_frees the slot and
 *      decrements watcher_active_count.
 *
 * Called from the dispatcher safepoint BEFORE watcher_eval_dirty per spec §6.5.
 * Reuses vm->in_watcher_eval as a reentrancy guard (same scratch-frame contract
 * as watcher_eval_dirty — drain and eval are always sequential, never nested). */
void
drain_pending_onleave_queue(UVM *vm)
{
    URBI_ASSERT_NOT_ISR(vm);
    URBI_INTERNAL_ASSERT(!vm->in_watcher_eval);

    vm->in_watcher_eval = 1;

    /* Spec #1 §6.3: iterate with a pointer-to-pointer walk so we can skip
     * (defer) entries whose body strand is still alive without disturbing
     * the FIFO order.  Deferred entries remain on the queue and are retried
     * at the next safepoint (bounded by URBI_CLEANUP_MAX latency).
     *
     * tail pointer invariant: after the loop, pending_onleave_tail must point
     * to the last remaining entry, or NULL if the queue is empty.
     * We recompute tail in a single forward scan after the main loop rather
     * than trying to maintain it inline during removal — the queue is short
     * (bounded by URBI_WATCHER_POOL_SIZE) and this drain runs at safepoints. */
    UWatcher **pp = &vm->pending_onleave_head;
    while (*pp != NULL) {
        UWatcher *w = *pp;

        if (w->body_strand != NULL) {
            /* Body still running — defer: leave on queue, advance pointer. */
            pp = &w->next_active;
            continue;
        }

        /* Pop from queue (pp already points to the right link field). */
        *pp = w->next_active;
        w->next_active = NULL;

        /* Run onleave handler if present. */
        if (w->onleave != NULL) {
            run_watcher_onleave(vm, w);
        }

        /* Unregister: bit-6 scan, member_watchers_head unlink (no-op — PUSH
         * already removed from tag list), active_watchers_head unlink (no-op —
         * PUSH already removed from active list), pool_free, decrement counter. */
        urbi_watcher_unregister_internal(vm, w);
        /* pp is unchanged — now points to what was w->next_active, i.e.
         * the next entry (already advanced by the *pp = w->next_active above). */
    }

    /* Recompute tail: scan from head to find the last entry (O(n), but n is
     * small and this only runs when at least one entry was present). */
    if (vm->pending_onleave_head == NULL) {
        vm->pending_onleave_tail = NULL;
    } else {
        UWatcher *t = vm->pending_onleave_head;
        while (t->next_active != NULL) t = t->next_active;
        vm->pending_onleave_tail = t;
    }

    vm->in_watcher_eval = 0;
}
