/* SPDX-License-Identifier: BSD-3-Clause */
/* uevent_subscribe: at_watchers FIFO list helpers for UEvent.
 * Spec #3 §6.1 + §6.3.
 *
 * Two operations on UEvent.at_watchers_head (linked via UWatcher.next_in_event):
 *   uevent_at_watchers_append — tail-append (O(N); N is small, installs rare)
 *   uevent_at_watchers_remove — prev-pointer unlink
 *
 * These are internal helpers; not part of the public include/urbi/ API. */

#include "event/uevent_subscribe.h"
#include "event/uevent.h"
#include "watcher/uwatcher.h"  /* UWatcher, next_in_event */

void
uevent_at_watchers_append(UEvent *e, UWatcher *w)
{
    w->next_in_event = NULL;
    if (!e->at_watchers_head) {
        e->at_watchers_head = w;
        return;
    }
    UWatcher *t = e->at_watchers_head;
    while (t->next_in_event) t = t->next_in_event;
    t->next_in_event = w;
}

void
uevent_at_watchers_remove(UEvent *e, UWatcher *target)
{
    UWatcher **prev = &e->at_watchers_head;
    UWatcher  *cur  = e->at_watchers_head;
    while (cur && cur != target) {
        prev = &cur->next_in_event;
        cur  = cur->next_in_event;
    }
    if (cur) {
        *prev = cur->next_in_event;
        cur->next_in_event = NULL;
    }
}
