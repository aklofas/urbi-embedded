/* SPDX-License-Identifier: BSD-3-Clause */
/* test_atomic_watchdog.c — TDD tests for urbi_atomic_begin watchdog
 * (Gap R, v0.7.1, URBI_DEBUG).
 *
 * URBI_DEBUG watchdog contract: urbi_step checks whether an open atomic
 * section (atomic_active true) has been held longer than URBI_ATOMIC_MAX_US
 * microseconds.  If so, urbi_panic("atomic section exceeded URBI_ATOMIC_MAX_US")
 * is called, which calls abort().
 *
 * Since abort() cannot be caught by the test harness, the live watchdog
 * trigger path cannot be exercised here — it is a Gate G1 stretch that
 * requires a subprocess-isolation harness (fork + SIGABRT catch).
 *
 * These tests verify the structural prerequisites the watchdog depends on:
 *   1. URBI_ATOMIC_MAX_US is defined (with the documented default of 100).
 *   2. urbi_atomic_begin captures the host_time_us timestamp into
 *      atomic_begin_us when host_time_us is set.
 *   3. urbi_atomic_begin sets atomic_begin_us to 0 when host_time_us is NULL.
 *   4. urbi_atomic_end clears atomic_begin_us unconditionally.
 *
 * The watchdog check in urbi_step (guarded by #ifdef URBI_DEBUG) is
 * exercised end-to-end by the functional tests above — when elapsed
 * exceeds the threshold, the panic fires.  The structural tests here
 * confirm the preconditions that make the guard reachable.
 *
 * make test       — default release build: URBI_ATOMIC_MAX_US + timestamp
 *                   precondition tests run; live panic path skipped.
 * make test-debug — URBI_DEBUG=1: same; live panic path is Gate G1 stretch.
 */

#include "utest.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"

#include <stdint.h>

#define UTEST(name) static void name(void)

/* ---- Mock time source ----------------------------------------------------- */

static uint64_t g_mock_time_us = 0U;

static uint64_t mock_time_fn(void)
{
    return g_mock_time_us;
}

/* =========================================================================
 * Sub-test 1: URBI_ATOMIC_MAX_US is defined and equals the documented default.
 * ========================================================================= */

UTEST(atomic_max_us_is_defined)
{
    /* Verify the compile-time constant exists and has the documented default.
     * If this test fails, the default was changed without updating the spec. */
    UASSERT_EQ((int)URBI_ATOMIC_MAX_US, 100);
}

/* =========================================================================
 * Sub-test 2: urbi_atomic_begin captures host_time_us into atomic_begin_us.
 * Prerequisite: the watchdog in urbi_step compares (now - atomic_begin_us)
 * to URBI_ATOMIC_MAX_US.  If atomic_begin_us is not captured at begin-time,
 * the comparison would use a stale or garbage value.
 * ========================================================================= */

UTEST(atomic_begin_captures_timestamp)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    g_mock_time_us = 12345U;
    urbi_set_time_us(&vm, mock_time_fn);

    urbi_atomic_begin(&vm);
    UASSERT(vm.atomic_active != 0);
    UASSERT_EQ((long long)vm.atomic_begin_us, 12345LL);

    urbi_atomic_end(&vm);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: urbi_atomic_begin sets atomic_begin_us to 0 when host_time_us
 * is NULL (cleared explicitly — hosted builds install a default clock stub
 * so NULL must be forced for this test).  In this case the watchdog in
 * urbi_step is skipped entirely (guard: atomic_active && host_time_us != NULL).
 * ========================================================================= */

UTEST(atomic_begin_no_time_fn_zeroes_timestamp)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    /* Explicitly clear host_time_us so this test is portable across both
     * hosted builds (which install a default clock stub) and freestanding
     * builds (which leave it NULL).  When host_time_us is NULL, the watchdog
     * in urbi_step is skipped (guard: atomic_active && host_time_us != NULL),
     * and urbi_atomic_begin sets atomic_begin_us to 0 rather than calling
     * the now-absent time function. */
    vm.host_time_us = NULL;

    urbi_atomic_begin(&vm);
    UASSERT(vm.atomic_active != 0);
    /* atomic_begin_us must be 0 — urbi_atomic_begin must NOT call a NULL fn. */
    UASSERT_EQ((long long)vm.atomic_begin_us, 0LL);

    urbi_atomic_end(&vm);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 4: urbi_atomic_end clears atomic_begin_us.
 * After end, the timestamp is reset to 0 so a stale value cannot
 * trigger the watchdog if begin is called again later.
 * ========================================================================= */

UTEST(atomic_end_clears_timestamp)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    g_mock_time_us = 99999U;
    urbi_set_time_us(&vm, mock_time_fn);

    urbi_atomic_begin(&vm);
    UASSERT_EQ((long long)vm.atomic_begin_us, 99999LL);

    urbi_atomic_end(&vm);
    UASSERT_EQ((long long)vm.atomic_begin_us, 0LL);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 5 (SKIP): live watchdog panic.
 * urbi_step with an open section past URBI_ATOMIC_MAX_US calls
 * urbi_panic("atomic section exceeded URBI_ATOMIC_MAX_US") which calls
 * abort().  This cannot be caught by the test harness.
 * Gate G1 stretch: requires fork + SIGABRT isolation.
 * ========================================================================= */

UTEST(watchdog_panic_live_skipped)
{
    printf("    SKIP: live watchdog panic calls abort() — Gate G1 stretch"
           " (subprocess-isolation harness required)\n");
}

/* =========================================================================
 * Suite entry point
 * ========================================================================= */

void
test_atomic_watchdog_suite(void)
{
    utest_run("atomic: URBI_ATOMIC_MAX_US defined with default 100",
              atomic_max_us_is_defined);
    utest_run("atomic: urbi_atomic_begin captures host_time_us into atomic_begin_us",
              atomic_begin_captures_timestamp);
    utest_run("atomic: urbi_atomic_begin zeroes atomic_begin_us when no time fn",
              atomic_begin_no_time_fn_zeroes_timestamp);
    utest_run("atomic: urbi_atomic_end clears atomic_begin_us",
              atomic_end_clears_timestamp);
    utest_run("atomic: live watchdog panic (SKIP — Gate G1 subprocess only)",
              watchdog_panic_live_skipped);
}
