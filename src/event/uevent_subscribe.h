/* SPDX-License-Identifier: BSD-3-Clause */
/* uevent_subscribe: at_watchers FIFO list helpers (internal).
 * Spec #3 §6.1 + §6.3. */

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
