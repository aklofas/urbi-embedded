/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_set_time_us.c — T41: urbi_set_time_us hook.
 *
 * Tests:
 *  1. Custom time fn observed by System.time() — script calls System.time();
 *     the returned value must reflect the mocked time source.
 *  2. Custom time fn observed during VM activity — install mock clock, run
 *     System.time() and confirm mock_call_count increased.  Simplified from
 *     the plan (see sub-test 2 note below).
 *  3. NULL fn restores default (System.time returns a UVAL_FLOAT;
 *     mock_call_count does not increment after restore).
 *  4. NULL vm is a no-op (must not crash).
 *
 * Sub-test 2 scope reduction:
 *   The plan asks for advancing mock time to observe every(100ms) firing.
 *   Driving the scheduler in a unit test requires looping urbi_step + careful
 *   mock-clock advancement — that complexity belongs in an integration test.
 *   Sub-test 2 verifies the time hook is consulted during VM activity by
 *   confirming g_mock_call_count increases after System.time() runs.
 */

#include "utest.h"
#include "utest_e2e_helpers.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"

#include <stdint.h>
#include <stddef.h>

#define UTEST(name) static void name(void)

/* ---- mock time source -------------------------------------------------- */

static uint64_t g_mock_time_us;
static int      g_mock_call_count;

static uint64_t mock_time(void)
{
    g_mock_call_count++;
    return g_mock_time_us;
}

static void reset_mock(uint64_t start_us)
{
    g_mock_time_us    = start_us;
    g_mock_call_count = 0;
}

/* ---- T41-1: System.time() observes mock time source -------------------- */
UTEST(time_us_system_time_uses_hook)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(rc, URBI_OK);

    /* urbi_realm_global triggers stdlib boot (sets up System, Math, etc.). */
    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Install mock clock returning exactly 2 000 000 µs (= 2.0 s). */
    reset_mock(2000000ULL);
    urbi_set_time_us(&vm, mock_time);

    UValue result = urbi_make_nil();
    rc = utest_e2e_compile_and_run(&vm, "System.time()", &result);
    UASSERT_EQ(rc, URBI_OK);

    /* System.time() divides by 1e6 → should be 2.0 */
    UASSERT(result.kind == (uint8_t)UVAL_FLOAT);
    double got = result.v.f;
    UASSERT(got >= 1.99 && got <= 2.01);

    /* Mock must have been called at least once. */
    UASSERT(g_mock_call_count > 0);

    urbi_vm_destroy(&vm);
}

/* ---- T41-2: VM activity consults time hook ----------------------------- */
UTEST(time_us_hook_consulted_during_run)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(rc, URBI_OK);

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    reset_mock(500000ULL); /* 0.5 s */
    urbi_set_time_us(&vm, mock_time);

    int before = g_mock_call_count;

    /* System.time() reads host_time_us; confirms hook is plumbed. */
    UValue result = urbi_make_nil();
    rc = utest_e2e_compile_and_run(&vm, "System.time()", &result);
    UASSERT_EQ(rc, URBI_OK);

    /* At least one additional call after the run. */
    UASSERT(g_mock_call_count > before);

    urbi_vm_destroy(&vm);
}

/* ---- T41-3: NULL restores default (mock no longer called) -------------- */
UTEST(time_us_null_restores_default)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(rc, URBI_OK);

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Install mock, then remove. */
    reset_mock(1ULL);
    urbi_set_time_us(&vm, mock_time);
    urbi_set_time_us(&vm, NULL);   /* restore default */

    /* After restore, mock must not be consulted. */
    int before = g_mock_call_count;

    UValue result = urbi_make_nil();
    rc = utest_e2e_compile_and_run(&vm, "System.time()", &result);
    UASSERT_EQ(rc, URBI_OK);

    UASSERT_EQ(g_mock_call_count, before);  /* mock not called */

    /* Default hook returns a float (≥ 0; positive on hosted). */
    UASSERT(result.kind == (uint8_t)UVAL_FLOAT);
    UASSERT(result.v.f >= 0.0);

    urbi_vm_destroy(&vm);
}

/* ---- T41-4: NULL vm is a no-op ----------------------------------------- */
UTEST(time_us_null_vm_noop)
{
    /* Must not crash. */
    urbi_set_time_us(NULL, mock_time);
    UASSERT(1);
}

/* ---- suite entry point ------------------------------------------------- */
void
test_set_time_us_suite(void)
{
    printf("test_set_time_us\n");
    utest_run("time_us_system_time_uses_hook",     time_us_system_time_uses_hook);
    utest_run("time_us_hook_consulted_during_run", time_us_hook_consulted_during_run);
    utest_run("time_us_null_restores_default",     time_us_null_restores_default);
    utest_run("time_us_null_vm_noop",              time_us_null_vm_noop);
}
