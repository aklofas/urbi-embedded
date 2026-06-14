/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_periodic_cadence.c — TDD tests for SCHED-14 (v0.13.3):
 *
 *   every_float_arg_is_seconds:
 *     A bare float argument to every() must be treated as SECONDS, matching
 *     sleep().  Pre-fix: float period was truncated to µs via (uint64_t)f,
 *     so every(0.05) → period_us = 0, firing every pump pass.
 *     Post-fix: period_us = (uint64_t)(int64_t)(f * 1e6) = 50000.
 *
 *   every_cadence_is_fixed_with_slide:
 *     urbi_periodic_body_completed must re-arm by advancing next_fire_us
 *     by one period from the PREVIOUS deadline (fixed/legacy every| cadence),
 *     not from now (after-completion cadence).
 *     Pre-fix: next_fire_us = now + period (after-completion: 130000).
 *     Post-fix on overrun: next_fire_us resumes at now + period (NOT the
 *     literal SCHED-14 slide-to-now).  This pins the controller-ratified
 *     skip-missed-periods deviation — see every_overrun_resumes_and_quiesces
 *     and the temporal.c re-arm comment for why slide-to-now hangs.
 *
 *   every_overrun_resumes_and_quiesces:
 *     ANTI-HANG regression.  A periodic whose body consistently overruns its
 *     period (advances the mock clock past the deadline each fire) must still
 *     let urbi_step reach WAKE_AT/QUIESCENT between fires — exactly one fire
 *     per deadline crossing, no perpetual-RUNNING burst.  FAILS (caps out,
 *     does not hang) if the re-arm slide target is reverted to literal `now`.
 *
 *   every_and_sleep_reject_nonfinite:
 *     INFINITY and too-large values must be rejected before the cast
 *     (uint64_t)(int64_t)(f * 1e6), which is UB for out-of-range doubles.
 *     every(inf) → no periodic installed (vm->periodics_head stays NULL).
 *     sleep(inf) → no strand parked  (vm->wakeup_pending_count stays 0).
 */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "stdlib/temporal.h"   /* urbi_periodic_body_completed, UPeriodic */
#include "runtime/umacros.h"   /* urbi_zero */

#include <math.h>     /* INFINITY, NAN */
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ---- mock clock shared by cadence and nonfinite tests ---- */

static uint64_t g_cadence_now_us;
static uint64_t cadence_mock_clock(void *ud) { (void)ud; return g_cadence_now_us; }

/* ---- overrun body for every_overrun_resumes_and_quiesces ---- */

/* Each fire advances the mock clock past the deadline (body "runs" for >= one
 * period) and counts the fire.  Installed via urbi_register and called as the
 * bare global tick() from the every() body — mirrors the reviewer's overrun
 * shape (body duration >= period). */
static uint64_t g_overrun_fires;
static uint64_t g_overrun_bump_us;
static int
overrun_tick(struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    g_overrun_fires++;
    g_cadence_now_us += g_overrun_bump_us;
    *out = urbi_make_nil();
    return 0;
}

/* =========================================================================
 * Test 1: every(0.05) → period_us = 50000 (float = SECONDS)
 * =========================================================================
 *
 * Oracle: after installing every(0.05) { 1 }, read vm->periodics_head->period_us.
 *   Pre-fix: (uint64_t)(0.05) = 0 (truncated).
 *   Post-fix: (uint64_t)(int64_t)(0.05 * 1e6) = 50000.
 *
 * Implementation note: utest_e2e_compile_and_run drives urbi_run_chunk
 * (not urbi_repl_eval), so there is no extra REPL drain loop.  urbi_run_chunk
 * exits once the loader strand dies; period_us is already written. */
