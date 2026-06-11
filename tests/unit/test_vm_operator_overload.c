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
#include "urbi/gc.h"          /* urbi_gc_force_full (GC-06 UAF-shape case) */
#include "vm/uvm.h"
#include "object/uobject.h"   /* urbi_object_set_local_slot (GC-06 staleness) */
#include "value/uintern.h"    /* ustr_intern */

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

/* ===================================================================
 * refactor-3 GC-06/VM-06c: the IC caches (holder, slot_idx), never the
 * closure VALUE — in-place overwrite of an operator slot (which
 * deliberately does NOT bump topology_gen, topology spec §4.2 row 2)
 * must dispatch the NEW closure at an already-filled call site.
 *
 * All three tests route the operator through a function so the SAME
 * compiled call site (same pc_offset inside f's proto) runs before and
 * after the overwrite.  Globals are pre-declared and written by plain
 * assignment so no realm slot-add lands between IC fill and re-call
 * (a slot add on a prototype bumps topology_gen and would mask the
 * staleness by forcing a re-resolve).
 * =================================================================== */

UTEST(vm_op_overload_ic_stale_after_inplace_redefine)
{
    /* Script-level staleness: setSlot overwrite of an existing "+" slot
     * is an in-place value write (no gen bump); the second f(c) at the
     * same site must dispatch the new closure.  Pre-GC-06 this returned
     * 11 (stale closure cached by value); expected is 12. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "class VecRedef { var v = 0 };\n"
        "VecRedef.setSlot(\"+\", function(other) { 1 });\n"
        "var c = VecRedef.new();\n"
        "var f = function(x) { x + x };\n"
        "var r1 = 0;\n"
        "var r2 = 0;\n"
        "r1 = f(c);\n"
        "VecRedef.setSlot(\"+\", function(other) { 2 });\n"
        "r2 = f(c);\n"
        "r1 * 10 + r2",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(12, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

UTEST(vm_op_overload_ic_polymorphic_site_dispatches_per_receiver)
{
    /* Polymorphic-site wrong-dispatch: the IC entry keyed only on
     * (pc_offset, op_name, topology_gen) served the FIRST receiver's
     * operator to every later receiver at the same site.  Every receiver
     * is created before the first call (Class.new() bumps topology_gen
     * and would otherwise mask the bug by invalidating the entry), and
     * results are pre-declared + assigned (no realm slot-add between fill
     * and re-call).  Pre-fix: 1,1,1,1 -> 1111.  Expected:
     *   f(c)=1 (fills C entry), f(d)=2 (different class, same site),
     *   f(i)=9 (instance-local shadow), f(c)=1 (C entry still hits)
     * -> 1*1000 + 2*100 + 9*10 + 1 = 1291. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "class PolyC {};\n"
        "PolyC.setSlot(\"+\", function(other) { 1 });\n"
        "class PolyD {};\n"
        "PolyD.setSlot(\"+\", function(other) { 2 });\n"
        "var c = PolyC.new();\n"
        "var d = PolyD.new();\n"
        "var i = PolyC.new();\n"
        "i.setSlot(\"+\", function(other) { 9 });\n"
        "var f = function(x) { x + x };\n"
        "var r1 = 0;\n"
        "var r2 = 0;\n"
        "var r3 = 0;\n"
        "var r4 = 0;\n"
        "r1 = f(c);\n"
        "r2 = f(d);\n"
        "r3 = f(i);\n"
        "r4 = f(c);\n"
        "r1 * 1000 + r2 * 100 + r3 * 10 + r4",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(1291, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

UTEST(vm_op_overload_ic_stale_after_c_api_set_local_slot)
{
    /* C-API staleness: overwrite the holder's "+" slot directly via
     * urbi_object_set_local_slot (the canonical in-place path — asserts
     * pin that topology_gen does NOT change, so a pre-GC-06 IC entry
     * stays "valid" and would keep serving the old closure). */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    struct URealm *gr = urbi_realm_global(&vm);
    UASSERT(gr != NULL);
    if (gr == NULL) { urbi_vm_destroy(&vm); return; }

    /* Eval 1 (caller-owned arena+module so fc/g protos outlive the call):
     * define the class + original "+", the call-site function fc, and the
     * replacement closure g; fill the IC via fc(c1). */
    UArena  arena1;
    UProto  module1 = {0};
    uarena_init(&arena1, 4096);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena1, &module1,
        "class VecCapi { var v = 0 };\n"
        "VecCapi.setSlot(\"+\", function(other) { 1 });\n"
        "var c1 = VecCapi.new();\n"
        "var fc = function(x) { x + x };\n"
        "var g = function(other) { 2 };\n"
        "fc(c1)",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(1, (int)out.v.i);

    /* In-place overwrite from C: holder is the class object itself. */
    USymbol *plus = (USymbol *)ustr_intern(&vm, "+", 1);
    UASSERT(plus != NULL);
    UValue holder_val = urbi_make_nil();
    UValue g_val      = urbi_make_nil();
    rc = urbi_realm_get_global(&vm, gr, "VecCapi", 7, &holder_val);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_OBJECT, (int)holder_val.kind);
    rc = urbi_realm_get_global(&vm, gr, "g", 1, &g_val);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_CLOSURE, (int)g_val.kind);

    uint64_t gen_before = vm.topology_gen;
    rc = urbi_object_set_local_slot(&vm, (UObject *)holder_val.v.p,
                                    plus, g_val);
    UASSERT_EQ(0, rc);
    /* Pin the precondition: in-place value write — no topology bump.
     * If this ever starts bumping, the staleness scenario is vacuous. */
    UASSERT(vm.topology_gen == gen_before);

    /* Eval 2: same compiled call site (inside fc's proto) must now
     * dispatch g.  Pre-GC-06: stale 1. */
    UArena  arena2;
    UProto  module2 = {0};
    uarena_init(&arena2, 4096);
    rc = utest_e2e_compile_and_run_with_module(&vm, &arena2, &module2,
        "fc(c1)", &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(2, (int)out.v.i);

    uchunk_destroy(&module2, NULL);
    uarena_destroy(&arena2);
    uchunk_destroy(&module1, NULL);
    uarena_destroy(&arena1);
    urbi_vm_destroy(&vm);
}

