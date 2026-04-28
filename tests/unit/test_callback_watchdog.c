/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: ISR-safety assertions + URBI_DEBUG callback watchdog (T19).
 *
 * All watchdog tests are compiled only in URBI_DEBUG builds.  When URBI_DEBUG
 * is not defined, only the trivial compilation-smoke test runs to verify the
 * file compiles and links in both modes.
 *
 * make test       — default build: 1 case (smoke).
 * make test-debug — URBI_DEBUG=1: 1 smoke + 4 watchdog + 3 ISR-check cases. */

#include "utest.h"
#include "urbi.h"
#include "uvm.h"
#include "ustrand.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define UTEST(name) static void name(void)

/* ---- Smoke test: verify the file compiles in both debug and release modes --- */

UTEST(watchdog_release_build_compiles_out)
{
    /* Verify that URBI_CALLBACK_WARN_US, URBI_WATCHDOG_WARN, URBI_WATCHDOG_ASSERT
     * are defined in all builds. */
    UASSERT(URBI_CALLBACK_WARN_US > 0u);
    UASSERT(URBI_WATCHDOG_WARN  == 0);
    UASSERT(URBI_WATCHDOG_ASSERT == 1);
    UASSERT(1);  /* always passes */
}

/* ---- URBI_DEBUG-only tests ------------------------------------------------- */

#ifdef URBI_DEBUG

/* Mock state for watchdog and ISR tests. */

static int g_log_called;
static int g_log_level;
static uint64_t g_mock_time;

/* Mock host_log_fn: records calls and log level. */
static void mock_log_fn(struct UVM *vm, int level, const char *fmt, ...)
{
    (void)vm;
    (void)fmt;
    g_log_called++;
    g_log_level = level;
}

/* Controllable mock clock: increments by a configurable amount on each call. */
static uint64_t g_time_step_us;
static uint64_t mock_time_us(void)
{
    uint64_t t = g_mock_time;
    g_mock_time += g_time_step_us;
    return t;
}

/* Fast mock host function: returns nil immediately. */
static UValue fast_host_fn(struct UStrand *s, int argc, UValue *argv)
{
    UValue r;
    (void)s; (void)argc; (void)argv;
    r.kind = 0;
    r.v.i  = 0;
    return r;
}

/* Slow mock host function: same as fast but the mock clock steps past threshold. */
static UValue slow_host_fn(struct UStrand *s, int argc, UValue *argv)
{
    UValue r;
    (void)s; (void)argc; (void)argv;
    r.kind = 0;
    r.v.i  = 0;
    /* The slow effect comes from g_time_step_us being set large before the
     * watchdog wrapper is invoked; the function itself does nothing special. */
    return r;
}

/* ---- Case 1: fast callback does not trigger the watchdog log -------------- */

UTEST(watchdog_fast_callback_no_warn)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* Install mock time and log hooks. */
    vm.host_time_us = mock_time_us;
    vm.host_log_fn  = mock_log_fn;
    vm.callback_warn_us = 1000u;
    vm.callback_watchdog_mode = URBI_WATCHDOG_WARN;

    g_mock_time    = 0u;
    g_time_step_us = 100u;   /* each call advances by 100 µs — well under 1000 µs */
    g_log_called   = 0;
    g_log_level    = -1;

    UValue result = urbi_call_host_with_watchdog(&vm, NULL, fast_host_fn, 0, NULL);

    /* Elapsed = second_call − first_call = (0+100) − 0 = 100 µs < 1000 µs. */
    UASSERT(g_log_called == 0);
    UASSERT(result.kind == 0);

    uvm_destroy(&vm);
}

/* ---- Case 2: slow callback triggers a WARN log call ----------------------- */

UTEST(watchdog_slow_callback_warns)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    vm.host_time_us = mock_time_us;
    vm.host_log_fn  = mock_log_fn;
    vm.callback_warn_us = 1000u;
    vm.callback_watchdog_mode = URBI_WATCHDOG_WARN;

    g_mock_time    = 0u;
    g_time_step_us = 2000u;  /* each clock read advances 2000 µs; elapsed = 2000 > 1000 */
    g_log_called   = 0;
    g_log_level    = -1;

    (void)urbi_call_host_with_watchdog(&vm, NULL, slow_host_fn, 0, NULL);

    /* Elapsed = 2000 µs > 1000 µs threshold → WARN should be logged. */
    UASSERT(g_log_called == 1);
    UASSERT(g_log_level  == URBI_LOG_WARN);

    uvm_destroy(&vm);
}

/* ---- Case 3: watchdog in WARN mode with no log fn — no crash -------------- */