UTEST(every_float_arg_is_seconds)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* NULL clock: every_native reads now=0 and sets next_fire_us = period_us. */

    /* Install every(0.05) { 1 }.  This calls every_native with args[0]=UVAL_FLOAT(0.05)
     * and args[1]=UVAL_CLOSURE wrapping the body `1`. */
    int rc = utest_e2e_compile_and_run(&vm, "every(0.05) { 1 }", NULL);
    /* expect URBI_OK (every_native returns nil; type error would give URBI_ERR_STRAND_FATAL) */
    UASSERT_EQ(URBI_OK, rc);

    /* The periodic must have been installed. */
    UASSERT(vm.periodics_head != NULL);

    /* SCHED-14 (owner-decided 2026-06-11): bare float = SECONDS.
     * 0.05s × 1e6 = 50000µs.
     * Pre-fix red: period_us = 0 (truncated float).
     * Post-fix green: period_us = 50000. */
    UASSERT_EQ(50000ULL, (unsigned long long)vm.periodics_head->period_us);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Test 2: fixed cadence with slide-to-now catch-up
 * =========================================================================
 *
 * Directly exercises urbi_periodic_body_completed with a synthetic UPeriodic
 * and stub UStrand (no scheduler involvement).
 *
 *   Setup:  period_us = 100000 (100ms), next_fire_us = 0 (fired at t=0),
 *           mock clock advances to 30000 (body "ran" for 30ms).
 *
 *   Pre-fix (after-completion):
 *     p->next_fire_us = now + period_us = 30000 + 100000 = 130000.
 *
 *   Post-fix (fixed cadence):
 *     p->next_fire_us += period_us = 0 + 100000 = 100000.
 *     100000 > 30000 (now), so no slide.
 *
 * A second call with clock advanced to 250000 tests the OVERRUN slide path:
 *   the deadline is already in the past (250000 > 100000 + 100000 = 200000).
 *
 *   This assertion pins the CONTROLLER-RATIFIED skip-missed-periods deviation,
 *   NOT the literal SCHED-14 "slide-to-now / single late fire": on overrun the
 *   deadline resumes at now + period (250000 + 100000 = 350000), so the missed
 *   periods are skipped and the periodic pump (which re-reads the clock) does
 *   not find the just-slid deadline still due and re-fire within the same
 *   urbi_step.  Resuming at the literal `now` (250000) instead would make the
 *   periodic perpetually-due and hang the VM — see the temporal.c re-arm
 *   comment and the every_overrun_resumes_and_quiesces regression below. */
UTEST(every_cadence_is_fixed_with_slide)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    vm.host_time_us = cadence_mock_clock;
    vm.host_time_ud = NULL;
    g_cadence_now_us = 0;

    /* Allocate a UPeriodic directly (bypass the scheduler; not on periodics_head). */
    UPeriodic *p = (UPeriodic *)vm.alloc_fn(NULL, sizeof(UPeriodic), vm.alloc_ud);
    UASSERT(p != NULL);
    urbi_zero(p, sizeof *p);
    p->period_us    = 100000U;   /* 100ms */
    p->next_fire_us = 0U;        /* body fired at t=0 */

    /* Stub UStrand: zero-initialized, fatal_status = UEXEC_OK.
     * urbi_periodic_body_completed only reads s->periodic_owner and
     * s->fatal_status; cleanup_base NULL is safe (not touched on this path). */
    UStrand s;
    memset(&s, 0, sizeof s);
    s.fatal_status  = UEXEC_OK;
    s.periodic_owner = p;
    p->current_strand = &s;

    /* Simulate 30ms body run time. */
    g_cadence_now_us = 30000U;

    urbi_periodic_body_completed(&vm, &s);

    /* SCHED-14 (owner-decided 2026-06-11): fixed cadence — advance by period
     * from previous deadline (0), not from now (30000).
     * Pre-fix red: 30000 + 100000 = 130000.
     * Post-fix green: 0 + 100000 = 100000 (no slide since 100000 > 30000). */
    UASSERT_EQ(100000ULL, (unsigned long long)p->next_fire_us);

    /* Verify the back-pointers were cleared by the completion hook. */
    UASSERT(s.periodic_owner   == NULL);
    UASSERT(p->current_strand  == NULL);

    /* --- Second call: slide-to-now path ---------------------------------- */
    /* Re-arm manually for the overrun scenario:
     *   period_us = 100000, next_fire_us = 100000, clock = 250000.
     *   250000 > 100000 + 100000 (post-fix advances to 200000, then
     *   200000 <= 250000 → slides to 250000). */
    p->next_fire_us   = 100000U;
    p->current_strand = &s;
    s.periodic_owner  = p;
    s.fatal_status    = UEXEC_OK;

    g_cadence_now_us  = 250000U;

    urbi_periodic_body_completed(&vm, &s);

    /* Overrun: 100000 + 100000 = 200000 <= 250000 → resume at now + period
     *          = 250000 + 100000 = 350000 (controller-ratified skip-missed-
     *          periods; NOT the literal slide-to-now = 250000, which hangs).
     * Pre-fix (after-completion): 250000 + 100000 = 350000 (no slide logic);
     *          this case does not distinguish pre/post — the non-slide case 1
     *          assertion (100000 vs pre-fix 130000) is what pins the re-arm
     *          base.  This assertion pins the overrun RESUME TARGET. */
    UASSERT_EQ(350000ULL, (unsigned long long)p->next_fire_us);

    /* Cleanup: free the UPeriodic directly (not on periodics_head). */
    vm.alloc_fn(p, 0, vm.alloc_ud);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Test 3: every() and sleep() reject INFINITY and overflow values
 * =========================================================================
 *
 * The old guard `f < 0.0 || f != f` passes INFINITY through to the cast
 * (uint64_t)(int64_t)(f * 1e6), which is C UB for out-of-range values.
 * The new guard `!(f >= 0.0) || f > 9.2e12` catches:
 *   - NaN      (!(NaN >= 0.0) is true)
 *   - -inf     (same)
 *   - +INFINITY (INFINITY > 9.2e12 is true)
 *   - very large finite values (9.3e13 > 9.2e12, µs conversion overflows)
 *
 * Oracles:
 *   every(inf)   → vm->periodics_head == NULL (no periodic installed)
 *   sleep(inf)   → vm->wakeup_pending_count == 0 (no strand parked)
 *
 * INFINITY cannot be used as a float literal in urbiscript source (the lexer
 * returns LEX_FLOAT_OVERFLOW for out-of-range values like 1e309).  Instead we
 * inject a UVAL_FLOAT(INFINITY) via urbi_realm_set_global and read it from
 * script as Realm.inf_val.
 *
 * The very-large-finite test (9.3e13) uses a float literal because 9.3e13 is
 * within the double range and passes the lexer. */
