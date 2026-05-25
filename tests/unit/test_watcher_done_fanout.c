/* SPDX-License-Identifier: BSD-3-Clause */
/* test_watcher_done_fanout.c — TDD tests for urbi_watcher_body_done_fn
 * fanout for both script-side and host-side watcher bodies (Gap J, T70).
 *
 * Four sub-tests:
 *   1. done_fn_fires_for_host_watcher: install a done_fn; register a host
 *      watcher; inject + drain; verify done_fn fires with the non-zero handle
 *      returned by urbi_register_watcher.
 *   2. done_fn_handle_zero_for_script: install a done_fn; run `at(evt?) ...`
 *      script; inject + drain + step; verify done_fn fires with handle == 0
 *      (URBI_WATCHER_HANDLE_INVALID = script-side body completion).
 *   3. done_fn_fires_for_auto_unregister: a host watcher that returns
 *      URBI_ERR_WATCHER_UNREGISTER still triggers done_fn with status
 *      == URBI_ERR_WATCHER_UNREGISTER before being removed.
 *   4. done_fn_fanout_two_watchers_same_event: script-side + host-side watcher
 *      both on the same event; done_fn fires twice (once per watcher body),
 *      with handle == 0 for script and handle != 0 for host. */

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
 * Shared done_fn capture
 * ========================================================================= */

#define DONE_MAX 8
typedef struct {
    int                   call_count;
    urbi_watcher_handle_t handles[DONE_MAX];
    int                   statuses[DONE_MAX];
} DoneCapture;

static DoneCapture g_done;

static void
done_capture_fn(struct UVM *vm, void *ud, urbi_watcher_handle_t handle, int status)
{
    (void)vm; (void)ud;
    if (g_done.call_count < DONE_MAX) {
        g_done.handles[g_done.call_count]  = handle;
        g_done.statuses[g_done.call_count] = status;
    }
    g_done.call_count++;
}

/* Simple no-op host watcher cb. */
static int
noop_cb(struct UVM *vm, urbi_event_id_t event_id,
        const UValue *args, int argc, void *ud)
{
    (void)vm; (void)event_id; (void)args; (void)argc; (void)ud;
    return URBI_OK;
}

/* Host watcher cb that auto-unregisters. */
static int
auto_unreg_cb(struct UVM *vm, urbi_event_id_t event_id,
              const UValue *args, int argc, void *ud)
{
    (void)vm; (void)event_id; (void)args; (void)argc; (void)ud;
    return URBI_ERR_WATCHER_UNREGISTER;
}

/* =========================================================================
 * Sub-test 1: done_fn fires for host watcher with non-zero handle.
 * ========================================================================= */

UTEST(done_fn_fires_for_host_watcher)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    g_done.call_count = 0;
    urbi_set_watcher_body_done_fn(&vm, done_capture_fn, NULL);

    urbi_event_id_t id = urbi_event_register(&vm, realm, "fan1", NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    urbi_watcher_handle_t h = urbi_register_watcher(&vm, realm, id,
                                                     noop_cb, NULL);
    UASSERT(h != URBI_WATCHER_HANDLE_INVALID);

    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id, NULL, 0U));
    uevent_ring_drain(&vm);

    /* done_fn must have fired once with the host handle (non-zero). */
    UASSERT_EQ(1, g_done.call_count);
    UASSERT(g_done.handles[0] != URBI_WATCHER_HANDLE_INVALID);
    UASSERT_EQ((int)h, (int)g_done.handles[0]);
    UASSERT_EQ(URBI_OK, g_done.statuses[0]);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: done_fn fires with handle == 0 for script-side watcher body.
 * ========================================================================= */