UTEST(vm_op_overload_ic_no_uaf_after_gc_sweeps_old_closure)
{
    /* UAF shape: overwrite the slot, force a full GC so the OLD closure
     * cell is swept, then re-run the filled call site.  Pre-GC-06 the IC
     * returned the freed closure (dangling dispatch — ASan UAF); the
     * (holder, slot_idx) re-read picks up the live replacement. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UArena  arena1;
    UProto  module1 = {0};
    uarena_init(&arena1, 4096);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena1, &module1,
        "class VecUaf { var v = 0 };\n"
        "VecUaf.setSlot(\"+\", function(other) { 1 });\n"
        "var cu = VecUaf.new();\n"
        "var fu = function(x) { x + x };\n"
        "fu(cu)",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(1, (int)out.v.i);

    /* Eval 2: in-place overwrite from script; the original "+" closure
     * is now unreferenced (the slot was its only reference). */
    UArena  arena2;
    UProto  module2 = {0};
    uarena_init(&arena2, 4096);
    rc = utest_e2e_compile_and_run_with_module(&vm, &arena2, &module2,
        "VecUaf.setSlot(\"+\", function(other) { 2 }); 0", &out);
    UASSERT_EQ(URBI_OK, rc);

    /* Sweep the old closure. */
    urbi_gc_force_full(&vm);

    /* Eval 3: the same call site must dispatch the live replacement. */
    UArena  arena3;
    UProto  module3 = {0};
    uarena_init(&arena3, 4096);
    rc = utest_e2e_compile_and_run_with_module(&vm, &arena3, &module3,
        "fu(cu)", &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(2, (int)out.v.i);

    uchunk_destroy(&module3, NULL);
    uarena_destroy(&arena3);
    uchunk_destroy(&module2, NULL);
    uarena_destroy(&arena2);
    uchunk_destroy(&module1, NULL);
    uarena_destroy(&arena1);
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
    utest_run("vm_op_overload_ic_stale_after_inplace_redefine",
                                                      vm_op_overload_ic_stale_after_inplace_redefine);
    utest_run("vm_op_overload_ic_polymorphic_site_dispatches_per_receiver",
                                                      vm_op_overload_ic_polymorphic_site_dispatches_per_receiver);
    utest_run("vm_op_overload_ic_stale_after_c_api_set_local_slot",
                                                      vm_op_overload_ic_stale_after_c_api_set_local_slot);
    utest_run("vm_op_overload_ic_no_uaf_after_gc_sweeps_old_closure",
                                                      vm_op_overload_ic_no_uaf_after_gc_sweeps_old_closure);
}
