/* SPDX-License-Identifier: BSD-3-Clause */
/* test_vm_operator_overload.c — Phase 4 (Gap #4): operator overload via
 * type-error fallback dispatch on 9 ops (+ - * / unary- == != < <=).
 *
 * Scope notes:
 *   - Gap #1 (closure upvalue capture) has not landed.
 *   - Gap #3 (`this` keyword) has not landed.
 * Test cases therefore cannot access the receiver from inside a method body
 * via `this`.  We use function literals that return constants (e.g. 99) or
 * take the rhs as an explicit argument, to validate that:
 *   (a) the atom fast path runs first (no regression for int/float ops),
 *   (b) on type-error, slot lookup on the lhs proto chain occurs,
 *   (c) the slot is called as a method,
 *   (d) the return value is captured correctly.
 * Full receiver-access patterns will be validated once Gap #1 + #3 land. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include <stddef.h>
#include <string.h>

#include "urbi/urbi.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* ===================================================================
 * Atom fast-path regressions (must still work after Gap #4 lands)
 * =================================================================== */

UTEST(vm_op_add_atom_fast_path_unaffected)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm, "1 + 2", &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(3, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

UTEST(vm_op_sub_atom_fast_path_unaffected)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm, "5 - 3", &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(2, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

UTEST(vm_op_mul_atom_fast_path_unaffected)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm, "3 * 4", &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(12, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

UTEST(vm_op_div_atom_fast_path_unaffected)
{
    /* Int/Int division promotes to Float: 6/3 = 2.0 */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm, "6 / 3", &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_FLOAT, (int)out.kind);
    UASSERT(out.v.f > 1.99 && out.v.f < 2.01);
    urbi_vm_destroy(&vm);
}

UTEST(vm_op_neg_atom_fast_path_unaffected)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm, "-7", &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(-7, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * User-type overload tests: slot installed on class, invoked via operator
 *
 * Pattern: class Vec { var v = 0 };
 *          Vec.setSlot("+", function(other) { 99 });
 *          var a = Vec.new(); var b = Vec.new();
 *          a + b   --> 99
 *
 * The function body returns a constant (99) to avoid needing `this`.
 * This validates the dispatch path: type-error -> slot lookup -> call.
 * =================================================================== */

UTEST(vm_op_add_user_type_overload)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "class VecAdd { var v = 0 };\n"
        "VecAdd.setSlot(\"+\", function(other) { 99 });\n"
        "var a = VecAdd.new();\n"
        "var b = VecAdd.new();\n"
        "a + b",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(99, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

UTEST(vm_op_sub_user_type_overload)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "class VecSub { var v = 0 };\n"
        "VecSub.setSlot(\"-\", function(other) { 77 });\n"
        "var a = VecSub.new();\n"
        "var b = VecSub.new();\n"
        "a - b",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(77, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

UTEST(vm_op_mul_user_type_overload)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "class VecMul { var v = 0 };\n"
        "VecMul.setSlot(\"*\", function(other) { 55 });\n"
        "var a = VecMul.new();\n"
        "var b = VecMul.new();\n"
        "a * b",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(55, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

UTEST(vm_op_div_user_type_overload)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "class VecDiv { var v = 0 };\n"
        "VecDiv.setSlot(\"/\", function(other) { 33 });\n"
        "var a = VecDiv.new();\n"
        "var b = VecDiv.new();\n"
        "a / b",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(33, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

UTEST(vm_op_neg_user_type_overload)
{
    /* Unary negation: "-" slot with no args (arity-0 method). */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "class VecNeg { var v = 0 };\n"
        "VecNeg.setSlot(\"-\", function() { 11 });\n"
        "var a = VecNeg.new();\n"
        "-a",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(11, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

UTEST(vm_op_eq_user_type_overload)
{
    /* OP_EQ: overload is triggered when lhs is a user object.
     * The "==" slot returns a truthy int, so the conditional
     * checks that the overloaded value is used. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "class VecEq { var v = 0 };\n"
        "VecEq.setSlot(\"==\", function(other) { 1 });\n"
        "var a = VecEq.new();\n"
        "var b = VecEq.new();\n"
        "if (a == b) { 42 } else { 0 }",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(42, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

UTEST(vm_op_neq_user_type_overload)
{
    /* OP_NEQ: overload is triggered when lhs is a user object.
     * The "!=" slot returns 1 (not-equal), so the if-branch fires. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "class VecNeq { var v = 0 };\n"
        "VecNeq.setSlot(\"!=\", function(other) { 1 });\n"
        "var a = VecNeq.new();\n"
        "var b = VecNeq.new();\n"
        "if (a != b) { 88 } else { 0 }",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(88, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

UTEST(vm_op_lt_user_type_overload)
{
    /* OP_LT: overload fires on user object lhs. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "class VecLt { var v = 0 };\n"
        "VecLt.setSlot(\"<\", function(other) { 1 });\n"
        "var a = VecLt.new();\n"
        "var b = VecLt.new();\n"
        "if (a < b) { 66 } else { 0 }",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(66, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

UTEST(vm_op_le_user_type_overload)
{
    /* OP_LE: overload fires on user object lhs. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "class VecLe { var v = 0 };\n"
        "VecLe.setSlot(\"<=\", function(other) { 1 });\n"
        "var a = VecLe.new();\n"
        "var b = VecLe.new();\n"
        "if (a <= b) { 44 } else { 0 }",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(44, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Missing-slot falls through to type-error (not silently succeeds)
 * =================================================================== */

UTEST(vm_op_overload_missing_falls_to_type_error)
{
    /* No "+" slot on the object: original type-error must be preserved. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "var bare = Object.clone();\n"
        "bare + 1",
        &out);
    UASSERT(rc != URBI_OK);
    urbi_vm_destroy(&vm);
}

UTEST(vm_op_lt_missing_falls_to_type_error)
{
    /* OP_LT with no slot: type-error must propagate. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "var bare2 = Object.clone();\n"
        "bare2 < 1",
        &out);
    UASSERT(rc != URBI_OK);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * IC repeat: calling the same operator twice uses the cached method
 * =================================================================== */

UTEST(vm_op_add_overload_ic_cached)
{
    /* Two consecutive a + b calls: second hit should use IC cache. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "class VecIc { var v = 0 };\n"
        "VecIc.setSlot(\"+\", function(other) { 7 });\n"
        "var a = VecIc.new();\n"
        "var b = VecIc.new();\n"
        "var r1 = a + b;\n"
        "var r2 = a + b;\n"
        "r1 + r2",  /* 7 + 7 = 14 via atom fast path */
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(14, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void
test_vm_operator_overload_suite(void)
{
    printf("test_vm_operator_overload\n");
    utest_run("vm_op_add_atom_fast_path_unaffected",  vm_op_add_atom_fast_path_unaffected);
    utest_run("vm_op_sub_atom_fast_path_unaffected",  vm_op_sub_atom_fast_path_unaffected);
    utest_run("vm_op_mul_atom_fast_path_unaffected",  vm_op_mul_atom_fast_path_unaffected);
    utest_run("vm_op_div_atom_fast_path_unaffected",  vm_op_div_atom_fast_path_unaffected);
    utest_run("vm_op_neg_atom_fast_path_unaffected",  vm_op_neg_atom_fast_path_unaffected);
    utest_run("vm_op_add_user_type_overload",         vm_op_add_user_type_overload);
    utest_run("vm_op_sub_user_type_overload",         vm_op_sub_user_type_overload);
    utest_run("vm_op_mul_user_type_overload",         vm_op_mul_user_type_overload);
    utest_run("vm_op_div_user_type_overload",         vm_op_div_user_type_overload);
    utest_run("vm_op_neg_user_type_overload",         vm_op_neg_user_type_overload);
    utest_run("vm_op_eq_user_type_overload",          vm_op_eq_user_type_overload);
    utest_run("vm_op_neq_user_type_overload",         vm_op_neq_user_type_overload);
    utest_run("vm_op_lt_user_type_overload",          vm_op_lt_user_type_overload);
    utest_run("vm_op_le_user_type_overload",          vm_op_le_user_type_overload);
    utest_run("vm_op_overload_missing_falls_to_type_error",
                                                      vm_op_overload_missing_falls_to_type_error);
    utest_run("vm_op_lt_missing_falls_to_type_error", vm_op_lt_missing_falls_to_type_error);
    utest_run("vm_op_add_overload_ic_cached",         vm_op_add_overload_ic_cached);
}
