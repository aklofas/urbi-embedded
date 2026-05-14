/* SPDX-License-Identifier: BSD-3-Clause */
/* test_drain_routing_unregistered.c — TDD tests for unregistered-event
 * fallthrough to Wave-1 legacy drain handler (Gap B / T61 backward compat).
 *
 * Four sub-tests:
 *   1. drain_unregistered_out_of_range: event_id 0x12345 (out of urbi_event_id_t
 *      range) falls through to legacy drain handler.
 *   2. drain_unregistered_in_range_no_registry: event_id 100 is in valid range
 *      but no event is registered at that slot — falls through to legacy handler.
 *   3. drain_registered_takes_priority: after urbi_event_register returns id N,
 *      inject id=N; legacy handler does NOT fire (registry route consumed it).
 *   4. drain_tombstoned_falls_through: after urbi_event_unregister, the same id
 *      falls through to the legacy handler (tombstoned = unregistered). */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "event/uevent_ring.h"
#include "event/uevent_registry.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)
#define CAPTURE_MAX 8

/* =========================================================================
 * Shared legacy drain capture.
 * ========================================================================= */

typedef struct {
    uint32_t ids[CAPTURE_MAX];
    uint32_t count;
} LegacyCapture;

static LegacyCapture g_legacy;

static void
legacy_capture(struct UVM *vm, uint32_t event_id, UValue payload)
{
    (void)vm; (void)payload;
    if (g_legacy.count < (uint32_t)CAPTURE_MAX) {
        g_legacy.ids[g_legacy.count] = event_id;
        g_legacy.count++;
    }
}

/* =========================================================================
 * Sub-test 1: out-of-range id (0x12345) falls through to legacy handler.
 * ========================================================================= */

UTEST(drain_unregistered_out_of_range)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    g_legacy.count = 0U;
    urbi_register_event_drain(&vm, legacy_capture);

    /* 0x12345 > 0xFFFE: out of urbi_event_id_t range. */
    int rc = urbi_inject_event(&vm, 0x12345U, NULL, 0U);
    UASSERT_EQ(URBI_OK, rc);

    uevent_ring_drain(&vm);

    UASSERT_EQ(1U, g_legacy.count);
    UASSERT_EQ(0x12345U, g_legacy.ids[0]);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: in-range id 100 with no registry entry falls through.
 * ========================================================================= */

UTEST(drain_unregistered_in_range_no_registry)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    g_legacy.count = 0U;
    urbi_register_event_drain(&vm, legacy_capture);

    /* id 100 is in valid range (0..0xFFFE) but no event registered at slot 100. */
    int rc = urbi_inject_event(&vm, 100U, NULL, 0U);
    UASSERT_EQ(URBI_OK, rc);

    uevent_ring_drain(&vm);

    UASSERT_EQ(1U, g_legacy.count);
    UASSERT_EQ(100U, g_legacy.ids[0]);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: registered id does NOT reach legacy handler.
 * ========================================================================= */

UTEST(drain_registered_takes_priority)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    g_legacy.count = 0U;
    urbi_register_event_drain(&vm, legacy_capture);

    urbi_event_id_t id = urbi_event_register(&vm, realm, "priority_evt",
                                              NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    int rc = urbi_inject_event(&vm, (uint32_t)id, NULL, 0U);
    UASSERT_EQ(URBI_OK, rc);

    uevent_ring_drain(&vm);

    /* Registry path consumed the event; legacy handler must NOT have fired. */
    UASSERT_EQ(0U, g_legacy.count);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 4: tombstoned id (after unregister) falls through to legacy handler.
 * ========================================================================= */

UTEST(drain_tombstoned_falls_through)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    g_legacy.count = 0U;
    urbi_register_event_drain(&vm, legacy_capture);

    urbi_event_id_t id = urbi_event_register(&vm, realm, "tomb_fallthrough",
                                              NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* Unregister: entry tombstoned → drain will fall through to legacy. */
    int rc = urbi_event_unregister(&vm, realm, id);
    UASSERT_EQ(URBI_OK, rc);

    /* Inject and drain: should fall through to legacy handler. */
    rc = urbi_inject_event(&vm, (uint32_t)id, NULL, 0U);
    UASSERT_EQ(URBI_OK, rc);

    uevent_ring_drain(&vm);

    UASSERT_EQ(1U, g_legacy.count);
    UASSERT_EQ((uint32_t)id, g_legacy.ids[0]);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_drain_routing_unregistered_suite(void)
{
    utest_run("drain_routing_unregistered: out-of-range id → legacy handler",
              drain_unregistered_out_of_range);
    utest_run("drain_routing_unregistered: in-range but no registry → legacy",
              drain_unregistered_in_range_no_registry);
    utest_run("drain_routing_unregistered: registered id skips legacy handler",
              drain_registered_takes_priority);
    utest_run("drain_routing_unregistered: tombstoned id falls through",
              drain_tombstoned_falls_through);
}
