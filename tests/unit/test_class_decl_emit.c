/* SPDX-License-Identifier: BSD-3-Clause */
/* test_class_decl_emit.c — Phase 6: class-declaration emit (T38 desugar).
 *
 * Verifies that `class Foo { body }` desugars to:
 *   var Foo = Object.clone()
 *   <body emitted with Foo as receiver>
 *
 * and `class Foo : public A, B { body }` to:
 *   var Foo = Object.clone()
 *   Foo.protos.insertFront(B)        # protos in REVERSE order
 *   Foo.protos.insertFront(A)        # so chain ends in declaration order
 *   <body emitted with Foo as receiver>
 *
 * Runtime correctness is checked end-to-end via compile + run + slot
 * read-back through the realm-globals + slot APIs. */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "value/uintern.h"
#include "lex/ulex.h"
#include "module/umodule.h"
#include "parse/uparse.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "object/uobject.h"
#include "realm/urealm.h"

#define UTEST(name) static void name(void)

/* Helper: compile + run src under the VM's global realm.  Returns
 * URBI_OK on success.  On compile error, returns URBI_ERR_COMPILE; on
 * runtime error, returns the runtime error code. */
static int compile_and_run(UVM *vm, const char *src)
{
    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    uarena_init(&arena, 0);
    UModule module = {0};
    UEmitter e;
    uemit_init(&e, &module, &arena, vm, NULL);
    UParser p;
    uparse_init(&p, &lex, &arena);
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) {
            uarena_destroy(&arena);
            umodule_destroy(&module);
            return URBI_ERR_COMPILE;
        }
        if (uemit_statement(&e, node) != EMIT_OK) {
            uarena_destroy(&arena);
            umodule_destroy(&module);
            return URBI_ERR_COMPILE;
        }
        uarena_reset(&arena);
    }
    if (uemit_finish(&e) != EMIT_OK) {
        uarena_destroy(&arena);
        umodule_destroy(&module);
        return URBI_ERR_COMPILE;
    }
    UValue out = {0};
    int rc = urbi_run_chunk(vm, NULL, &module, &out);
    uarena_destroy(&arena);
    umodule_destroy(&module);
    return rc;
}

/* Helper: read a realm global by name; returns the UValue. */
static UValue realm_get(UVM *vm, const char *name)
{
    URealm *realm = urbi_realm_global(vm);
    UValue out = urbi_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(vm, realm, name, strlen(name), &out));
    return out;
}

/* Helper: lookup a slot on an object by name (proto-walk).  Returns 0 on
 * hit per urbi_object_lookup's contract (-1 on miss, 0 on hit). */
static UValue obj_lookup(UVM *vm, UObject *obj, const char *name)
{
    USymbol *sym = (USymbol *)ustr_intern(vm, name, strlen(name));
    UValue out = urbi_make_nil();
    int rc = urbi_object_lookup(vm, obj, sym, &out);
    UASSERT_EQ(0, rc);  /* 0 = hit, -1 = miss */
    return out;
}

/* === Task 69: basic class with no protos === */

UTEST(emit_class_decl_no_protos) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UASSERT_EQ(URBI_OK, compile_and_run(&vm,
        "class Foo { var x = 42 }"));

    UValue foo = realm_get(&vm, "Foo");
    UASSERT_EQ((int)UVAL_OBJECT, (int)foo.kind);
    UValue xv = obj_lookup(&vm, (UObject *)foo.v.p, "x");
    UASSERT_EQ((int)UVAL_INT, (int)xv.kind);
    UASSERT_EQ(42, (int)xv.v.i);
    /* Suppress unused-rc: also verify direct lookup hit (kind already set). */

    urbi_vm_destroy(&vm);
}

UTEST(emit_class_decl_empty_body) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UASSERT_EQ(URBI_OK, compile_and_run(&vm, "class Foo {}"));

    UValue foo = realm_get(&vm, "Foo");
    UASSERT_EQ((int)UVAL_OBJECT, (int)foo.kind);

    urbi_vm_destroy(&vm);
}

/* === Task 70: multi-proto MRO declaration order === */

UTEST(emit_class_decl_multi_proto_order) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* class Foo : public A, B { ... } — chain ends as [Foo, A, B, Object].
     * A.from_a and B.from_b should both be visible through Foo. */
    UASSERT_EQ(URBI_OK, compile_and_run(&vm,
        "var A = Object.clone();"
        "A.setSlot(\"from_a\", 100);"
        "var B = Object.clone();"
        "B.setSlot(\"from_b\", 200);"
        "class Foo : public A, B { var f = 42 }"));

    UValue foo = realm_get(&vm, "Foo");
    UASSERT_EQ((int)UVAL_OBJECT, (int)foo.kind);

    UValue va = obj_lookup(&vm, (UObject *)foo.v.p, "from_a");
    UASSERT_EQ(100, (int)va.v.i);
    UValue vb = obj_lookup(&vm, (UObject *)foo.v.p, "from_b");
    UASSERT_EQ(200, (int)vb.v.i);
    UValue vf = obj_lookup(&vm, (UObject *)foo.v.p, "f");
    UASSERT_EQ(42, (int)vf.v.i);

    urbi_vm_destroy(&vm);
}

