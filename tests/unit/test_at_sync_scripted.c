/* SPDX-License-Identifier: BSD-3-Clause */
/* End-to-end: scripted at sync (cond) body fires inline through real
 * bytecode dispatch.
 *
 * Companion to test_at_scripted_e2e.c (the AT-mode version), but exercises
 * the AT_SYNC path: the body runs synchronously on the scratch frame inside
 * watcher_eval_dirty (no body-strand spawn).  No test hooks installed —
 * the watcher's cond closure goes through urbi_run_closure_on_scratch and
 * the body goes through invoke_body_inline → urbi_run_closure_on_scratch.
 *
 * Body observes its run via Realm.fired counter — incremented inside the
 * body, read by the host afterwards via urbi_realm_get_global. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include <stddef.h>

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "watcher/uwatcher.h"

#define UTEST(name) static void name(void)

/* compile_and_run / run_to_no_runnable / make_int now live in
 * utest_e2e_helpers.{h,c}; see that header for documentation. */

/* ===================================================================
 * Test: scripted_at_sync_fires_on_rising_edge
 *
 * Install at sync (Realm.x > 5) Realm.fired = Realm.fired + 1 via a real
 * compiled script.  Trigger the rising edge by writing Realm.x = 10
 * through a nested function call (so the non-top-frame OP_RET safepoint
 * fires watcher_eval_dirty).  Unlike AT mode, the body runs inline via
 * invoke_body_inline (no body strand) — so Realm.fired must equal 1
 * immediately after compile_and_run returns.
 *
 * A second write (Realm.x = 20) must not re-fire (rising-edge discipline).
 * =================================================================== */
UTEST(scripted_at_sync_fires_on_rising_edge)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *gr = urbi_realm_global(&vm);
    UASSERT(gr != NULL);
    if (gr == NULL) { urbi_vm_destroy(&vm); return; }

    int rc;
    rc = urbi_realm_set_global(&vm, gr, "x",     1, utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);
    rc = urbi_realm_set_global(&vm, gr, "fired", 5, utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    /* === Phase 1: install the at sync watcher === */
    rc = utest_e2e_compile_and_run(&vm,
        "at sync (Realm.x > 5) Realm.fired = Realm.fired + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);
    if (rc != URBI_OK) {
        urbi_vm_destroy(&vm);
        return;
    }

    UASSERT(vm.active_watchers_head != NULL);

    UValue fired = {0};
    rc = urbi_realm_get_global(&vm, gr, "fired", 5, &fired);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT_EQ(0, (int)fired.v.i);

    /* === Phase 2: trigger the rising edge ===
     *
     * The non-top OP_RET safepoint fires watcher_eval_dirty.  For AT_SYNC,
     * the body runs inline on the scratch frame (no strand spawn).  So
     * Realm.fired must equal 1 by the time compile_and_run returns. */
    rc = utest_e2e_compile_and_run(&vm,
        "var trigger = function() { Realm.x = 10 }; trigger()",
        NULL);
    if (rc != URBI_OK) {
        while (vm.active_watchers_head != NULL)
            urbi_watcher_unregister_internal(&vm, vm.active_watchers_head);
        urbi_vm_destroy(&vm);
        return;
    }

    /* AT_SYNC: body fired inline; no strand to drain.  Run to quiescence
     * for symmetry with the AT-mode test (defensive — should be no-op). */
    int step_rc = utest_e2e_run_to_no_runnable(&vm);
    UASSERT(step_rc != -1);

    rc = urbi_realm_get_global(&vm, gr, "fired", 5, &fired);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT_EQ(1, (int)fired.v.i);

    /* === Phase 3: same-direction write must NOT re-fire === */
    rc = utest_e2e_compile_and_run(&vm,
        "var trigger2 = function() { Realm.x = 20 }; trigger2()",
        NULL);
    (void)rc;

    (void)utest_e2e_run_to_no_runnable(&vm);

    rc = urbi_realm_get_global(&vm, gr, "fired", 5, &fired);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT_EQ(1, (int)fired.v.i);

    /* === Cleanup === */
    while (vm.active_watchers_head != NULL)
        urbi_watcher_unregister_internal(&vm, vm.active_watchers_head);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_at_sync_scripted_suite(void)
{
    printf("test_at_sync_scripted\n");
    utest_run("scripted_at_sync_fires_on_rising_edge",
              scripted_at_sync_fires_on_rising_edge);
}