UTEST(done_fn_handle_zero_for_script)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    g_done.call_count = 0;
    urbi_set_watcher_body_done_fn(&vm, done_capture_fn, NULL);

    /* Pre-install Realm.fan2_fired = 0. */
    int rc = urbi_realm_set_global(&vm, realm, "fan2_fired", 10,
                                   utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    urbi_event_id_t id = urbi_event_register(&vm, realm, "fan2", NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* Install script-side watcher body. */
    rc = utest_e2e_compile_and_run(&vm,
        "at (fan2?) Realm.fan2_fired = Realm.fan2_fired + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Inject + drain + step to quiescence. */
    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id, NULL, 0U));
    uevent_ring_drain(&vm);
    int q = utest_e2e_run_to_no_runnable(&vm);
    UASSERT(q >= 0);

    /* Script watcher body completed → done_fn fires with handle == 0. */
    /* Note: host_watcher count is 0 (no urbi_register_watcher called) so
     * only the script-side done fires. */
    int script_done_count = 0;
    int i;
    for (i = 0; i < g_done.call_count; i++) {
        if (g_done.handles[i] == URBI_WATCHER_HANDLE_INVALID) {
            script_done_count++;
        }
    }
    UASSERT(script_done_count >= 1);

    /* fan2_fired must be 1 (body ran). */
    UValue out = utest_e2e_make_nil();
    rc = urbi_realm_get_global(&vm, realm, "fan2_fired", 10, &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(1LL, out.v.i);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: done_fn fires for auto-unregister with UNREGISTER status.
 * ========================================================================= */

UTEST(done_fn_fires_for_auto_unregister)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    g_done.call_count = 0;
    urbi_set_watcher_body_done_fn(&vm, done_capture_fn, NULL);

    urbi_event_id_t id = urbi_event_register(&vm, realm, "fan3", NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    urbi_watcher_handle_t h = urbi_register_watcher(&vm, realm, id,
                                                     auto_unreg_cb, NULL);
    UASSERT(h != URBI_WATCHER_HANDLE_INVALID);

    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id, NULL, 0U));
    uevent_ring_drain(&vm);

    /* done_fn fires with handle = h and status = URBI_ERR_WATCHER_UNREGISTER. */
    UASSERT_EQ(1, g_done.call_count);
    UASSERT_EQ((int)h, (int)g_done.handles[0]);
    UASSERT_EQ(URBI_ERR_WATCHER_UNREGISTER, g_done.statuses[0]);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 4: script + host watcher on same event → done_fn fires twice.
 * ========================================================================= */

UTEST(done_fn_fanout_two_watchers_same_event)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    g_done.call_count = 0;
    urbi_set_watcher_body_done_fn(&vm, done_capture_fn, NULL);

    /* Pre-install counter. */
    int rc = urbi_realm_set_global(&vm, realm, "fan4_fired", 10,
                                   utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    urbi_event_id_t id = urbi_event_register(&vm, realm, "fan4", NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* Script-side watcher. */
    rc = utest_e2e_compile_and_run(&vm,
        "at (fan4?) Realm.fan4_fired = Realm.fan4_fired + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Host-side watcher. */
    urbi_watcher_handle_t h = urbi_register_watcher(&vm, realm, id,
                                                     noop_cb, NULL);
    UASSERT(h != URBI_WATCHER_HANDLE_INVALID);

    /* Inject + drain + step. */
    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id, NULL, 0U));
    uevent_ring_drain(&vm);
    int q = utest_e2e_run_to_no_runnable(&vm);
    UASSERT(q >= 0);

    /* done_fn must have fired at least twice:
     *   - once for the host watcher (handle == h, during drain)
     *   - once for the script watcher body (handle == 0, after strand completes) */
    UASSERT(g_done.call_count >= 2);

    /* Find at least one script-done (handle == 0) and one host-done (handle == h). */
    int found_script = 0;
    int found_host   = 0;
    int i;
    for (i = 0; i < g_done.call_count; i++) {
        if (g_done.handles[i] == URBI_WATCHER_HANDLE_INVALID) found_script = 1;
        if (g_done.handles[i] == h)                            found_host   = 1;
    }
    UASSERT(found_script);
    UASSERT(found_host);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_watcher_done_fanout_suite(void)
{
    utest_run("watcher_done_fanout: done_fn fires with non-zero handle for host watcher",
              done_fn_fires_for_host_watcher);
    utest_run("watcher_done_fanout: done_fn fires with handle==0 for script watcher",
              done_fn_handle_zero_for_script);
    utest_run("watcher_done_fanout: done_fn fires with UNREGISTER status on auto-unreg",
              done_fn_fires_for_auto_unregister);
    utest_run("watcher_done_fanout: both script and host watchers fire done_fn",
              done_fn_fanout_two_watchers_same_event);
}
