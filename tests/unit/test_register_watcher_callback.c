/* SPDX-License-Identifier: BSD-3-Clause */
/* test_register_watcher_callback.c — TDD tests for urbi_register_watcher
 * callback invocation on dispatch (Gap J, v0.7.1).
 *
 * Four sub-tests:
 *   1. callback_fires_on_inject: register a host watcher for event "accel";
 *      inject the event; drain; verify cb fired once.
 *   2. callback_receives_correct_args: destructure fn provides 3 floats;
 *      verify host watcher receives argc==3 and correct values.
 *   3. two_watchers_same_event_both_fire: two host watchers on same event;
 *      both must fire in registration order.
 *   4. main_thread_only: urbi_in_isr returns false during watcher cb
 *      (verifies MAIN-thread execution context in URBI_DEBUG builds;
 *       always-passes in non-debug builds since urbi_in_isr is absent). */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "event/uevent_ring.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* =========================================================================
 * Shared capture structures
 * ========================================================================= */

typedef struct {
    int                   call_count;
    urbi_event_id_t       last_event_id;
    int                   last_argc;
    UValue                last_args[16];
} WatcherCapture;

static WatcherCapture g_cap1;
static WatcherCapture g_cap2;
#ifdef URBI_DEBUG
static int            g_isr_during_cb;  /* sub-test 4 */
#endif

static int
capture_cb1(struct UVM *vm, urbi_event_id_t event_id,
            const UValue *args, int argc, void *ud)
{
    int i;
    (void)vm; (void)ud;
    g_cap1.call_count++;
    g_cap1.last_event_id = event_id;
    g_cap1.last_argc = argc;
    for (i = 0; i < argc && i < 16; i++) {
        g_cap1.last_args[i] = args[i];
    }
    return URBI_OK;
}

static int
capture_cb2(struct UVM *vm, urbi_event_id_t event_id,
            const UValue *args, int argc, void *ud)
{
    int i;
    (void)vm; (void)ud;
    g_cap2.call_count++;
    g_cap2.last_event_id = event_id;
    g_cap2.last_argc = argc;
    for (i = 0; i < argc && i < 16; i++) {
        g_cap2.last_args[i] = args[i];
    }
    return URBI_OK;
}

#ifdef URBI_DEBUG
static int
isr_check_cb(struct UVM *vm, urbi_event_id_t event_id,
             const UValue *args, int argc, void *ud)
{
    struct UVM *captured_vm = (struct UVM *)ud;
    (void)event_id; (void)args; (void)argc; (void)vm;
    /* Record whether we are in ISR context during this callback. */
    g_isr_during_cb = urbi_in_isr(captured_vm) ? 1 : 0;
    return URBI_OK;
}
#endif /* URBI_DEBUG */

/* =========================================================================
 * Shared 3-float destructure fn
 * ========================================================================= */

static int
float3_destruct(struct UVM *vm,
                const urbi_event_payload_t *payload, size_t payload_len,
                UValue *out_args, int max_args, void *ud)
{
    (void)vm; (void)ud;
    if (payload == NULL || payload_len < 3 * sizeof(float) || max_args < 3) {
        return 0;
    }
    out_args[0].kind = (uint8_t)UVAL_FLOAT;
    out_args[0].v.f  = (double)payload->f32[0];
    out_args[1].kind = (uint8_t)UVAL_FLOAT;
    out_args[1].v.f  = (double)payload->f32[1];
    out_args[2].kind = (uint8_t)UVAL_FLOAT;
    out_args[2].v.f  = (double)payload->f32[2];
    return 3;
}

/* =========================================================================
 * Sub-test 1: callback fires once on inject + drain.
 * ========================================================================= */

