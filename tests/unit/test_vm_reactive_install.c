/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_vm_reactive_install.c — characterization pins for the seven
 * reactive-install dispatch arms (v0.10.15-vm-decomp-2, W1 stage 2).
 *
 * Stage 2 extracts OP_AT_INSTALL / OP_AT_SYNC_INSTALL / OP_WHENEVER_INSTALL /
 * OP_WAITUNTIL_INSTALL / OP_AT_EVENT_INSTALL / OP_AT_EVENT_SYNC_INSTALL /
 * OP_WHENEVER_EVENT_INSTALL into urbi_vm_reactive_install (uvm_reactive_install.c).
 *
 * These two pins exercise the two distinct install ENTRY POINTS end-to-end:
 *   - urbi_watcher_install_watcher_runtime (closure-cond family) via `at (cond) body`
 *   - urbi_watcher_install_at_event_runtime (event family)       via `at (e?) body`
 * and MUST pass identically before AND after the extraction (zero-delta gate).
 *
 * The per-mode variants and the irregular OP_WAITUNTIL_INSTALL park/fast-path
 * are already pinned at the dispatch level by test_at_install_dispatch.c
 * (at / whenever / at_sync / waituntil-cond-true), test_waituntil_install.c
 * (waituntil park-when-false + immediate-wake), and test_at_event_dispatch.c
 * (at_event / at_event_sync linkage); all run under the same zero-delta gate.
 * This file adds the high-level e2e confidence that the extracted switch routes
 * each entry point correctly. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* === Test 1: `at (cond) body` (closure-cond family, urbi_watcher_install_watcher_runtime)
 * installs and fires on the rising edge of an object-slot condition.  Mirrors
 * the install + trigger-in-a-function + run-to-quiescence pattern of
 * test_vm_slot_helpers.c case 6. */
UTEST(test_reactive_at_fires_on_rising_edge)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "rfired", 6,
                                              utest_e2e_make_int(0)));

    /* Object slot so the cond read traces through OP_GETSLOT into the read-set. */
    UASSERT_EQ(URBI_OK, utest_e2e_compile_and_run(&vm,
        "Realm.ro = Object.new(); Realm.ro.x = 0;", NULL));
    UASSERT_EQ(URBI_OK, utest_e2e_compile_and_run(&vm,
        "at (Realm.ro.x > 5) { Realm.rfired = Realm.rfired + 1 }", NULL));

    /* Watcher must be installed on the active chain. */
    UASSERT(vm.active_watchers_head != NULL);

    /* Trigger inside a function call so the watcher eval runs at the OP_RET
     * safepoint (mirrors test_vm_slot_helpers.c case 6). */
    UASSERT_EQ(URBI_OK, utest_e2e_compile_and_run(&vm,
        "var trig = function() { Realm.ro.x = 10 }; trig()", NULL));
    UASSERT(utest_e2e_run_to_no_runnable(&vm) != -1);

    UValue fired = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "rfired", 6, &fired));
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT(fired.v.i >= 1);

    /* Drain active watchers before destroy. */
    {
        extern void urbi_watcher_unregister_internal(UVM *, void *);
        while (vm.active_watchers_head != NULL)
            urbi_watcher_unregister_internal(&vm, vm.active_watchers_head);
    }

    urbi_vm_destroy(&vm);
}

/* === Test 2: `at (e?) body` (event family, urbi_watcher_install_at_event_runtime) installs
 * on an event and fires its body when the event is emitted.  The watcher joins
 * event->at_watchers_head (not the active chain), so no manual drain is needed
 * — urbi_vm_destroy reclaims it. */
UTEST(test_reactive_at_event_fires_on_emit)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t ev = urbi_event_register(&vm, r, "revt", NULL, NULL);
    UASSERT(ev != URBI_EVENT_ID_INVALID);

    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "efired", 6,
                                              utest_e2e_make_int(0)));
    UASSERT_EQ(URBI_OK, utest_e2e_compile_and_run(&vm,
        "at (revt?) { Realm.efired = Realm.efired + 1 }", NULL));

    urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
    UASSERT(utest_e2e_run_to_no_runnable(&vm) != -1);

    UValue fired = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "efired", 6, &fired));
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT(fired.v.i >= 1);

    urbi_vm_destroy(&vm);
}

/* === Suite entry. ==================================================== */
void
test_vm_reactive_install_suite(void)
{
    utest_run("urbi_vm_reactive_install: at (cond) body fires on rising edge",
              test_reactive_at_fires_on_rising_edge);
    utest_run("urbi_vm_reactive_install: at (e?) body fires on event emit",
              test_reactive_at_event_fires_on_emit);
}
