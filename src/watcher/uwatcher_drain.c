/* SPDX-License-Identifier: BSD-3-Clause */
/* Watcher pending-onleave queue: push helper, run_watcher_onleave,
 * drain_pending_onleave_queue.
 * Reactive runtime landed in M5 (see docs/milestones/m5-reactive.md).
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
#include "tag/utag.h"           /* UTag, member_watchers_head */
#include "event/uevent_subscribe.h"  /* uevent_at_watchers_remove (W2/v0.10.2) */
#include "urbi/urbi.h"           /* URBI_ASSERT_NOT_ISR, URBI_LOG_WARN */
#include "runtime/umacros.h"  /* URBI_INTERNAL_ASSERT */
#include <stddef.h>

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

    if (vm->test_hooks != NULL && vm->test_hooks->watcher_onleave != NULL) {
        vm->test_hooks->watcher_onleave(vm, w);
        return;
    }

    /* Real bytecode dispatch on the scratch frame.  Throws are suppressed
     * (drain path cannot propagate; the watcher is being torn down). */
    UValue out = {0};
    int    threw = 0;
    (void)urbi_run_closure_on_scratch(vm, w->onleave, &out, &threw);
    if (threw && vm->host_log_fn) {
        vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
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
 *   3b. (W2/v0.10.2) For AT_EVENT/AT_EVENT_SYNC/WHENEVER_EVENT, synchronously
 *       unlink from event->at_watchers_head to close the drain-vs-emit window.
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

    /* Step 3b (W2/v0.10.2): AT_EVENT, AT_EVENT_SYNC, and WHENEVER_EVENT
     * watchers also thread on event->at_watchers_head via next_in_event.
     * Unlink synchronously here so that any c_event_emit_async/_sync call
     * that fires between this push and the next safepoint drain does NOT
     * dispatch a zombie body strand on the cancelled realm.
     *
     * The drain-vs-emit window is the primary hazard (reactive audit F2):
     * tag-stop cascade is most likely to coincide with event traffic, and
     * do_spawn_body_coroutine would allocate a strand on the cancelled
     * realm's strands_head and wire body->watcher_body_owner on a watcher
     * that is logically being torn down.
     *
     * URBI_WATCHER_PENDING_UNREGISTER (set in Step 1) provides defence in
     * depth; the synchronous unlink here is the primary fix.
     *
     * Closes reactive audit F2. */
    if ((w->mode == UWATCHER_AT_EVENT
         || w->mode == UWATCHER_AT_EVENT_SYNC
         || w->mode == UWATCHER_WHENEVER_EVENT)
        && w->event != NULL) {
        uevent_at_watchers_remove(w->event, w);
        w->event = NULL;
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
 *   2. Update pending_onleave_tail inline if the popped entry was the tail
 *      (onleave-orphan fix — BEFORE invoking the handler).
 *   3. If w->onleave != NULL: run_watcher_onleave.
 *   4. urbi_watcher_unregister_internal: clears bit-6 on read-set cells
 *      (scan-on-unregister skips w because it was already removed from
 *      active_watchers_head by pending_onleave_queue_push — the (o == w)
 *      guard in unregister is a no-op), then pool_frees the slot and
 *      decrements watcher_active_count.
 *
 * Called from the dispatcher safepoint BEFORE watcher_eval_dirty per spec §6.5.
 * Reuses vm->watchers->in_eval as a reentrancy guard (same scratch-frame contract
 * as watcher_eval_dirty — drain and eval are always sequential, never nested).
 *
 * SCHED-12: save/restore in_eval so a nested call from vm_reactive_drain
 * inside watcher_eval_dirty preserves the outer eval's in_eval=1.  The
 * previous URBI_INTERNAL_ASSERT(!in_eval) is removed — save/restore makes
 * nested calls safe; vm_reactive_drain's guard prevents unbounded nesting. */
void
drain_pending_onleave_queue(UVM *vm)
{
    URBI_ASSERT_NOT_ISR(vm);

    uint8_t saved_eval = vm->watchers->in_eval;
    vm->watchers->in_eval = 1;

    /* Spec #1 §6.3: iterate with a pointer-to-pointer walk so we can skip
     * (defer) entries whose body strand is still alive without disturbing
     * the FIFO order.  Deferred entries remain on the queue and are retried
     * at the next safepoint (bounded by URBI_CLEANUP_MAX latency).
     *
     * Tail invariant: updated inline (before invoking each handler) when the
     * popped entry was the tail, so a mid-drain push by the onleave handler
     * can correctly append via pending_onleave_tail (onleave-orphan fix). */
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

        /* Inline tail update (onleave-orphan fix): if w was the tail, rescan
         * from head BEFORE invoking the handler.  An onleave that pushes a new
         * entry appends to pending_onleave_tail; with the stale pre-pop tail
         * the append would orphan the new entry.  After the pop, head already
         * reflects the removal, so the scan finds the correct new tail. */
        if (w == vm->pending_onleave_tail) {
            if (vm->pending_onleave_head == NULL) {
                vm->pending_onleave_tail = NULL;
            } else {
                UWatcher *t = vm->pending_onleave_head;
                while (t->next_active != NULL) t = t->next_active;
                vm->pending_onleave_tail = t;
            }
        }

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

    vm->watchers->in_eval = saved_eval;
}
