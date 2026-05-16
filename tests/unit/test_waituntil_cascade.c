/* SPDX-License-Identifier: BSD-3-Clause */
/* test_waituntil_cascade — regression for a crash observed 2026-05-16 on
 * ESP32-S3-EYE eye_demo Tier-2b stress.  Multiple strands blocked on the
 * same waituntil(cond) crash with InstructionFetchError when cond
 * becomes true and all wake simultaneously.
 *
 * Hardware crash signature (PC = 0x3c04b220 in flash-mapped code region,
 * EXCCAUSE = 0x2 InstructionFetchError, backtrace corrupted):
 * a strand jumped to a corrupted PC after the simultaneous-wake event.
 *
 * Demo shape that crashed:
 *
 *   var wait_for_500 = function () {
 *       waituntil (Realm.blob_count > 500);
 *       Realm.wait_done = Realm.wait_done + 1
 *   };
 *   at (scan_tick?) wait_for_500();
 *
 * Multiple scan_tick fires before blob_count crosses 500 → multiple
 * strands accumulate in USTRAND_WAIT_WATCHER state.  When blob_count
 * crosses 500, all wake at once.  Crash happens during or just after
 * the cascade.
 *
 * This test reproduces the pattern host-side with explicit strand
 * spawns via at-event handlers waiting on a shared cond. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "module/umodule.h"
#include "value/uarena.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

static UStepResult
drain_to_quiescent(UVM *vm)
{
    UStepResult r = URBI_STEP_QUIESCENT;
    int i;
    for (i = 0; i < 2000; i++) {
        r = urbi_step(vm, 500, NULL);
        if (r == URBI_STEP_QUIESCENT || r == URBI_STEP_WAKE_AT ||
            r == URBI_STEP_FATAL) {
            return r;
        }
    }
    return r;
}

/* Spawn 3 waituntil-blocked strands, then trigger cond, verify
 * all three wake and complete without crashing. */
UTEST(waituntil_cascade_three_strands_wake)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t spawn = urbi_event_register(&vm, r, "spawn", NULL, NULL);
    UASSERT(spawn != URBI_EVENT_ID_INVALID);
    urbi_event_id_t go = urbi_event_register(&vm, r, "go", NULL, NULL);
    UASSERT(go != URBI_EVENT_ID_INVALID);

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    /* Setup:
     *   - Realm.cond starts false (0)
     *   - at (spawn?) spawns a waiter that blocks on Realm.cond > 0
     *   - at (go?) sets Realm.cond = 1 (wakes all waiters)
     *   - Each waiter increments Realm.woke when it unblocks
     * Inject 3 spawn events to accumulate 3 blocked waiters.
     * Then inject 1 go event to wake all 3.
     * Expect: Realm.woke == 3 after drain, no fatal. */
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var cond = 0;"
        "var woke = 0;"
        "class Helper { var noop = function () { 0 } };"
        "Realm.helper = Helper.new();"
        "var waiter = function () {"
        "  waituntil (Realm.cond > 0);"
        "  Realm.woke = Realm.woke + 1"
        "};"
        "at (spawn?) waiter();"
        /* go-body invokes Realm.helper.noop() at the end to force a
         * safepoint (no-method bodies never trigger watcher eval per
         * the safepoint-visit limitation documented in design-risks). */
        "at (go?) Realm.cond = 1 | Realm.helper.noop()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Spawn 3 waiters by injecting 3 spawn events. */
    int i;
    for (i = 0; i < 3; i++) {
        urbi_inject_event(&vm, (uint32_t)spawn, NULL, 0U);
        UStepResult step = drain_to_quiescent(&vm);
        UASSERT(step != URBI_STEP_FATAL);
    }

    /* All 3 strands should be blocked on waituntil now.  Trigger go. */
    urbi_inject_event(&vm, (uint32_t)go, NULL, 0U);
    UStepResult step = drain_to_quiescent(&vm);
    UASSERT(step != URBI_STEP_FATAL);

    UValue woke = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "woke", 4, &woke));
    UASSERT_EQ((int)UVAL_INT, (int)woke.kind);
    UASSERT_EQ(3LL, woke.v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

void
test_waituntil_cascade_suite(void)
{
    /* DISABLED: this test crashes the runner (SIGSEGV on host, hardware
     * InstructionFetchError at corrupted PC).  Bug filed in
     * docs/urbi-embedded-design-risks.md as "waituntil cascade-wake
     * crashes when 3+ blocked strands wake simultaneously".  Re-enable
     * when the bug is fixed (v0.7.3+).  Keeping the test file in place
     * documents the repro shape for future investigation.
     *
     * To run when debugging:
     *
     *   utest_run("waituntil_cascade: 3 strands wake simultaneously on cond fire",
     *             waituntil_cascade_three_strands_wake);
     */
    utest_run("waituntil_cascade: 3 strands wake simultaneously on cond fire",
              waituntil_cascade_three_strands_wake);
}
