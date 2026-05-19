/* SPDX-License-Identifier: BSD-3-Clause */
/* test_emit_this.c — Phase 2 (Gap #3): `this` keyword in method bodies.
 *
 * Verifies:
 *   - `this` inside a method body resolves to the receiver (R0).
 *   - `this` at top-level raises EMIT_NO_THIS_OUTSIDE_METHOD.
 *   - `this.slot` read inside a method accesses the receiver's slot.
 *   - `this.slot = v` write inside a method updates the receiver.
 *   - `this.method()` sibling-method call via receiver.
 *
 * Scope note: does NOT exercise closure upvalue capture (Gap #1); all
 * function literals used here only access `this` (the implicit receiver
 * R0), not any outer variables. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include <stddef.h>
#include <string.h>

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "object/uobject.h"
#include "emit/uemit.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "lex/ulex.h"
#include "chunk/uchunk.h"
#include "value/uarena.h"

#define UTEST(name) static void name(void)

/* Helper: compile src; return the emit error (EMIT_OK or error code).
 * Does NOT run the module. */
static UEmitError compile_only(UVM *vm, const char *src)
{
    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    uarena_init(&arena, 0);
    UProto module = {0};
    UEmitter e;
    uemit_init(&e, &module, &arena, vm, NULL);
    UParser p;
    uparse_init(&p, &lex, &arena);
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) {
            uarena_destroy(&arena);
            uchunk_destroy(&module, NULL);
            return EMIT_AST_ERROR;
        }
        UEmitError err = uemit_statement(&e, node);
        if (err != EMIT_OK) {
            uarena_destroy(&arena);
            uchunk_destroy(&module, NULL);
            return err;
        }
        uarena_reset(&arena);
    }
    UEmitError final_err = uemit_finish(&e);
    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
    return final_err;
}

/* Helper: compile + run src; return URBI_OK on success. */
static int compile_and_run(UVM *vm, const char *src)
{
    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    uarena_init(&arena, 0);
    /* Heap-allocate module: closures from function literals may survive this
     * helper's return and be GC-finalized only when urbi_vm_destroy is called. */
    UProto *module = (UProto *)vm->alloc_fn(NULL, sizeof(UProto), vm->alloc_ud);
    if (module == NULL) { uarena_destroy(&arena); return URBI_ERR_OOM; }
    memset(module, 0, sizeof(*module));
    module->heap_allocated = true;
    module->alloc_fn       = vm->alloc_fn;
    module->alloc_ud       = vm->alloc_ud;
    UEmitter e;
    uemit_init(&e, module, &arena, vm, NULL);
    UParser p;
    uparse_init(&p, &lex, &arena);
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) {
            uarena_destroy(&arena);
            uchunk_destroy(module, vm);
            return URBI_ERR_COMPILE;
        }
        if (uemit_statement(&e, node) != EMIT_OK) {
            uarena_destroy(&arena);
            uchunk_destroy(module, vm);
            return URBI_ERR_COMPILE;
        }
        uarena_reset(&arena);
    }
    if (uemit_finish(&e) != EMIT_OK) {
        uarena_destroy(&arena);
        uchunk_destroy(module, vm);
        return URBI_ERR_COMPILE;
    }
    UValue out = {0};
    int rc = urbi_run_chunk(vm, NULL, module, &out);
    uarena_destroy(&arena);
    uchunk_destroy(module, vm);
    return rc;
}

/* Helper: read a realm global by name. */
static UValue realm_get(UVM *vm, const char *name)
{
    URealm *realm = urbi_realm_global(vm);
    UValue out = urbi_make_nil();
    (void)urbi_realm_get_global(vm, realm, name, (int)strlen(name), &out);
    return out;
}

/* ===================================================================
 * T16-1: `this` at top-level raises EMIT_NO_THIS_OUTSIDE_METHOD
 * =================================================================== */

UTEST(emit_this_toplevel_error)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UEmitError err = compile_only(&vm, "this");
    UASSERT_EQ((int)EMIT_NO_THIS_OUTSIDE_METHOD, (int)err);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T16-2: `this` inside function body compiles without error
 * =================================================================== */

UTEST(emit_this_in_function_compiles)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* function() { this } — nested funcstate (parent != NULL) → OK */
    UEmitError err = compile_only(&vm, "function() { this }");
    UASSERT_EQ((int)EMIT_OK, (int)err);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T16-3: method body returns `this` (receiver identity)
 *
 * class T {}; T.setSlot("me", function() { this }); var t = T.new();
 * verify t.me() === t  (same object pointer)
 * =================================================================== */

UTEST(emit_this_returns_receiver)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UASSERT_EQ(URBI_OK, compile_and_run(&vm,
        "class T {};"
        "T.setSlot(\"me\", function() { this });"
        "var t = T.new();"
        "var result = t.me()"));

    UValue tv = realm_get(&vm, "t");
    UValue rv = realm_get(&vm, "result");

    /* Both must be objects and point to the same allocation. */
    UASSERT_EQ((int)UVAL_OBJECT, (int)tv.kind);
    UASSERT_EQ((int)UVAL_OBJECT, (int)rv.kind);
    UASSERT_EQ((int)(intptr_t)tv.v.p, (int)(intptr_t)rv.v.p);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T16-4: `this.slot` read — reads from receiver
 * =================================================================== */