UTEST(callback_fires_on_inject)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    g_cap1.call_count = 0;

    urbi_event_id_t id = urbi_event_register(&vm, realm, "accel1",
                                             NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    urbi_watcher_handle_t h = urbi_register_watcher(&vm, realm, id,
                                                     capture_cb1, NULL);
    UASSERT(h != URBI_WATCHER_HANDLE_INVALID);

    /* Inject: no payload (no destructure fn → argc=0). */
    int rc = urbi_inject_event(&vm, (uint32_t)id, NULL, 0U);
    UASSERT_EQ(URBI_OK, rc);

    uevent_ring_drain(&vm);

    UASSERT_EQ(1, g_cap1.call_count);
    UASSERT_EQ((int)id, (int)g_cap1.last_event_id);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: callback receives correct destructured args.
 * ========================================================================= */

UTEST(callback_receives_correct_args)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    g_cap1.call_count = 0;
    g_cap1.last_argc  = -1;

    urbi_event_id_t id = urbi_event_register(&vm, realm, "accel2",
                                             float3_destruct, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    urbi_watcher_handle_t h = urbi_register_watcher(&vm, realm, id,
                                                     capture_cb1, NULL);
    UASSERT(h != URBI_WATCHER_HANDLE_INVALID);

    /* Inject 3 floats: 1.0, 2.0, 3.0. */
    urbi_event_payload_t pload = {0};
    pload.f32[0] = 1.0f;
    pload.f32[1] = 2.0f;
    pload.f32[2] = 3.0f;
    int rc = urbi_inject_event(&vm, (uint32_t)id,
                               &pload, 3 * sizeof(float));
    UASSERT_EQ(URBI_OK, rc);

    uevent_ring_drain(&vm);

    /* Host watcher must receive all 3 args. */
    UASSERT_EQ(1, g_cap1.call_count);
    UASSERT_EQ(3, g_cap1.last_argc);
    UASSERT_EQ((int)UVAL_FLOAT, (int)g_cap1.last_args[0].kind);
    UASSERT_EQ((int)UVAL_FLOAT, (int)g_cap1.last_args[1].kind);
    UASSERT_EQ((int)UVAL_FLOAT, (int)g_cap1.last_args[2].kind);
    /* Exact float comparison acceptable for round-trip f32 → double. */
    UASSERT(g_cap1.last_args[0].v.f == 1.0);
    UASSERT(g_cap1.last_args[1].v.f == 2.0);
    UASSERT(g_cap1.last_args[2].v.f == 3.0);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: two watchers on the same event, both fire in order.
 * ========================================================================= */

UTEST(two_watchers_same_event_both_fire)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    g_cap1.call_count = 0;
    g_cap2.call_count = 0;

    urbi_event_id_t id = urbi_event_register(&vm, realm, "accel3",
                                             NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    urbi_watcher_handle_t h1 = urbi_register_watcher(&vm, realm, id,
                                                      capture_cb1, NULL);
    urbi_watcher_handle_t h2 = urbi_register_watcher(&vm, realm, id,
                                                      capture_cb2, NULL);
    UASSERT(h1 != URBI_WATCHER_HANDLE_INVALID);
    UASSERT(h2 != URBI_WATCHER_HANDLE_INVALID);
    UASSERT(h1 != h2);  /* distinct handles */

    int rc = urbi_inject_event(&vm, (uint32_t)id, NULL, 0U);
    UASSERT_EQ(URBI_OK, rc);

    uevent_ring_drain(&vm);

    UASSERT_EQ(1, g_cap1.call_count);
    UASSERT_EQ(1, g_cap2.call_count);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 4: urbi_in_isr returns false during watcher cb (MAIN context).
 *
 * In URBI_DEBUG builds: explicitly verify urbi_in_isr(vm) == false during cb.
 * In non-debug builds: this sub-test is a no-op (urbi_in_isr absent).
 * ========================================================================= */

UTEST(callback_runs_on_main_thread)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

#ifdef URBI_DEBUG
    g_isr_during_cb = -1;  /* sentinel: cb has not run yet */

    /* Install a trivial ISR check function (always false → MAIN context). */
    urbi_set_isr_check_fn(&vm, NULL, NULL);  /* NULL = never ISR */

    urbi_event_id_t id = urbi_event_register(&vm, realm, "accel4", NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* Pass &vm as ud so isr_check_cb can call urbi_in_isr(vm). */
    urbi_watcher_handle_t h = urbi_register_watcher(&vm, realm, id,
                                                     isr_check_cb, &vm);
    UASSERT(h != URBI_WATCHER_HANDLE_INVALID);

    int rc = urbi_inject_event(&vm, (uint32_t)id, NULL, 0U);
    UASSERT_EQ(URBI_OK, rc);

    uevent_ring_drain(&vm);

    /* cb must have run (sentinel replaced) and must have seen MAIN context. */
    UASSERT_EQ(0, g_isr_during_cb);   /* 0 = not in ISR */
#else
    /* Non-debug: always pass. */
    UASSERT(1);
#endif /* URBI_DEBUG */

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_register_watcher_callback_suite(void)
{
    utest_run("register_watcher_callback: cb fires once on inject+drain",
              callback_fires_on_inject);
    utest_run("register_watcher_callback: cb receives correct destructured args",
              callback_receives_correct_args);
    utest_run("register_watcher_callback: two watchers on same event both fire",
              two_watchers_same_event_both_fire);
    utest_run("register_watcher_callback: cb runs on MAIN thread (not ISR)",
              callback_runs_on_main_thread);
}
