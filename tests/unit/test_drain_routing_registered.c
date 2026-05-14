/* SPDX-License-Identifier: BSD-3-Clause */
/* test_drain_routing_registered.c — TDD tests for registered-event drain
 * auto-routing path (Gap B / T61).
 *
 * Four sub-tests:
 *   1. drain_registered_calls_destruct_fn: destructure fn is called at drain
 *      time with the correct raw payload bytes.
 *   2. drain_registered_fires_watcher_body: after drain + urbi_step, a
 *      script-side `at(evt)` watcher body runs once.
 *   3. drain_registered_no_legacy_handler: the legacy urbi_register_event_drain
 *      handler does NOT fire for registered event ids.
 *   4. drain_registered_bad_destruct_drops_event: when destruct_fn returns < 0,
 *      the event is dropped and the watcher body does NOT run.
 *
 * Note on multi-arg: today c_event_emit_async accepts a single UValue payload
 * (args[0] or NIL).  Full args[0..argc-1] threading into watcher body
 * registers R[0..n] is deferred to Sub-Bundle 3 (T64+).  Sub-test 1 verifies
 * the destructure fn args via the destructure callback itself. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "event/uevent_ring.h"
#include "event/uevent_registry.h"
#include "runtime/umacros.h"   /* urbi_strlen */

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* =========================================================================
 * Shared destructure helper: reads payload.u32[0] and writes it as a UVAL_INT.
 * ========================================================================= */

typedef struct {
    int         call_count;
    uint32_t    last_u32_0;
    uint32_t    last_u32_1;
    uint32_t    last_u32_2;
    size_t      last_payload_len;
} DestructCapture;

static DestructCapture g_destruct;

static int
capture_destruct_fn(struct UVM *vm,
                    const urbi_event_payload_t *payload, size_t payload_len,
                    UValue *out_args, int max_args, void *ud)
{
    (void)vm;
    (void)ud;

    g_destruct.call_count++;
    g_destruct.last_payload_len = payload_len;

    if (payload != NULL && payload_len >= sizeof(uint32_t)) {
        g_destruct.last_u32_0 = payload->u32[0];
    }
    if (payload != NULL && payload_len >= 2 * sizeof(uint32_t)) {
        g_destruct.last_u32_1 = payload->u32[1];
    }
    if (payload != NULL && payload_len >= 3 * sizeof(uint32_t)) {
        g_destruct.last_u32_2 = payload->u32[2];
    }

    /* Write args[0] = payload.u32[0] as an integer. */
    if (max_args >= 1 && payload != NULL &&
            payload_len >= sizeof(uint32_t)) {
        out_args[0].kind = (uint8_t)UVAL_INT;
        out_args[0].v.i  = (int64_t)payload->u32[0];
        return 1;
    }
    return 0;
}

/* Destructure fn that always returns -1 (simulates error). */
static int
error_destruct_fn(struct UVM *vm,
                  const urbi_event_payload_t *payload, size_t payload_len,
                  UValue *out_args, int max_args, void *ud)
{
    (void)vm; (void)payload; (void)payload_len;
    (void)out_args; (void)max_args; (void)ud;
    return -1;
}

/* Legacy drain capture for sub-test 3. */
static uint32_t g_legacy_call_count;
static void
legacy_drain(struct UVM *vm, uint32_t event_id, UValue payload)
{
    (void)vm; (void)event_id; (void)payload;
    g_legacy_call_count++;
}

/* =========================================================================
 * Sub-test 1: destruct_fn is called at drain with correct payload bytes.
 * ========================================================================= */

