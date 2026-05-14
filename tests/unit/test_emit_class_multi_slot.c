/* SPDX-License-Identifier: BSD-3-Clause */
/* test_emit_class_multi_slot.c — Phase 3 (Gap #2): multi-slot class body.
 *
 * Verifies that `class C { var x = 1; var y = 2; }` emits cleanly via
 * AST_BIN_SEP / AST_NARY recursion in emit_class_body_stmt.
 *
 * Scope note: test cases intentionally avoid Gap #1 (closure upvalue
 * capture) and Gap #3 (`this` keyword), which are not yet landed in
 * Wave 3.  Function literals in class bodies are restricted to
 * non-upvalue, non-this-using bodies (e.g. `function() { 42 }`). */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include <stddef.h>
#include <string.h>

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "object/uobject.h"
#include "value/uintern.h"

#define UTEST(name) static void name(void)

/* Helper: read a realm global by name. */
static UValue realm_get(UVM *vm, const char *name)
{
    URealm *realm = urbi_realm_global(vm);
    UValue out = urbi_make_nil();
    (void)urbi_realm_get_global(vm, realm, name, (int)strlen(name), &out);
    return out;
}

/* Helper: lookup a slot on an object by name (proto-walk). */
static UValue obj_lookup(UVM *vm, UObject *obj, const char *name)
{
    USymbol *sym = (USymbol *)ustr_intern(vm, name, strlen(name));
    UValue out = urbi_make_nil();
    (void)urbi_object_lookup(vm, obj, sym, &out);
    return out;
}

/* ===================================================================
 * Test 1: two var decls in a class body
 * =================================================================== */

UTEST(emit_class_two_var_decls)
{
    /* class C { var x = 1; var y = 2 } — should emit
     * C.x = 1; C.y = 2 as sibling sites. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    int rc = utest_e2e_compile_and_run(&vm,
        "class C { var x = 1; var y = 2; };",
        NULL);
    UASSERT_EQ(URBI_OK, rc);
    if (rc != URBI_OK) { urbi_vm_destroy(&vm); return; }

    UValue cv = realm_get(&vm, "C");
    UASSERT_EQ((int)UVAL_OBJECT, (int)cv.kind);
    if ((int)cv.kind != (int)UVAL_OBJECT) { urbi_vm_destroy(&vm); return; }

    UValue xv = obj_lookup(&vm, (UObject *)cv.v.p, "x");
    UASSERT_EQ((int)UVAL_INT, (int)xv.kind);
    UASSERT_EQ(1, (int)xv.v.i);

    UValue yv = obj_lookup(&vm, (UObject *)cv.v.p, "y");
    UASSERT_EQ((int)UVAL_INT, (int)yv.kind);
    UASSERT_EQ(2, (int)yv.v.i);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 2: three var decls in a class body
 * =================================================================== */

UTEST(emit_class_three_var_decls)
{
    /* class C { var x = 1; var y = 2; var z = 3 } */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    int rc = utest_e2e_compile_and_run(&vm,
        "class C3 { var x = 1; var y = 2; var z = 3; };",
        NULL);
    UASSERT_EQ(URBI_OK, rc);
    if (rc != URBI_OK) { urbi_vm_destroy(&vm); return; }

    UValue cv = realm_get(&vm, "C3");
    UASSERT_EQ((int)UVAL_OBJECT, (int)cv.kind);
    if ((int)cv.kind != (int)UVAL_OBJECT) { urbi_vm_destroy(&vm); return; }

    UValue xv = obj_lookup(&vm, (UObject *)cv.v.p, "x");
    UASSERT_EQ(1, (int)xv.v.i);

    UValue yv = obj_lookup(&vm, (UObject *)cv.v.p, "y");
    UASSERT_EQ(2, (int)yv.v.i);

    UValue zv = obj_lookup(&vm, (UObject *)cv.v.p, "z");
    UASSERT_EQ(3, (int)zv.v.i);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 3: mixed var + function + var sequence
 *
 * Exercises the freereg-discipline-between-siblings path.
 * Function literal uses no `this` or upvalues (Gap #1 / #3 not landed).
 * =================================================================== */

UTEST(emit_class_var_function_var)
{
    /* class C { var x = 1; var f = function(){42}; var y = 2; } */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    int rc = utest_e2e_compile_and_run(&vm,
        "class Cmix { var x = 1; var f = function() { return 42 }; var y = 2; };",
        NULL);
    UASSERT_EQ(URBI_OK, rc);
    if (rc != URBI_OK) { urbi_vm_destroy(&vm); return; }

    UValue cv = realm_get(&vm, "Cmix");
    UASSERT_EQ((int)UVAL_OBJECT, (int)cv.kind);
    if ((int)cv.kind != (int)UVAL_OBJECT) { urbi_vm_destroy(&vm); return; }

    UValue xv = obj_lookup(&vm, (UObject *)cv.v.p, "x");
    UASSERT_EQ(1, (int)xv.v.i);

    UValue yv = obj_lookup(&vm, (UObject *)cv.v.p, "y");
    UASSERT_EQ(2, (int)yv.v.i);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 4: new() sees all slots from a multi-slot class
 * =================================================================== */

UTEST(emit_class_new_sees_multi_slots)
{
    /* class Point { var x = 1; var y = 2 }; var p = Point.new();
     * p.x and p.y must be inherited (COW). */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    int rc = utest_e2e_compile_and_run(&vm,
        "class Point { var x = 1; var y = 2; };"
        "var pt = Point.new();",
        NULL);
    UASSERT_EQ(URBI_OK, rc);
    if (rc != URBI_OK) { urbi_vm_destroy(&vm); return; }

    UValue ptv = realm_get(&vm, "pt");
    UASSERT_EQ((int)UVAL_OBJECT, (int)ptv.kind);
    if ((int)ptv.kind != (int)UVAL_OBJECT) { urbi_vm_destroy(&vm); return; }

    UValue xv = obj_lookup(&vm, (UObject *)ptv.v.p, "x");
    UASSERT_EQ(1, (int)xv.v.i);

    UValue yv = obj_lookup(&vm, (UObject *)ptv.v.p, "y");
    UASSERT_EQ(2, (int)yv.v.i);

    urbi_vm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void
test_emit_class_multi_slot_suite(void)
{
    printf("test_emit_class_multi_slot\n");
    utest_run("emit_class_two_var_decls",       emit_class_two_var_decls);
    utest_run("emit_class_three_var_decls",     emit_class_three_var_decls);
    utest_run("emit_class_var_function_var",    emit_class_var_function_var);
    utest_run("emit_class_new_sees_multi_slots",emit_class_new_sees_multi_slots);
}