UTEST(every_and_sleep_reject_nonfinite)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    vm.host_time_us = cadence_mock_clock;
    vm.host_time_ud = NULL;
    g_cadence_now_us = 0;

    /* Prime the stdlib boot so typeerror_proto is resolved (affects the
     * quality of the error message, not the guard itself, but avoids
     * unrelated stderr noise from the degraded-fallback path). */
    char out[256];
    int init_rc = urbi_repl_eval(&vm, NULL, "1", 1, out, sizeof out);
    UASSERT_EQ(URBI_OK, init_rc);

    /* Inject INFINITY into a realm global (lexer would reject 1e309). */
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UValue inf_val;
    urbi_zero(&inf_val, sizeof inf_val);
    inf_val.kind = (uint8_t)UVAL_FLOAT;
    inf_val.v.f  = (double)INFINITY;
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, realm, "inf_val", 7, inf_val));

    /* Pre-condition: no periodic installed. */
    UASSERT(vm.periodics_head == NULL);

    /* every(INFINITY) { 1 } must NOT install a periodic.
     * Pre-fix: INFINITY passes `f < 0.0 || f != f` guard → UB cast → periodic installed.
     * Post-fix: INFINITY > 9.2e12 → guard triggers → type error, no install. */
    (void)urbi_repl_eval(&vm, NULL,
        "every(Realm.inf_val) { 1 }", 27, out, sizeof out);
    UASSERT(vm.periodics_head == NULL);   /* red pre-fix: non-NULL */

    /* every(9.3e13) { 1 } — large-finite overflow case.
     * 9.3e13 µs would overflow int64 on conversion (9.3e13 * 1e6 > 2^64);
     * the new guard catches f > 9.2e12 before the cast.
     * Pre-fix: 9.3e13 passes the old guard (positive, not NaN) → UB.
     * Post-fix: 9.3e13 > 9.2e12 → rejected, no periodic installed. */
    (void)urbi_repl_eval(&vm, NULL,
        "every(9.3e13) { 1 }", 20, out, sizeof out);
    UASSERT(vm.periodics_head == NULL);   /* red pre-fix: non-NULL */

    /* sleep(INFINITY) must NOT park a strand on the sleep queue.
     * Pre-fix: INFINITY passes old guard → (uint64_t)(int64_t)(INFINITY * 1e6) UB
     *          → strand parks with garbage duration → wakeup_pending_count = 1.
     * Post-fix: guard triggers → type error → strand exits → count stays 0. */
    UASSERT_EQ(0U, vm.wakeup_pending_count);
    (void)urbi_repl_eval(&vm, NULL,
        "sleep(Realm.inf_val)", 20, out, sizeof out);
    UASSERT_EQ(0U, vm.wakeup_pending_count);   /* red pre-fix: 1 */

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Test 4: overrunning periodic still lets the VM quiesce between fires
 * =========================================================================
 *
 * ANTI-HANG regression for the re-arm slide target.  An every(P) whose body
 * consistently overruns its period (here: each fire advances the mock clock
 * by P + 1, so body-duration > period) must NOT spin urbi_step in perpetual
 * RUNNING: the deadline resumes at now + period, so after a body fire the
 * post-dispatch periodic pump (which re-reads the clock) finds the deadline
 * in the FUTURE and the VM reaches WAKE_AT/QUIESCENT — exactly one fire per
 * deadline crossing, no catch-up burst.
 *
 * If the re-arm slide target is reverted to literal `now`, the just-slid
 * deadline equals the completion clock and the pump re-fires the body within
 * the same urbi_step forever; urbi_step then only ever returns RUNNING, the
 * bounded inner loop below caps out, and the WAKE_AT/QUIESCENT + one-fire
 * assertions FAIL.  The loop is bounded by step_calls so a revert FAILS the
 * test rather than hanging the suite. */
