/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: OP_AT_EVENT_INSTALL[_SYNC] dispatch helpers (T47).
 * Spec #3 §6.2.
 *
 * Uses install_at_event_runtime directly (the helper also called by the
 * OP_AT_EVENT_INSTALL / OP_AT_EVENT_SYNC_INSTALL dispatchers) so that the
 * link-chain assertions are isolated from the full bytecode pipeline.
 *
 * Cases:
 *   1. at_event_install_links_into_event_chain:
 *      install_at_event_runtime places the watcher on event->at_watchers_head
 *      with correct mode and back-pointer.
 *   2. at_event_sync_install_links_with_sync_mode:
 *      UWATCHER_AT_EVENT_SYNC mode is stored correctly.
 *   3. at_event_install_does_not_join_active_watchers:
 *      vm->active_watchers_head must remain NULL (AT_EVENT skips that list).
 *   4. at_event_install_pool_exhausted:
 *      With pool drained, install returns URBI_INSTALL_OOM_POOL. */

#include "utest.h"

#include "uevent.h"
#include "watcher/uwatcher.h"
#include "watcher/uwatcher_install.h"
#include "vm/uvm.h"
#include "ustrand.h"

#include <stddef.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* ===== Test 1: basic linkage ============================================ */

UTEST(at_event_install_links_into_event_chain)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    UWatcherInstallResult r =
        install_at_event_runtime(&vm, &s, UWATCHER_AT_EVENT, e, NULL, NULL);
    UASSERT_EQ((int)URBI_INSTALL_OK, (int)r);

    /* Watcher must be linked into the event's at_watchers_head. */
    UASSERT(e->at_watchers_head != NULL);
    UASSERT_EQ((int)UWATCHER_AT_EVENT, (int)e->at_watchers_head->mode);
    UASSERT(e->at_watchers_head->event == e);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ===== Test 2: sync mode stored correctly ============================== */

UTEST(at_event_sync_install_links_with_sync_mode)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    UWatcherInstallResult r =
        install_at_event_runtime(&vm, &s, UWATCHER_AT_EVENT_SYNC, e, NULL, NULL);
    UASSERT_EQ((int)URBI_INSTALL_OK, (int)r);

    UASSERT(e->at_watchers_head != NULL);
    UASSERT_EQ((int)UWATCHER_AT_EVENT_SYNC, (int)e->at_watchers_head->mode);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ===== Test 3: does not join active_watchers_head ====================== */

UTEST(at_event_install_does_not_join_active_watchers)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    UWatcherInstallResult r =
        install_at_event_runtime(&vm, &s, UWATCHER_AT_EVENT, e, NULL, NULL);
    UASSERT_EQ((int)URBI_INSTALL_OK, (int)r);

    /* AT_EVENT must NOT appear on active_watchers_head. */
    UASSERT(vm.active_watchers_head == NULL);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ===== Test 4: pool exhausted returns OOM ============================== */

UTEST(at_event_install_pool_exhausted)
{
    UVM vm;
    UStrand s;
    UWatcher *held[URBI_WATCHER_POOL_SIZE];
    int i;

    uvm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);

    /* Drain the pool. */
    for (i = 0; i < URBI_WATCHER_POOL_SIZE; i++)
        held[i] = uwatcher_pool_alloc(&vm);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    UWatcherInstallResult r =
        install_at_event_runtime(&vm, &s, UWATCHER_AT_EVENT, e, NULL, NULL);
    UASSERT_EQ((int)URBI_INSTALL_OOM_POOL, (int)r);

    /* Return pool slots. */
    for (i = 0; i < URBI_WATCHER_POOL_SIZE; i++) {
        if (held[i] != NULL) {
            held[i]->mode = UWATCHER_AT;  /* avoid AT_EVENT unregister path */
            urbi_watcher_unregister_internal(&vm, held[i]);
        }
    }

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ===== Suite entry point =============================================== */

void
test_at_event_dispatch_suite(void)
{
    printf("test_at_event_dispatch\n");
    utest_run("at_event_install_links_into_event_chain",
              at_event_install_links_into_event_chain);
    utest_run("at_event_sync_install_links_with_sync_mode",
              at_event_sync_install_links_with_sync_mode);
    utest_run("at_event_install_does_not_join_active_watchers",
              at_event_install_does_not_join_active_watchers);
    utest_run("at_event_install_pool_exhausted",
              at_event_install_pool_exhausted);
}