UTEST(watchdog_slow_no_log_fn_is_silent)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    vm.host_time_us = mock_time_us;
    vm.host_log_fn  = NULL;  /* no log fn installed */
    vm.callback_warn_us = 500u;
    vm.callback_watchdog_mode = URBI_WATCHDOG_WARN;

    g_mock_time    = 0u;
    g_time_step_us = 1000u;  /* elapsed = 1000 > 500; but no log_fn → silently drop */

    /* Must not crash or misbehave. */
    (void)urbi_call_host_with_watchdog(&vm, NULL, slow_host_fn, 0, NULL);
    UASSERT(1);  /* reached here without crash */

    uvm_destroy(&vm);
}

/* ---- Case 4: isr_check_fn returns false → non-ISR-safe call succeeds ------ */

/* Predicate returning false (normal, non-ISR context). */
static bool isr_check_not_in_isr(void) { return false; }

UTEST(isr_check_fn_set_returns_false_no_panic)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    urbi_set_isr_check_fn(&vm, isr_check_not_in_isr);
    UASSERT(vm.isr_check_fn == isr_check_not_in_isr);

    /* urbi_realm_global has URBI_ASSERT_NOT_ISR; isr_check_fn returns false
     * → no panic.  Should return a valid Realm. */
    struct URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    uvm_destroy(&vm);
}

/* ---- Case 5: urbi_set_callback_watchdog_mode stores the mode -------------- */

UTEST(set_watchdog_mode_stored)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT(vm.callback_watchdog_mode == URBI_WATCHDOG_WARN);
    urbi_set_callback_watchdog_mode(&vm, URBI_WATCHDOG_ASSERT);
    UASSERT(vm.callback_watchdog_mode == URBI_WATCHDOG_ASSERT);
    urbi_set_callback_watchdog_mode(&vm, URBI_WATCHDOG_WARN);
    UASSERT(vm.callback_watchdog_mode == URBI_WATCHDOG_WARN);

    uvm_destroy(&vm);
}

/* ---- Case 6: urbi_set_isr_check_fn stores and clears the fn pointer ------- */

UTEST(set_isr_check_fn_stores_and_clears)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT(vm.isr_check_fn == NULL);
    urbi_set_isr_check_fn(&vm, isr_check_not_in_isr);
    UASSERT(vm.isr_check_fn == isr_check_not_in_isr);
    urbi_set_isr_check_fn(&vm, NULL);
    UASSERT(vm.isr_check_fn == NULL);

    uvm_destroy(&vm);
}

/* ---- Case 7: watchdog ASSERT mode when callback is fast — no panic -------- */

UTEST(watchdog_assert_mode_fast_no_panic)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    vm.host_time_us = mock_time_us;
    vm.host_log_fn  = mock_log_fn;
    vm.callback_warn_us = 1000u;
    vm.callback_watchdog_mode = URBI_WATCHDOG_ASSERT;

    g_mock_time    = 0u;
    g_time_step_us = 50u;   /* well under threshold */
    g_log_called   = 0;

    (void)urbi_call_host_with_watchdog(&vm, NULL, fast_host_fn, 0, NULL);

    /* Fast callback in ASSERT mode: no panic, no log. */
    UASSERT(g_log_called == 0);

    uvm_destroy(&vm);
}

/* NOTE: watchdog_assert_mode_fires (slow callback in ASSERT mode) calls
 * urbi_panic → abort().  This cannot be tested in-process without setjmp/longjmp
 * patching of urbi_panic, which is beyond the M3 scope.  The assert path is
 * exercised by the code path in urbi_call_host_with_watchdog and is verified by
 * inspection.  Deferred to a future death-test framework (T40 or similar). */

#endif /* URBI_DEBUG */

/* ---- Suite registration ---------------------------------------------------- */

void test_callback_watchdog_suite(void)
{
    utest_run("watchdog_release_build_compiles_out",
              watchdog_release_build_compiles_out);
#ifdef URBI_DEBUG
    utest_run("watchdog_fast_callback_no_warn",
              watchdog_fast_callback_no_warn);
    utest_run("watchdog_slow_callback_warns",
              watchdog_slow_callback_warns);
    utest_run("watchdog_slow_no_log_fn_is_silent",
              watchdog_slow_no_log_fn_is_silent);
    utest_run("isr_check_fn_set_returns_false_no_panic",
              isr_check_fn_set_returns_false_no_panic);
    utest_run("set_watchdog_mode_stored",
              set_watchdog_mode_stored);
    utest_run("set_isr_check_fn_stores_and_clears",
              set_isr_check_fn_stores_and_clears);
    utest_run("watchdog_assert_mode_fast_no_panic",
              watchdog_assert_mode_fast_no_panic);
#endif /* URBI_DEBUG */
}
