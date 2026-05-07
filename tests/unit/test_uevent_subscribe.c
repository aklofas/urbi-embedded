/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: uevent at_watchers FIFO helpers (T48).
 * Spec #3 §6.1 + §6.3.
 *
 * Cases:
 *   1. uevent_at_watchers_append_preserves_fifo:
 *      Three appends produce w1→w2→w3 in order; w3->next_in_event is NULL.
 *   2. uevent_at_watchers_remove_unlinks:
 *      Append w1 + w2; remove w1; head becomes w2; w1->next_in_event cleared. */

#include "utest.h"

#include "event/uevent.h"
#include "event/uevent_subscribe.h"
#include "watcher/uwatcher.h"
#include "vm/uvm.h"

#include <stddef.h>

#define UTEST(name) static void name(void)

/* === helper: allocate a bare watcher from the pool ====================== */

static UWatcher *
make_watcher(UVM *vm)
{
    UWatcher *w = uwatcher_pool_alloc(vm);
    /* Clear event fields so we start from a known state. */
    if (w) {
        w->next_in_event = NULL;
        w->event         = NULL;
    }
    return w;
}

/* ===== Test 1: append preserves FIFO order ============================= */

UTEST(uevent_at_watchers_append_preserves_fifo)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    UWatcher *w1 = make_watcher(&vm);
    UWatcher *w2 = make_watcher(&vm);
    UWatcher *w3 = make_watcher(&vm);
    UASSERT(w1 != NULL);
    UASSERT(w2 != NULL);
    UASSERT(w3 != NULL);

    uevent_at_watchers_append(e, w1);
    uevent_at_watchers_append(e, w2);
    uevent_at_watchers_append(e, w3);

    UASSERT(e->at_watchers_head == w1);
    UASSERT(w1->next_in_event   == w2);
    UASSERT(w2->next_in_event   == w3);
    UASSERT(w3->next_in_event   == NULL);

    uvm_destroy(&vm);
}

/* ===== Test 2: remove unlinks the target ================================ */

UTEST(uevent_at_watchers_remove_unlinks)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    UWatcher *w1 = make_watcher(&vm);
    UWatcher *w2 = make_watcher(&vm);
    UASSERT(w1 != NULL);
    UASSERT(w2 != NULL);

    uevent_at_watchers_append(e, w1);
    uevent_at_watchers_append(e, w2);

    uevent_at_watchers_remove(e, w1);

    /* w2 should now be the head; w1's next pointer cleared. */
    UASSERT(e->at_watchers_head == w2);
    UASSERT(w1->next_in_event   == NULL);

    uvm_destroy(&vm);
}

/* ===== Suite entry point =============================================== */

void
test_uevent_subscribe_suite(void)
{
    printf("test_uevent_subscribe\n");
    utest_run("uevent_at_watchers_append_preserves_fifo",
              uevent_at_watchers_append_preserves_fifo);
    utest_run("uevent_at_watchers_remove_unlinks",
              uevent_at_watchers_remove_unlinks);
}
