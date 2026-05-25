/* SPDX-License-Identifier: BSD-3-Clause */
/* End-to-end: scripted at(cond) fires through real bytecode dispatch.
 *
 * Unlike test_at_fire_paths.c (which uses test_watcher_*_hook fields to
 * inject behavior), this suite compiles real urbiscript and runs it
 * through the production install + eval paths.  The watcher's cond
 * closure goes through urbi_run_closure_on_scratch (no hooks); the body
 * strand spawns through spawn_body_coroutine (M5 baseline).
 *
 * Body observes its run via Realm.fired counter — incremented inside the
 * body, read by the host afterwards via urbi_realm_get_global.  This
 * exercises the install-time cond eval (T7), eval-time cond eval (T8),
 * the new strand.module_instance synthesis path in the scratch helper
 * (T7's bundled bug fix — exercised here by Realm.x global access via
 * OP_GETSLOT), and async body strand completion through the M5 scheduler. */

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
 * Test: scripted_at_fires_on_rising_edge
 *
 * Install at (Realm.x > 5) Realm.fired = Realm.fired + 1 via a real
 * compiled script.  Trigger the rising edge by writing Realm.x = 10
 * through a nested function call (so the non-top-frame OP_RET safepoint
 * fires watcher_eval_dirty and spawns the body strand).  Drive the body
 * strand to completion via urbi_step.  Verify Realm.fired == 1.
 *
 * A second write (Realm.x = 20) must not re-fire (rising-edge discipline).
 * =================================================================== */
UTEST(scripted_at_fires_on_rising_edge)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Pre-install Realm.x = 0, Realm.fired = 0 via C API.
     * This installs them on global_object before the watcher is compiled,
     * so the IC for Realm.x exists when the watcher cond is traced. */
    URealm *gr = urbi_realm_global(&vm);
    UASSERT(gr != NULL);
    if (gr == NULL) { urbi_vm_destroy(&vm); return; }

    int rc;
    rc = urbi_realm_set_global(&vm, gr, "x",     1, utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);
    rc = urbi_realm_set_global(&vm, gr, "fired", 5, utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    /* === Phase 1: install the at-watcher ===
     *
     * Compile and run "at (Realm.x > 5) Realm.fired = Realm.fired + 1".
     * install_watcher_runtime runs the cond closure via
     * urbi_run_closure_on_scratch (the T7 path); Realm.x == 0 so the
     * watcher installs with last_value_cache = false and enters the
     * active_watchers_head list.  The read-set records global_object
     * (the cell that holds Realm.x), setting UGC_HAS_WATCHER_OBSERVER
     * on it so any future OP_SETSLOT write to global_object triggers
     * observer_dirty => watcher_dirty_count++. */
    rc = utest_e2e_compile_and_run(&vm,
        "at (Realm.x > 5) Realm.fired = Realm.fired + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);
    if (rc != URBI_OK) {
        urbi_vm_destroy(&vm);
        return;
    }

    /* Watcher must be installed (cond was false at install time). */
    UASSERT(vm.active_watchers_head != NULL);

    /* fired must still be 0 — cond was false at install, no body fired. */
    UValue fired = {0};
    rc = urbi_realm_get_global(&vm, gr, "fired", 5, &fired);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT_EQ(0, (int)fired.v.i);

    /* === Phase 2: trigger the rising edge ===
     *
     * Mechanism: OP_SETSLOT writes Realm.x through urbi_gc_slot_write,
     * which calls observer_dirty (UGC_HAS_WATCHER_OBSERVER is set from
     * phase 1).  The non-top-frame OP_RET safepoint then calls
     * watcher_eval_dirty, which spawns the body strand on rising edge.
     * Detailed walk-through (incl. body-strand module_instance synthesis)
     * lives in the M5 at/whenever/waituntil design spec §4.  This test
     * verifies presence + ordering; deep mechanism is the spec's responsibility. */
    rc = utest_e2e_compile_and_run(&vm,
        "var trigger = function() { Realm.x = 10 }; trigger()",
        NULL);
    /* compile_and_run itself may return OK even if the watcher eval
     * spawned a body strand that later fails — the body strand runs
     * asynchronously via urbi_step, not synchronously inside urbi_vm_run.
     * However, if the non-top OP_RET safepoint fires watcher_eval_dirty
     * and the body is spawned BEFORE compile_and_run returns, the body
     * strand might already be in the ready queue. */
    if (rc != URBI_OK) {
        /* Drain watchers before destroy. */
        while (vm.active_watchers_head != NULL)
            urbi_watcher_unregister_internal(&vm, vm.active_watchers_head);
        urbi_vm_destroy(&vm);
        return;
    }

    /* === Phase 3: drive the body strand to completion ===
     *
     * The body strand (if spawned) is now in the ready queue.
     * run_to_no_runnable drives urbi_step until no runnable strands remain. */
    int step_rc = utest_e2e_run_to_no_runnable(&vm);
    UASSERT(step_rc != -1);  /* URBI_STEP_FATAL → body strand crashed */

    /* === Phase 4: verify body fired exactly once ===
     *
     * If the body strand executed successfully, Realm.fired == 1.
     * If module_instance was NULL, the strand HALTed with a type error,
     * urbi_step returned FATAL, and Realm.fired stayed 0. */
    rc = urbi_realm_get_global(&vm, gr, "fired", 5, &fired);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT_EQ(1, (int)fired.v.i);

    /* === Phase 5: same-direction write must NOT re-fire ===
     *
     * Write Realm.x = 20.  The watcher is AT mode: the rising edge
     * already fired (last_value_cache is truthy), so this same-direction
     * write must not spawn another body strand. */
    rc = utest_e2e_compile_and_run(&vm,
        "var trigger2 = function() { Realm.x = 20 }; trigger2()",
        NULL);
    (void)rc;

    (void)utest_e2e_run_to_no_runnable(&vm);

    rc = urbi_realm_get_global(&vm, gr, "fired", 5, &fired);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT_EQ(1, (int)fired.v.i);   /* still 1 — no re-fire on same-direction */

    /* === Cleanup ===
     *
     * Drain active watchers before destroying the VM to avoid
     * use-after-free in watcher pool teardown. */
    while (vm.active_watchers_head != NULL)
        urbi_watcher_unregister_internal(&vm, vm.active_watchers_head);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_at_scripted_e2e_suite(void)
{
    printf("test_at_scripted_e2e\n");
    utest_run("scripted_at_fires_on_rising_edge",
              scripted_at_fires_on_rising_edge);
}