UTEST(every_overrun_resumes_and_quiesces)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    vm.host_time_us = cadence_mock_clock;
    vm.host_time_ud = NULL;
    g_cadence_now_us = 0;
    g_overrun_fires  = 0;
    g_overrun_bump_us = 101U;   /* > period (100us): every fire overruns */

    /* Register the body's tick() — advances the clock past the deadline each
     * fire and counts.  Bare global, resolvable from an every() body (same as
     * the inner every() in nested.chk). */
    UASSERT_EQ(URBI_OK, urbi_register(&vm, NULL, "tick", overrun_tick));

    /* Install every(100us) { tick() }.  next_fire_us = now(0) + 100 = 100. */
    int rc = utest_e2e_compile_and_run(&vm, "every(100us) { tick() }", NULL);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT(vm.periodics_head != NULL);
    UASSERT_EQ(0ULL, (unsigned long long)g_overrun_fires);  /* not due at t=0 */

    /* Drive several deadline crossings.  Each round advances the clock to the
     * current deadline, then steps; the periodic must fire once and the VM
     * must quiesce. */
    const int ROUNDS = 4;
    int round;
    for (round = 1; round <= ROUNDS; round++) {
        uint64_t prev_fires = g_overrun_fires;

        UASSERT(vm.periodics_head != NULL);
        g_cadence_now_us = vm.periodics_head->next_fire_us;  /* fire is now due */

        UStepResult res  = URBI_STEP_RUNNING;
        uint64_t    wake = 0U;
        int         step_calls = 0;
        /* Bounded: terminates regardless of correctness.  Correct now+period
         * quiesces in a single call (one tiny tick() body << 4096 opcodes);
         * literal `now` re-fires the body every pump pass so urbi_step only
         * ever returns RUNNING, the cap (8) breaks the loop, and the
         * assertions below — not a hang — report the regression.  The small
         * budget keeps a reverted build's spin to ~8*4096 fires/round so it
         * FAILS in well under a second rather than grinding. */
        const int STEP_CAP = 8;
        while (res == URBI_STEP_RUNNING && step_calls < STEP_CAP) {
            res = urbi_step(&vm, 4096ULL, &wake);
            step_calls++;
        }

        /* Anti-hang: reached a quiescent verdict, NOT perpetual RUNNING. */
        UASSERT(res == URBI_STEP_WAKE_AT || res == URBI_STEP_QUIESCENT);
        /* Exactly one fire per deadline crossing — no catch-up burst. */
        UASSERT_EQ(1ULL, (unsigned long long)(g_overrun_fires - prev_fires));
    }

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point
 * ========================================================================= */

void test_periodic_cadence_suite(void)
{
    utest_run("periodic_cadence: every(0.05) → period_us = 50000 (SCHED-14 float=seconds)",
              every_float_arg_is_seconds);
    utest_run("periodic_cadence: fixed cadence + slide-to-now (SCHED-14 legacy every|)",
              every_cadence_is_fixed_with_slide);
    utest_run("periodic_cadence: overrun resumes at now+period and quiesces (SCHED-14 anti-hang)",
              every_overrun_resumes_and_quiesces);
    utest_run("periodic_cadence: every/sleep reject INFINITY and overflow (SCHED-14 guards)",
              every_and_sleep_reject_nonfinite);
}
