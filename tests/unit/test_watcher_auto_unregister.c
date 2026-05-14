/* SPDX-License-Identifier: BSD-3-Clause */
/* test_watcher_auto_unregister.c — TDD tests for URBI_ERR_WATCHER_UNREGISTER
 * auto-unregistration sentinel (Gap J, v0.7.1).
 *
 * Two sub-tests:
 *   1. auto_unregister_after_one_fire: a watcher cb that returns
 *      URBI_ERR_WATCHER_UNREGISTER fires exactly once; a subsequent inject
 *      + drain does NOT fire it again.
 *   2. normal_watcher_persists: a watcher cb that returns URBI_OK fires on
 *      every inject (not removed after one fire). */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "event/uevent_ring.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* =========================================================================
 * Shared state
 * ========================================================================= */

static int g_once_count;   /* sub-test 1: one-shot watcher */
static int g_persist_count; /* sub-test 2: persistent watcher */

/* Returns URBI_ERR_WATCHER_UNREGISTER on first call → auto-unregisters. */
static int
one_shot_cb(struct UVM *vm, urbi_event_id_t event_id,
            const UValue *args, int argc, void *ud)
{
    (void)vm; (void)event_id; (void)args; (void)argc; (void)ud;
    g_once_count++;
    return URBI_ERR_WATCHER_UNREGISTER;
}

/* Always returns URBI_OK → stays registered. */
static int
persist_cb(struct UVM *vm, urbi_event_id_t event_id,
           const UValue *args, int argc, void *ud)
{
    (void)vm; (void)event_id; (void)args; (void)argc; (void)ud;
    g_persist_count++;
    return URBI_OK;
}

/* =========================================================================
 * Sub-test 1: URBI_ERR_WATCHER_UNREGISTER → fires once, then removed.
 * ========================================================================= */

UTEST(auto_unregister_after_one_fire)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    g_once_count = 0;

    urbi_event_id_t id = urbi_event_register(&vm, realm, "once_evt", NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    urbi_watcher_handle_t h = urbi_register_watcher(&vm, realm, id,
                                                     one_shot_cb, NULL);
    UASSERT(h != URBI_WATCHER_HANDLE_INVALID);

    /* First inject + drain: cb fires, returns UNREGISTER sentinel. */
    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id, NULL, 0U));
    uevent_ring_drain(&vm);
    UASSERT_EQ(1, g_once_count);

    /* Second inject + drain: cb must NOT fire (auto-unregistered). */
    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id, NULL, 0U));
    uevent_ring_drain(&vm);
    UASSERT_EQ(1, g_once_count);   /* still 1, not 2 */

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: URBI_OK return → watcher persists across multiple events.
 * ========================================================================= */

UTEST(normal_watcher_persists)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    g_persist_count = 0;

    urbi_event_id_t id = urbi_event_register(&vm, realm, "persist_evt",
                                             NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    urbi_watcher_handle_t h = urbi_register_watcher(&vm, realm, id,
                                                     persist_cb, NULL);
    UASSERT(h != URBI_WATCHER_HANDLE_INVALID);

    /* Three inject + drain cycles: cb fires every time. */
    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id, NULL, 0U));
    uevent_ring_drain(&vm);
    UASSERT_EQ(1, g_persist_count);

    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id, NULL, 0U));
    uevent_ring_drain(&vm);
    UASSERT_EQ(2, g_persist_count);

    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id, NULL, 0U));
    uevent_ring_drain(&vm);
    UASSERT_EQ(3, g_persist_count);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_watcher_auto_unregister_suite(void)
{
    utest_run("watcher_auto_unregister: UNREGISTER sentinel fires once then removed",
              auto_unregister_after_one_fire);
    utest_run("watcher_auto_unregister: URBI_OK return keeps watcher registered",
              normal_watcher_persists);
}