UTEST(emit_this_slot_read)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UASSERT_EQ(URBI_OK, compile_and_run(&vm,
        "class C { var x = 99 };"
        "C.setSlot(\"getX\", function() { this.x });"
        "var c = C.new();"
        "var v = c.getX()"));

    UValue vv = realm_get(&vm, "v");
    UASSERT_EQ((int)UVAL_INT, (int)vv.kind);
    UASSERT_EQ(99, (int)vv.v.i);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T16-5: `this.slot = v` write — updates receiver slot
 * =================================================================== */

UTEST(emit_this_slot_write)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UASSERT_EQ(URBI_OK, compile_and_run(&vm,
        "class Counter { var count = 0 };"
        "Counter.setSlot(\"inc\", function() { this.count = this.count + 1 });"
        "var ct = Counter.new();"
        "ct.inc();"
        "ct.inc();"
        "var total = ct.count"));

    UValue tv = realm_get(&vm, "total");
    UASSERT_EQ((int)UVAL_INT, (int)tv.kind);
    UASSERT_EQ(2, (int)tv.v.i);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T16-6: `this.method()` — sibling-method call via receiver
 * =================================================================== */

UTEST(emit_this_method_call)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UASSERT_EQ(URBI_OK, compile_and_run(&vm,
        "class D { var val = 7 };"
        "D.setSlot(\"helper\", function() { this.val });"
        "D.setSlot(\"caller\", function() { this.helper() });"
        "var d = D.new();"
        "var w = d.caller()"));

    UValue wv = realm_get(&vm, "w");
    UASSERT_EQ((int)UVAL_INT, (int)wv.kind);
    UASSERT_EQ(7, (int)wv.v.i);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T16-7: `this` in deeply-nested function (parent->parent != NULL)
 * =================================================================== */

UTEST(emit_this_in_nested_function)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* function() { function() { this } } — two levels deep; both should
     * compile cleanly (inner funcstate has parent != NULL). */
    UEmitError err = compile_only(&vm, "function() { function() { this } }");
    UASSERT_EQ((int)EMIT_OK, (int)err);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T16-8: `this.slot = v` on instance does not mutate class prototype
 * =================================================================== */

UTEST(emit_this_slot_write_instance_only)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UASSERT_EQ(URBI_OK, compile_and_run(&vm,
        "class E { var n = 0 };"
        "E.setSlot(\"set5\", function() { this.n = 5 });"
        "var e1 = E.new();"
        "var e2 = E.new();"
        "e1.set5();"
        "var proto_n = E.n;"
        "var inst1_n = e1.n;"
        "var inst2_n = e2.n"));

    UValue pn = realm_get(&vm, "proto_n");
    UValue i1 = realm_get(&vm, "inst1_n");
    UValue i2 = realm_get(&vm, "inst2_n");

    /* proto unchanged */
    UASSERT_EQ((int)UVAL_INT, (int)pn.kind);
    UASSERT_EQ(0, (int)pn.v.i);
    /* e1 updated */
    UASSERT_EQ((int)UVAL_INT, (int)i1.kind);
    UASSERT_EQ(5, (int)i1.v.i);
    /* e2 untouched */
    UASSERT_EQ((int)UVAL_INT, (int)i2.kind);
    UASSERT_EQ(0, (int)i2.v.i);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T16-9: `this` in a class body function compiles OK
 * =================================================================== */

UTEST(emit_this_in_class_body_function)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* class F { var fn = function() { this } } — function literal inside
     * class body; the function's funcstate has parent != NULL. */
    UEmitError err = compile_only(&vm,
        "class F { var fn = function() { this } }");
    UASSERT_EQ((int)EMIT_OK, (int)err);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T16-10: `this` error message is EMIT_NO_THIS_OUTSIDE_METHOD
 * =================================================================== */

UTEST(emit_this_error_name)
{
    /* Verify the error name string is correct (string table lookup). */
    const char *name = uemit_error_name(EMIT_NO_THIS_OUTSIDE_METHOD);
    UASSERT(name != NULL);
    UASSERT(strcmp(name, "EMIT_NO_THIS_OUTSIDE_METHOD") == 0);
}

void test_emit_this_suite(void) {
    utest_run("emit_this_toplevel_error",          emit_this_toplevel_error);
    utest_run("emit_this_in_function_compiles",    emit_this_in_function_compiles);
    utest_run("emit_this_returns_receiver",        emit_this_returns_receiver);
    utest_run("emit_this_slot_read",               emit_this_slot_read);
    utest_run("emit_this_slot_write",              emit_this_slot_write);
    utest_run("emit_this_method_call",             emit_this_method_call);
    utest_run("emit_this_in_nested_function",      emit_this_in_nested_function);
    utest_run("emit_this_slot_write_instance_only", emit_this_slot_write_instance_only);
    utest_run("emit_this_in_class_body_function",  emit_this_in_class_body_function);
    utest_run("emit_this_error_name",              emit_this_error_name);
}
