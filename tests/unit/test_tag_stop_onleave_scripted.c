/* SPDX-License-Identifier: BSD-3-Clause */
/* End-to-end: tag-stop drain runs the watcher's onleave handler through
 * real bytecode dispatch.
 *
 * Companion to test_at_sync_scripted.c (AT_SYNC) and test_at_scripted_e2e.c
 * (AT-mode rising edge) — exercises the third onleave invocation path:
 * the pending_onleave_queue drained at a safepoint after urbi_tag_stop.
 *
 * The test installs a top-level `at (cond) body onleave handler` watcher.
 * Because the install site has no enclosing tag scope, urbi_watcher_resolve_owning_tag
 * falls back to s->realm->tag, so the watcher's owning_tag IS the global
 * realm tag.  Calling urbi_tag_stop(vm, gr->tag, nil) cascades the watcher
 * onto vm->pending_onleave_head.  Driving a no-op nested function call
 * hits the dispatcher safepoint, which calls urbi_watcher_drain_pending_onleave_queue,
 * which calls run_watcher_onleave — the function this task wires.
 *
 * Body fires Realm.fired += 1; onleave fires Realm.left += 100.  After
 * trigger + tag_stop + drain, fired must be 1 and left must be 100.
 *
 * No test hooks installed — the watcher's cond, body, and onleave all
 * dispatch through urbi_run_closure_on_scratch (production path). */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include <stddef.h>

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "tag/utag.h"
#include "vm/uvm.h"
#include "watcher/uwatcher.h"

#define UTEST(name) static void name(void)

/* compile_and_run / run_to_no_runnable / make_int / make_nil now live
 * in utest_e2e_helpers.{h,c}; see that header for documentation. */

/* ===================================================================
 * Test: scripted_tag_stop_drains_onleave
 *
 * 1. Pre-install Realm.x = 0, Realm.fired = 0, Realm.left = 0.
 * 2. Compile `at (Realm.x > 5) Realm.fired = Realm.fired + 1
 *      onleave Realm.left = Realm.left + 100`.  Watcher's owning_tag
 *      defaults to gr->tag (urbi_watcher_resolve_owning_tag fallback).
 * 3. Trigger rising edge: nested `Realm.x = 10`.  Body fires inline
 *      (M5 spawn_body for AT mode actually spawns a strand; drive to
 *      completion).  Realm.fired == 1; URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE
 *      set on the watcher.
 * 4. urbi_tag_stop(vm, gr->tag, nil) → watcher cascaded onto
 *      pending_onleave_head; tag's member_watchers_head cleared.
 * 5. Drive a no-op nested function call so the dispatcher safepoint
 *      runs urbi_watcher_drain_pending_onleave_queue → run_watcher_onleave.
 * 6. Verify Realm.left == 100 (onleave fired through real bytecode).
 *      Verify pending_onleave_head == NULL (watcher fully unregistered).
 * =================================================================== */
UTEST(scripted_tag_stop_drains_onleave)
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
    rc = urbi_realm_set_global(&vm, gr, "left",  4, utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    /* === Phase 1: install the at-watcher with onleave === */
    rc = utest_e2e_compile_and_run(&vm,
        "at (Realm.x > 5) Realm.fired = Realm.fired + 1 "
        "onleave Realm.left = Realm.left + 100",
        NULL);
    UASSERT_EQ(URBI_OK, rc);
    if (rc != URBI_OK) {
        urbi_vm_destroy(&vm);
        return;
    }

    UASSERT(vm.active_watchers_head != NULL);
    UWatcher *w = vm.active_watchers_head;
    /* urbi_watcher_resolve_owning_tag fallback: top-level install gets gr->tag. */
    UASSERT(w->owning_tag == gr->tag);
    UASSERT(w->onleave != NULL);

    /* === Phase 2: trigger rising edge to fire body === */
    rc = utest_e2e_compile_and_run(&vm,
        "var trigger = function() { Realm.x = 10 }; trigger()",
        NULL);
    if (rc != URBI_OK) {
        while (vm.active_watchers_head != NULL)
            urbi_watcher_unregister_internal(&vm, vm.active_watchers_head);
        urbi_vm_destroy(&vm);
        return;
    }

    /* AT mode: body strand spawned; drive to completion. */
    int step_rc = utest_e2e_run_to_no_runnable(&vm);
    UASSERT(step_rc != -1);

    UValue fired = {0};
    rc = urbi_realm_get_global(&vm, gr, "fired", 5, &fired);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT_EQ(1, (int)fired.v.i);

    /* Onleave has not fired yet — falling edge hasn't occurred and
     * tag-stop hasn't been invoked. */
    UValue left = {0};
    rc = urbi_realm_get_global(&vm, gr, "left", 4, &left);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)left.kind);
    UASSERT_EQ(0, (int)left.v.i);

    /* === Phase 3: stop the realm tag — cascade watcher to pending queue === */
    UValue nil = utest_e2e_make_nil();
    rc = urbi_tag_stop(&vm, gr->tag, nil);
    UASSERT_EQ(URBI_OK, rc);

    /* Watcher must be on the pending_onleave_queue. */
    UASSERT(vm.pending_onleave_head == w);
    /* Tag's member_watchers_head cleared by the cascade. */
    UASSERT(gr->tag->member_watchers_head == NULL);

    /* === Phase 4: drive a no-op nested call so the safepoint drains === */
    rc = utest_e2e_compile_and_run(&vm,
        "var safepoint_noop = function() { 0 }; safepoint_noop()",
        NULL);
    (void)rc;
    (void)utest_e2e_run_to_no_runnable(&vm);

    /* === Phase 5: verify onleave fired and queue is drained === */
    UASSERT(vm.pending_onleave_head == NULL);

    rc = urbi_realm_get_global(&vm, gr, "left", 4, &left);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)left.kind);
    UASSERT_EQ(100, (int)left.v.i);

    /* === Cleanup ===
     *
     * urbi_tag_stop cascaded ALL watchers off active list into pending,
     * and drain unregistered them all.  No active watchers should remain. */
    UASSERT(vm.active_watchers_head == NULL);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_tag_stop_onleave_scripted_suite(void)
{
    printf("test_tag_stop_onleave_scripted\n");
    utest_run("scripted_tag_stop_drains_onleave",
              scripted_tag_stop_drains_onleave);
}
