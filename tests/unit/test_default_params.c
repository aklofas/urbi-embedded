/* SPDX-License-Identifier: BSD-3-Clause */
/* test_default_params — default parameter values (v0.13.5).
 *
 * function (a, b = expr) { ... }: trailing parameters may carry `= expr`
 * defaults, evaluated AT CALL TIME in the callee's scope when the caller
 * omits them.  Reference semantics: aldebaran ugrammar.y `formal:
 * var.opt "identifier" "=" exp` + factory.cc formals_to_decs (in-order
 * LocalDeclarations — defaults may read earlier params; a missing
 * non-defaulted formal raises regardless of defaults on earlier params).
 *
 * Mechanism under test (arity self-check discipline, wire header flag
 * bit 0): the emitter plants a min-arity prologue + default fills in
 * every >=1-param function; OP_CALL relaxes its arity check to
 * `nargs <= nparams` for flagged protos and seeds the actual count into
 * the synthetic \x01nargs local at R[nparams].  The below-min throw is
 * catchable; above-max stays the VM's typed TypeError. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "chunk/uchunk.h"
#include "value/uarena.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Run src in a fresh VM; assert the chunk result equals `expect` (int). */
static void
expect_int_result(const char *src, int64_t expect)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    UValue out = utest_e2e_make_nil();
    int rc = utest_e2e_compile_and_run(&vm, src, &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(expect, out.v.i);
    urbi_vm_destroy(&vm);
}

UTEST(default_used_when_omitted)
{
    expect_int_result("var f = function (a, b = 5) { a + b }; f(1)", 6);
}

UTEST(provided_arg_wins_over_default)
{
    expect_int_result("var f = function (a, b = 5) { a + b }; f(1, 2)", 3);
}

UTEST(all_defaulted_zero_arg_call)
{
    expect_int_result("var g = function (x = 1 + 1) { x }; g()", 2);
}

UTEST(multiple_trailing_defaults)
{
    expect_int_result(
        "var f = function (a, b = 10, c = 100) { a + b + c };"
        "f(1) + f(1, 2) + f(1, 2, 3)", 111 + 103 + 6);
}

UTEST(default_reads_earlier_param)
{
    /* Call-time, in-order evaluation in the callee scope (legacy
     * formals_to_decs semantics): b's default sees a already bound. */
    expect_int_result("var h = function (a, b = a + 1) { a + b }; h(10)", 21);
}

UTEST(default_evaluated_at_call_time)
{
    /* The default expression re-evaluates per call — not captured at
     * definition time. */
    expect_int_result(
        "var base = 100;"
        "var k = function (x = base) { x };"
        "base = 200;"
        "k()", 200);
}

UTEST(explicit_nil_is_provided_not_omitted)
{
    /* Passing nil occupies the parameter slot; the default must NOT fire
     * (count-based detection, not value-based). */
    expect_int_result(
        "var n = function (a, b = 5) { if (b == nil) { 1 } else { 0 } };"
        "n(1, nil)", 1);
}

UTEST(below_min_arity_throws_catchable)
{
    expect_int_result(
        "var f = function (a, b = 5) { a + b };"
        "var c = 0; try { f() } catch (e) { c = 1 }; c", 1);
}

UTEST(below_min_message_carries_expected_count)
{
    /* The prologue throw carries a static expected-count.  Pinned from
     * actual runner output; T10 (typed exceptions) retargets the throw
     * value to an ArityError instance later this wave. */
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    UValue out = utest_e2e_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "var f = function (a, b = 5) { a + b };"
        "var m = \"\"; try { f() } catch (e) { m = e }; m", &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_STR, (int)out.kind);
    UASSERT(out.v.p != NULL);
    UASSERT_EQ(0, strcmp((const char *)out.v.p,
                         "function call: wrong argument count (expected at least 1)"));
    urbi_vm_destroy(&vm);
}

UTEST(above_max_arity_throws_catchable)
{
    /* Too-many stays the VM-side typed TypeError (serialized nparams). */
    expect_int_result(
        "var f = function (a, b = 5) { a + b };"
        "var c = 0; try { f(1, 2, 3) } catch (e) { c = 1 }; c", 1);
}

UTEST(exact_arity_function_still_enforced_both_sides)
{
    expect_int_result(
        "var f = function (a, b) { a + b };"
        "var c = 0;"
        "try { f(1) } catch (e) { c = c + 1 };"
        "try { f(1, 2, 3) } catch (e) { c = c + 10 };"
        "c + f(1, 2)", 14);
}

