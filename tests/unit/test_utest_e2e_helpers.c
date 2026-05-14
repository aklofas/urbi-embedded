/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_utest_e2e_helpers.c — self-test for the e2e helpers.
 *
 * The helpers themselves are exercised transitively by every consumer
 * test (test_at_scripted_e2e.c et al.), but a direct self-test catches
 * regressions in the helpers independently of consumer drift. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include <string.h>

#include "urbi/urbi.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* compile_and_run_simple: compile + run a trivial expression and confirm
 * the helper returns URBI_OK and produces an integer result.  Exercises
 * the owns-arena-and-module path. */
UTEST(compile_and_run_simple)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue result = {0};
    int rc = utest_e2e_compile_and_run(&vm, "42", &result);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)result.kind);
    UASSERT_EQ(42, (int)result.v.i);

    urbi_vm_destroy(&vm);
}

/* make_int_round_trip: small constructor produces the documented kind
 * and value. */
UTEST(make_int_round_trip)
{
    UValue v = utest_e2e_make_int(123);
    UASSERT_EQ((int)UVAL_INT, (int)v.kind);
    UASSERT_EQ(123, (int)v.v.i);
}

/* make_nil_matches_canonical: utest_e2e_make_nil must be byte-identical
 * to the public urbi_make_nil() — it is just a delegate. */
UTEST(make_nil_matches_canonical)
{
    UValue v = utest_e2e_make_nil();
    UASSERT_EQ((int)UVAL_NIL, (int)v.kind);
    UValue canonical = urbi_make_nil();
    UASSERT_EQ(0, memcmp(&v, &canonical, sizeof(UValue)));
}

/* run_to_no_runnable_zero_iters_when_no_strand: fresh VM has no runnable
 * strand, so the helper hits URBI_STEP_QUIESCENT (or strand_runnable_count
 * == 0) on the first iteration and returns 1 — *not* the timeout sentinel. */
UTEST(run_to_no_runnable_zero_iters_when_no_strand)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    int rc = utest_e2e_run_to_no_runnable(&vm);
    UASSERT_EQ(1, rc);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_utest_e2e_helpers_suite(void)
{
    printf("test_utest_e2e_helpers\n");
    utest_run("compile_and_run_simple",                         compile_and_run_simple);
    utest_run("make_int_round_trip",                            make_int_round_trip);
    utest_run("make_nil_matches_canonical",                     make_nil_matches_canonical);
    utest_run("run_to_no_runnable_zero_iters_when_no_strand",   run_to_no_runnable_zero_iters_when_no_strand);
}