UTEST(emit_class_decl_multi_proto_left_first) {
    /* When A and B both define `x`, A wins (declaration order = chain
     * head; left-first DFS).  A in front of B. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(URBI_OK, compile_and_run(&vm,
        "class A { var x = 1 };"
        "class B { var x = 2 };"
        "class C : public A, B { var y = 99 }"));

    UValue c = realm_get(&vm, "C");
    UASSERT_EQ((int)UVAL_OBJECT, (int)c.kind);

    UValue xv = obj_lookup(&vm, (UObject *)c.v.p, "x");
    UASSERT_EQ((int)UVAL_INT, (int)xv.kind);
    UASSERT_EQ(1, (int)xv.v.i);  /* A wins over B */

    urbi_vm_destroy(&vm);
}

/* === Task 72: function-decl in class body === */

UTEST(emit_class_decl_function_in_body) {
    /* class Foo { var bar = function() { return 42 } } — Foo.bar()
     * returns 42. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(URBI_OK, compile_and_run(&vm,
        "class Foo { var bar = function() { return 42 } };"
        "var r = Foo.bar()"));

    UValue rv = realm_get(&vm, "r");
    UASSERT_EQ((int)UVAL_INT, (int)rv.kind);
    UASSERT_EQ(42, (int)rv.v.i);

    urbi_vm_destroy(&vm);
}

/* === Task 81: class as expression value === */

UTEST(emit_class_decl_value_is_uobject) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(URBI_OK, compile_and_run(&vm,
        "class Foo {};"
        "var v = Foo"));

    UValue vv = realm_get(&vm, "v");
    UASSERT_EQ((int)UVAL_OBJECT, (int)vv.kind);

    urbi_vm_destroy(&vm);
}

/* === Task 83: two classes in same scope === */

UTEST(emit_class_decl_two_classes_same_scope) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(URBI_OK, compile_and_run(&vm,
        "class A { var x = 1 };"
        "class B { var y = 2 }"));

    UValue av = realm_get(&vm, "A");
    UValue bv = realm_get(&vm, "B");
    UASSERT_EQ((int)UVAL_OBJECT, (int)av.kind);
    UASSERT_EQ((int)UVAL_OBJECT, (int)bv.kind);

    UValue xv = obj_lookup(&vm, (UObject *)av.v.p, "x");
    UValue yv = obj_lookup(&vm, (UObject *)bv.v.p, "y");
    UASSERT_EQ(1, (int)xv.v.i);
    UASSERT_EQ(2, (int)yv.v.i);

    urbi_vm_destroy(&vm);
}

/* === Task 71: nested-class shadow emit (legacy class.chk) === */

UTEST(emit_class_decl_nested_shadow) {
    /* class a { var foo = 40 }; class a : public a { var bar = 2 }
     * The inner `class a : public a` resolves the proto `a` to the
     * OUTER `a`.  After execution, `a` (the inner) inherits foo from
     * the outer and has bar locally. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(URBI_OK, compile_and_run(&vm,
        "class a { var foo = 40 };"
        "class a : public a { var bar = 2 };"
        "var b = a.new();"
        "var sum = b.foo + b.bar"));

    UValue sumv = realm_get(&vm, "sum");
    UASSERT_EQ((int)UVAL_INT, (int)sumv.kind);
    UASSERT_EQ(42, (int)sumv.v.i);

    urbi_vm_destroy(&vm);
}

void test_class_decl_emit_suite(void) {
    utest_run("emit_class_decl_no_protos",            emit_class_decl_no_protos);
    utest_run("emit_class_decl_empty_body",           emit_class_decl_empty_body);
    utest_run("emit_class_decl_multi_proto_order",    emit_class_decl_multi_proto_order);
    utest_run("emit_class_decl_multi_proto_left_first", emit_class_decl_multi_proto_left_first);
    utest_run("emit_class_decl_function_in_body",     emit_class_decl_function_in_body);
    utest_run("emit_class_decl_value_is_uobject",     emit_class_decl_value_is_uobject);
    utest_run("emit_class_decl_two_classes_same_scope", emit_class_decl_two_classes_same_scope);
    utest_run("emit_class_decl_nested_shadow",        emit_class_decl_nested_shadow);
}