UTEST(drain_registered_calls_destruct_fn)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Reset capture. */
    g_destruct.call_count     = 0;
    g_destruct.last_u32_0     = 0;
    g_destruct.last_payload_len = 0;

    urbi_event_id_t id = urbi_event_register(&vm, realm, "destruct_test",
                                              capture_destruct_fn, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* Inject event with u32[0]=42 as payload. */
    urbi_event_payload_t pload = {0};
    pload.u32[0] = 42U;
    int rc = urbi_inject_event(&vm, (uint32_t)id,
                                &pload, sizeof(pload));
    UASSERT_EQ(URBI_OK, rc);

    /* Drain: destruct_fn must be called. */
    uevent_ring_drain(&vm);

    UASSERT_EQ(1, g_destruct.call_count);
    UASSERT_EQ(42U, g_destruct.last_u32_0);
    UASSERT_EQ(sizeof(pload), g_destruct.last_payload_len);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: watcher body runs after drain + urbi_step.
 * ========================================================================= */

UTEST(drain_registered_fires_watcher_body)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Pre-install Realm.fired = 0. */
    int rc = urbi_realm_set_global(&vm, realm, "fired", 5,
                                   utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    /* Register the event. */
    g_destruct.call_count = 0;
    urbi_event_id_t id = urbi_event_register(&vm, realm, "body_test",
                                              capture_destruct_fn, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* Install `at (body_test?) { Realm.fired = Realm.fired + 1 }`.
     * The `?` suffix distinguishes AT_EVENT from a condition watcher.
     * body_test is a realm global of kind UVAL_EVENT so the install
     * hooks the UEvent's at_watchers_head. */
    rc = utest_e2e_compile_and_run(&vm,
        "at (body_test?) Realm.fired = Realm.fired + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Inject event. */
    urbi_event_payload_t pload = {0};
    pload.u32[0] = 1U;
    rc = urbi_inject_event(&vm, (uint32_t)id, &pload, sizeof(pload));
    UASSERT_EQ(URBI_OK, rc);

    /* Drain: destruct_fn called + c_event_emit_async fires. */
    uevent_ring_drain(&vm);
    UASSERT_EQ(1, g_destruct.call_count);

    /* Step to quiescence: body strand should run and complete. */
    int quiescent = utest_e2e_run_to_no_runnable(&vm);
    UASSERT(quiescent >= 0);   /* -1 = fatal */

    /* Realm.fired must be 1. */
    UValue fired = utest_e2e_make_nil();
    rc = urbi_realm_get_global(&vm, realm, "fired", 5, &fired);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT_EQ(1LL, fired.v.i);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: legacy drain handler does NOT fire for registered event ids.
 * ========================================================================= */

UTEST(drain_registered_no_legacy_handler)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    g_legacy_call_count = 0U;
    urbi_register_event_drain(&vm, legacy_drain);

    urbi_event_id_t id = urbi_event_register(&vm, realm, "no_legacy_test",
                                              NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* Inject and drain. */
    int rc = urbi_inject_event(&vm, (uint32_t)id, NULL, 0U);
    UASSERT_EQ(URBI_OK, rc);

    uevent_ring_drain(&vm);

    /* Legacy handler must NOT have fired for the registered event. */
    UASSERT_EQ(0U, g_legacy_call_count);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 4: bad destruct_fn (-1) drops event; watcher body does not run.
 * ========================================================================= */

UTEST(drain_registered_bad_destruct_drops_event)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Pre-install Realm.fired2 = 0. */
    int rc = urbi_realm_set_global(&vm, realm, "fired2", 6,
                                   utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    urbi_event_id_t id = urbi_event_register(&vm, realm, "bad_destruct_test",
                                              error_destruct_fn, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* Install watcher so any emit would bump fired2. */
    rc = utest_e2e_compile_and_run(&vm,
        "at (bad_destruct_test?) Realm.fired2 = Realm.fired2 + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Inject and drain: destruct_fn returns -1 → event dropped. */
    rc = urbi_inject_event(&vm, (uint32_t)id, NULL, 0U);
    UASSERT_EQ(URBI_OK, rc);
    uevent_ring_drain(&vm);

    /* No emit should have fired — watcher body should not run. */
    (void)utest_e2e_run_to_no_runnable(&vm);

    UValue fired2 = utest_e2e_make_nil();
    rc = urbi_realm_get_global(&vm, realm, "fired2", 6, &fired2);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)fired2.kind);
    UASSERT_EQ(0LL, fired2.v.i);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_drain_routing_registered_suite(void)
{
    utest_run("drain_routing_registered: destruct_fn called with payload",
              drain_registered_calls_destruct_fn);
    utest_run("drain_routing_registered: watcher body fires after drain+step",
              drain_registered_fires_watcher_body);
    utest_run("drain_routing_registered: legacy handler skipped",
              drain_registered_no_legacy_handler);
    utest_run("drain_routing_registered: bad destruct drops event",
              drain_registered_bad_destruct_drops_event);
}
