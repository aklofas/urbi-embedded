/* SPDX-License-Identifier: BSD-3-Clause */
/* uevent_subscribe: at_watchers FIFO list helpers (internal).
 * Spec #3 §6.1 + §6.3.
 *
 * Iteration-during-emit safety (EVENT-023):
 *   uevent_at_watchers_append walks the chain to find the tail; remove walks
 *   the chain to find the target.  Walking is safe when an emit is concurrent
 *   on the same event because the producer-side emit (c_event_emit_async /
 *   c_event_emit_sync in src/event/uevent_emit.c) snapshots `next_in_event`
 *   BEFORE invoking each subscriber — see the `next = w->next_in_event;`
 *   captures at the top of each emit's per-watcher loop iteration.  A
 *   subscriber removed during emit therefore can't yank the iterator's next
 *   link out from under it.  New subscribers added during emit don't fire
 *   until the next emit cycle (they land at tail; emit's snapshot of the
 *   prior iteration's next has already moved past).
 *
 * Single-threaded VM contract: this assumes URBI_SCHED_COOPERATIVE.
 * Preemptive scheduling (v1.x URBI_SCHED_PREEMPTIVE) needs an explicit lock
 * around at_watchers_head — flagged in design-risks. */

#ifndef UEVENT_SUBSCRIBE_H
#define UEVENT_SUBSCRIBE_H

#ifdef __cplusplus
extern "C" {
#endif

struct UEvent;
struct UWatcher;

/* uevent_at_watchers_append: tail-append w to e->at_watchers_head.
 * Sets w->next_in_event = NULL.  O(N) walk; N is small and installs rare. */
void uevent_at_watchers_append(struct UEvent *e, struct UWatcher *w);

/* uevent_at_watchers_remove: removes target from the event's watcher list.
 * Clears target->next_in_event only when target was found in the list; if
 * not found (caller error or already-removed elsewhere), the field is left
 * as-is.  Caller is responsible for clearing if it needs a known-NULL
 * invariant.  EVENT-010. */
void uevent_at_watchers_remove(struct UEvent *e, struct UWatcher *target);

#ifdef __cplusplus
}
#endif

#endif /* UEVENT_SUBSCRIBE_H */
