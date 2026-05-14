/* SPDX-License-Identifier: BSD-3-Clause */
/* test_event_unregister.c — TDD tests for urbi_event_unregister (Gap B / T59).
 *
 * Four sub-tests:
 *   1. unregister_sentinel_fires: after unregister the UEvent is emitted async
 *      (sentinel NIL payload) so bound watchers run one last time.  Verified
 *      by checking that c_event_emit_async sees the event (we count at_watcher
 *      spawns indirectly via the strand count after urbi_step).
 *      NOTE: full watcher spawn requires running urbiscript `at(evt)` which is
 *      an integration-level test.  At unit level we verify the sentinel path by
 *      confirming the UEvent's at_watchers_head is NOT walked if NULL (no crash),
 *      i.e. the function completes without error when no watchers are bound.
 *   2. unregister_removes_realm_global: after unregister the realm-global slot
 *      for the event name no longer resolves (URBI_ERR_SLOT_NOT_FOUND).
 *   3. unregister_tombstones_registry_entry: after unregister, registry lookup
 *      by id returns NULL (tombstoned).
 *   4. unregister_invalid_id_returns_err: invalid / already-tombstoned ids
 *      return URBI_ERR_INVALID_ARG. */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "event/uevent_registry.h"
#include "runtime/umacros.h"   /* urbi_strlen */

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* =========================================================================
 * Sub-test 1: unregister completes without error when no watchers are bound.
 *   (Verifies sentinel dispatch path: NIL emit on the UEvent with no
 *    at_watchers_head — should be a no-op without crashing.)
 * ========================================================================= */

UTEST(unregister_sentinel_no_crash)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    urbi_event_id_t id = urbi_event_register(&vm, realm, "sentinel_evt",
                                              NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* Unregister with no watchers bound: sentinel emit must not crash. */
    int rc = urbi_event_unregister(&vm, realm, id);
    UASSERT_EQ(URBI_OK, rc);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: realm-global slot is removed after unregister.
 * ========================================================================= */

UTEST(unregister_removes_realm_global)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    urbi_event_id_t id = urbi_event_register(&vm, realm, "gone_evt",
                                              NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* Slot must be present before unregister. */
    UValue out = urbi_make_nil();
    int rc = urbi_realm_get_global(&vm, realm, "gone_evt",
                                   urbi_strlen("gone_evt"), &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)out.kind, (int)UVAL_EVENT);

    /* Unregister. */
    rc = urbi_event_unregister(&vm, realm, id);
    UASSERT_EQ(URBI_OK, rc);

    /* Slot must be gone: shape-based lookup returns SLOT_NOT_FOUND. */
    out = urbi_make_nil();
    rc = urbi_realm_get_global(&vm, realm, "gone_evt",
                               urbi_strlen("gone_evt"), &out);
    UASSERT_EQ(URBI_ERR_SLOT_NOT_FOUND, rc);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: registry entry is tombstoned after unregister.
 * ========================================================================= */

UTEST(unregister_tombstones_registry_entry)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    urbi_event_id_t id = urbi_event_register(&vm, realm, "tomb_evt",
                                              NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* Entry must be reachable before unregister. */
    UEventRegistryEntry *entry =
        uevent_registry_lookup_by_id(&vm.event_registry, id);
    UASSERT(entry != NULL);

    /* Unregister. */
    int rc = urbi_event_unregister(&vm, realm, id);
    UASSERT_EQ(URBI_OK, rc);

    /* Lookup by id must return NULL (tombstoned). */
    entry = uevent_registry_lookup_by_id(&vm.event_registry, id);
    UASSERT(entry == NULL);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 4: invalid / double-unregister returns URBI_ERR_INVALID_ARG.
 * ========================================================================= */

UTEST(unregister_invalid_id_returns_err)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* URBI_EVENT_ID_INVALID → URBI_ERR_INVALID_ARG. */
    int rc = urbi_event_unregister(&vm, realm, URBI_EVENT_ID_INVALID);
    UASSERT_EQ(URBI_ERR_INVALID_ARG, rc);

    /* Out-of-range id → URBI_ERR_INVALID_ARG. */
    rc = urbi_event_unregister(&vm, realm, (urbi_event_id_t)42U);
    UASSERT_EQ(URBI_ERR_INVALID_ARG, rc);

    /* Register then unregister, then double-unregister → URBI_ERR_INVALID_ARG. */
    urbi_event_id_t id = urbi_event_register(&vm, realm, "double_evt",
                                              NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    rc = urbi_event_unregister(&vm, realm, id);
    UASSERT_EQ(URBI_OK, rc);

    rc = urbi_event_unregister(&vm, realm, id);
    UASSERT_EQ(URBI_ERR_INVALID_ARG, rc);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_event_unregister_suite(void)
{
    utest_run("event_unregister: sentinel emit + no-crash (no watchers)",
              unregister_sentinel_no_crash);
    utest_run("event_unregister: realm-global slot removed",
              unregister_removes_realm_global);
    utest_run("event_unregister: registry entry tombstoned",
              unregister_tombstones_registry_entry);
    utest_run("event_unregister: invalid/double-unregister → INVALID_ARG",
              unregister_invalid_id_returns_err);
}
