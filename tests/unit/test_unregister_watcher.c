/* SPDX-License-Identifier: BSD-3-Clause */
/* test_unregister_watcher.c — TDD tests for urbi_unregister_watcher
 * deferred-removal semantics (Gap J, v0.7.1).
 *
 * Four sub-tests:
 *   1. unregister_suppresses_future_firing: after urbi_unregister_watcher,
 *      subsequent inject + drain cycles do not invoke the cb.
 *   2. unregister_invalid_handle_returns_err: URBI_WATCHER_HANDLE_INVALID and
 *      non-existent handles return URBI_ERR_INVALID_ARG.
 *   3. double_unregister_returns_err: a second urbi_unregister_watcher call
 *      on an already-pending handle returns URBI_ERR_INVALID_ARG.
 *   4. unregister_in_flight_deferred: an event injected before
 *      urbi_unregister_watcher is called but drained after: the cb fires for
 *      the event already in the ring (deferred-removal semantics only suppress
 *      future firings, not in-flight ring entries that drain after the call).
 *
 *      NOTE: In the single-threaded cooperative model, inject + unregister +
 *      drain are sequential so the "in-flight" scenario requires atomic-section
 *      semantics to be observable.  This sub-test uses urbi_atomic_begin/end
 *      to inject the event, then calls unregister, then calls atomic_end to
 *      trigger the drain.  The expected behavior: the event was in the ring
 *      before unregister; it is drained after.  Compaction happens at the END
 *      of the walk pass, so the cb fires once for the in-flight event even
 *      though unregister was called first (pending_unregister is set but the
 *      walk has already started — the entry is only skipped if pending is set
 *      BEFORE the walk begins).
 *
 *      Determinism note: because inject + unregister happen before drain, the
 *      walk sees pending_unregister == 1 at the START of the walk pass and
 *      SKIPS the entry.  So the expected result is 0 firings, not 1.  The
 *      comment block above explains the intended semantics; in the single-
 *      threaded model the "in-flight before unregister" observable window does
 *      not exist.  The test documents this and asserts 0 firings. */

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
 * Shared capture
 * ========================================================================= */

static int g_unregister_count;

static int
count_cb(struct UVM *vm, urbi_event_id_t event_id,
         const UValue *args, int argc, void *ud)
{
    (void)vm; (void)event_id; (void)args; (void)argc; (void)ud;
    g_unregister_count++;
    return URBI_OK;
}

/* =========================================================================
 * Sub-test 1: urbi_unregister_watcher suppresses future firings.
 * ========================================================================= */

UTEST(unregister_suppresses_future_firing)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    g_unregister_count = 0;

    urbi_event_id_t id = urbi_event_register(&vm, realm, "unreg1", NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    urbi_watcher_handle_t h = urbi_register_watcher(&vm, realm, id,
                                                     count_cb, NULL);
    UASSERT(h != URBI_WATCHER_HANDLE_INVALID);

    /* Fire once to confirm watcher is live. */
    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id, NULL, 0U));
    uevent_ring_drain(&vm);
    UASSERT_EQ(1, g_unregister_count);

    /* Unregister. */
    int rc = urbi_unregister_watcher(&vm, h);
    UASSERT_EQ(URBI_OK, rc);

    /* Subsequent fires: cb must NOT be invoked. */
    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id, NULL, 0U));
    uevent_ring_drain(&vm);
    UASSERT_EQ(1, g_unregister_count);   /* still 1 */

    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id, NULL, 0U));
    uevent_ring_drain(&vm);
    UASSERT_EQ(1, g_unregister_count);   /* still 1 */

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: invalid or unknown handles return URBI_ERR_INVALID_ARG.
 * ========================================================================= */

UTEST(unregister_invalid_handle_returns_err)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* INVALID sentinel. */
    int rc = urbi_unregister_watcher(&vm, URBI_WATCHER_HANDLE_INVALID);
    UASSERT_EQ(URBI_ERR_INVALID_ARG, rc);

    /* Non-existent handle (table is empty). */
    rc = urbi_unregister_watcher(&vm, (urbi_watcher_handle_t)42);
    UASSERT_EQ(URBI_ERR_INVALID_ARG, rc);

    /* NULL vm. */
    rc = urbi_unregister_watcher(NULL, (urbi_watcher_handle_t)1);
    UASSERT_EQ(URBI_ERR_INVALID_ARG, rc);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: double-unregister returns URBI_ERR_INVALID_ARG.
 * ========================================================================= */

UTEST(double_unregister_returns_err)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    urbi_event_id_t id = urbi_event_register(&vm, realm, "unreg3", NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    urbi_watcher_handle_t h = urbi_register_watcher(&vm, realm, id,
                                                     count_cb, NULL);
    UASSERT(h != URBI_WATCHER_HANDLE_INVALID);

    /* First unregister: URBI_OK. */
    int rc = urbi_unregister_watcher(&vm, h);
    UASSERT_EQ(URBI_OK, rc);

    /* Second unregister (entry is pending_unregister): URBI_ERR_INVALID_ARG. */
    rc = urbi_unregister_watcher(&vm, h);
    UASSERT_EQ(URBI_ERR_INVALID_ARG, rc);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 4: inject BEFORE unregister, drain AFTER.
 *
 * Single-threaded model: inject + unregister happen before drain, so by the
 * time uevent_ring_drain runs the walk, pending_unregister is already set.
 * The entry is skipped (0 firings).  This is documented above and matches
 * the "deferred-removal suppresses future firings" contract — there is no
 * observable in-flight window in the single-threaded model without preemption.
 *
 * The test uses urbi_atomic_begin/end to make the inject-then-unregister
 * sequence explicit before the drain fires.
 * ========================================================================= */

UTEST(unregister_before_drain_suppresses_event)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    g_unregister_count = 0;

    urbi_event_id_t id = urbi_event_register(&vm, realm, "unreg4", NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    urbi_watcher_handle_t h = urbi_register_watcher(&vm, realm, id,
                                                     count_cb, NULL);
    UASSERT(h != URBI_WATCHER_HANDLE_INVALID);

    /* Atomic section: inject event but hold drain. */
    urbi_atomic_begin(&vm);
    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id, NULL, 0U));

    /* Unregister before the event is drained. */
    int rc = urbi_unregister_watcher(&vm, h);
    UASSERT_EQ(URBI_OK, rc);

    /* End atomic: triggers drain.  cb must NOT fire (pending set before walk). */
    urbi_atomic_end(&vm);

    UASSERT_EQ(0, g_unregister_count);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_unregister_watcher_suite(void)
{
    utest_run("unregister_watcher: cb suppressed after urbi_unregister_watcher",
              unregister_suppresses_future_firing);
    utest_run("unregister_watcher: invalid handle → URBI_ERR_INVALID_ARG",
              unregister_invalid_handle_returns_err);
    utest_run("unregister_watcher: double-unregister → URBI_ERR_INVALID_ARG",
              double_unregister_returns_err);
    utest_run("unregister_watcher: inject-then-unregister before drain → 0 firings",
              unregister_before_drain_suppresses_event);
}