UTEST(non_trailing_default_parses_but_is_dead)
{
    /* Legacy gap rule (ugrammar.y :1533 has no ordering constraint):
     * `function (a = 1, b)` parses; min arity = last non-defaulted index
     * + 1 = 2, so the default on `a` can never fire — calling with one
     * argument raises just as the legacy in-order LocalDeclaration
     * binding would. */
    expect_int_result(
        "var f = function (a = 1, b) { a + b };"
        "var c = 0; try { f(7) } catch (e) { c = 1 };"
        "c + f(7, 2)", 10);
}

UTEST(zero_param_function_unchanged)
{
    expect_int_result("var z = function () { 42 }; z()", 42);
}

UTEST(default_survives_serialize_roundtrip)
{
    /* Compile → serialize → deserialize → run: header flag bit 0 must
     * propagate through the wire so the relaxed OP_CALL check + prologue
     * seed still apply to the reloaded chunk. */
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    const char *src = "Realm.out = 0;"
                      "var f = function (a, b = 5) { a + b };"
                      "Realm.out = f(1)";
    unsigned char *buf = NULL;
    size_t         len = 0;
    char           err[256];
    UASSERT_EQ(URBI_OK, urbi_compile_source(&vm, src, strlen(src), "t",
                                            &buf, &len, err, sizeof err));
    UASSERT(buf != NULL);
    UASSERT(len > 24U);

    /* v0.13.5: emitter-produced chunk carries header flag bit 0. */
    UASSERT_EQ((uint8_t)0x01, buf[5]);

    UProto *reloaded = urbi_chunk_from_bytes(&vm, buf, len, err, sizeof err);
    UASSERT(reloaded != NULL);
    UASSERT_EQ((uint8_t)1, reloaded->arity_prologue);

    UValue out = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_run_chunk(&vm, r, reloaded, &out));
    UValue got = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "out", 3, &got));
    UASSERT_EQ((int)UVAL_INT, (int)got.kind);
    UASSERT_EQ(6LL, got.v.i);

    free(buf);
    /* The chunk's closures (Realm-global f) hold root-proto refs until VM
     * teardown; uchunk_destroy's rescue path parks the root on
     * vm->rescued_protos and urbi_vm_destroy frees it (the urbi_chunk_free
     * fast path asserts refcount == 0 and is wrong here). */
    uchunk_destroy(reloaded, &vm);
    urbi_vm_destroy(&vm);
}

UTEST(lazy_param_default_rejected_at_parse)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    UValue out = utest_e2e_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "var f = function (lazy x = 1) { 0 }; 0", &out);
    UASSERT(rc != URBI_OK);
    urbi_vm_destroy(&vm);
}

void
test_default_params_suite(void)
{
    utest_run("default_params: omitted trailing arg uses default",
              default_used_when_omitted);
    utest_run("default_params: provided arg wins over default",
              provided_arg_wins_over_default);
    utest_run("default_params: all-defaulted callable with zero args",
              all_defaulted_zero_arg_call);
    utest_run("default_params: multiple trailing defaults fill in order",
              multiple_trailing_defaults);
    utest_run("default_params: default reads earlier param (call-time order)",
              default_reads_earlier_param);
    utest_run("default_params: default evaluated at call time",
              default_evaluated_at_call_time);
    utest_run("default_params: explicit nil is provided, not omitted",
              explicit_nil_is_provided_not_omitted);
    utest_run("default_params: below min arity throws catchable",
              below_min_arity_throws_catchable);
    utest_run("default_params: below-min message carries expected count",
              below_min_message_carries_expected_count);
    utest_run("default_params: above max arity throws catchable",
              above_max_arity_throws_catchable);
    utest_run("default_params: exact-arity function enforced both sides",
              exact_arity_function_still_enforced_both_sides);
    utest_run("default_params: non-trailing default parses but is dead (legacy)",
              non_trailing_default_parses_but_is_dead);
    utest_run("default_params: zero-param function unchanged",
              zero_param_function_unchanged);
    utest_run("default_params: serialize round-trip preserves flag + fills",
              default_survives_serialize_roundtrip);
    utest_run("default_params: lazy param default rejected at parse",
              lazy_param_default_rejected_at_parse);
}
