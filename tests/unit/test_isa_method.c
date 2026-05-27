/* SPDX-License-Identifier: BSD-3-Clause */
/* test_isa_method.c — v0.10.11 / isA unit tests.
 *
 * Four cases:
 *   1. isA returns true when receiver's proto matches.
 *   2. isA returns false for unrelated proto.
 *   3. isA on atom receiver (Integer) returns true.
 *   4. isA with wrong arity surfaces an error. */

#include "utest.h"
#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "utest_e2e_helpers.h"

#include <stdint.h>

#define UTEST(name) static void name(void)

/* === Test 1: isA(self_proto) returns true ============================== */
UTEST(isa_self_match_true)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue out;
    int rc = utest_e2e_compile_and_run(&vm,
        "class P {}; var p = P.new(); p.isA(P)", &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_BOOL);
    UASSERT_EQ((int)out.v.i, 1);  /* true */

    urbi_vm_destroy(&vm);
}

/* === Test 2: isA(unrelated) returns false ============================== */
UTEST(isa_unrelated_false)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue out;
    int rc = utest_e2e_compile_and_run(&vm,
        "class A {}; class B {}; A.new().isA(B)", &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_BOOL);
    UASSERT_EQ((int)out.v.i, 0);  /* false */

    urbi_vm_destroy(&vm);
}

/* === Test 3: isA on Integer atom receiver returns true ================= */
UTEST(isa_atom_integer_true)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue out;
    int rc = utest_e2e_compile_and_run(&vm, "1.isA(Integer)", &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_BOOL);
    UASSERT_EQ((int)out.v.i, 1);  /* true */

    urbi_vm_destroy(&vm);
}

/* === Test 4: arity mismatch surfaces a TypeError ======================= */
UTEST(isa_arity_error)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue out;
    int rc = utest_e2e_compile_and_run(&vm, "1.isA()", &out);
    /* urbi_raise_arity transitions the strand to fatal state; run_chunk
     * surfaces it as URBI_ERR_STRAND_FATAL (not URBI_OK).  The REPL-
     * swallows pattern from v0.10.9 applies only to urbi_repl_eval. */
    UASSERT(rc != URBI_OK);

    urbi_vm_destroy(&vm);
}

/* ===== Suite ===== */
void test_isa_method_suite(void);

void
test_isa_method_suite(void)
{
    utest_run("isa_self_match_true",  isa_self_match_true);
    utest_run("isa_unrelated_false",  isa_unrelated_false);
    utest_run("isa_atom_integer_true", isa_atom_integer_true);
    utest_run("isa_arity_error",       isa_arity_error);
}
