/* SPDX-License-Identifier: BSD-3-Clause */
/* test_atomic_nesting.c — TDD tests for urbi_atomic_begin nesting detection
 * (Gap R, v0.7.1, URBI_DEBUG).
 *
 * URBI_DEBUG contract: calling urbi_atomic_begin while atomic_active is
 * already true calls urbi_panic("atomic section nested"), which calls abort().
 * Since abort() cannot be caught by the test harness, the live double-begin
 * path cannot be exercised here — it is a Gate G1 stretch (only fires with
 * URBI_DEBUG on).
 *
 * These tests verify the structural invariants that the guard depends on:
 *   1. atomic_active is 0 after urbi_vm_init (guard would not fire).
 *   2. atomic_active is 1 after urbi_atomic_begin (guard would fire on a
 *      second call in URBI_DEBUG builds).
 *   3. atomic_active is 0 after urbi_atomic_end (guard would not fire again).
 *
 * make test       — default release build: all 3 structural cases run.
 * make test-debug — URBI_DEBUG=1: same 3 cases; the live panic path is
 *                   documented but not called (would abort the test process).
 */

#include "utest.h"

#include "vm/uvm.h"
#include "urbi/urbi.h"

#include <stdint.h>

#define UTEST(name) static void name(void)

/* =========================================================================
 * Sub-test 1: atomic_active is zero after urbi_vm_init.
 * The URBI_DEBUG guard would NOT fire on first urbi_atomic_begin.
 * ========================================================================= */

UTEST(atomic_active_zero_after_init)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    UASSERT_EQ((int)vm.atomic_active, 0);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: atomic_active is set after urbi_atomic_begin.
 * A second call to urbi_atomic_begin in URBI_DEBUG would fire the panic.
 * This test only calls begin once — safe in all build modes.
 * ========================================================================= */

UTEST(atomic_active_set_after_begin)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    urbi_atomic_begin(&vm);
    UASSERT(vm.atomic_active != 0);

    /* Clean up to avoid stale state in urbi_vm_destroy. */
    urbi_atomic_end(&vm);
    UASSERT_EQ((int)vm.atomic_active, 0);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: atomic_active is cleared by urbi_atomic_end.
 * After end, a fresh urbi_atomic_begin would NOT fire the URBI_DEBUG guard.
 * ========================================================================= */

UTEST(atomic_active_cleared_after_end)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    urbi_atomic_begin(&vm);
    UASSERT(vm.atomic_active != 0);

    urbi_atomic_end(&vm);
    UASSERT_EQ((int)vm.atomic_active, 0);

    /* Second begin after end must be safe (no double-begin). */
    urbi_atomic_begin(&vm);
    UASSERT(vm.atomic_active != 0);
    urbi_atomic_end(&vm);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point
 * ========================================================================= */

void
test_atomic_nesting_suite(void)
{
    utest_run("atomic: atomic_active zero after urbi_vm_init",
              atomic_active_zero_after_init);
    utest_run("atomic: atomic_active set after urbi_atomic_begin (single-begin safe)",
              atomic_active_set_after_begin);
    utest_run("atomic: atomic_active cleared by urbi_atomic_end",
              atomic_active_cleared_after_end);
}
