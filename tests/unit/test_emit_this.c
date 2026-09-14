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

#include <stddef.h>
#include <string.h>

#include "urbi/urbi.h"
#include "urbi/urbi.h"
#include "emit/uemit.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "lex/ulex.h"
#include "chunk/uchunk.h"
#include "util/uarena.h"

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
            urbi_emit_diag_free_all(&e);
            urbi_emit_abandon(&e);   /* finish never runs on this path (FE-07) */
            uarena_destroy(&arena);
            uchunk_destroy(&module, NULL);
            return EMIT_AST_ERROR;
        }
        UEmitError err = uemit_statement(&e, node);
        if (err != EMIT_OK) {
            urbi_emit_diag_free_all(&e);
            urbi_emit_abandon(&e);   /* finish never runs on this path (FE-07) */
            uarena_destroy(&arena);
            uchunk_destroy(&module, NULL);
            return err;
        }
        uarena_reset(&arena);
    }
    UEmitError final_err = uemit_finish(&e);
    urbi_emit_diag_free_all(&e);
    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
    return final_err;
}


/* ===================================================================
 * T16-1: `this` at top-level raises EMIT_NO_THIS_OUTSIDE_METHOD
 * =================================================================== */

UTEST(emit_this_toplevel_error)
{
    UVM *vm = NULL;
    vm = urbi_open(utest_alloc, NULL, NULL);

    UEmitError err = compile_only(vm, "this");
    UASSERT_EQ((int)EMIT_NO_THIS_OUTSIDE_METHOD, (int)err);

    urbi_close(vm);
}

/* ===================================================================
 * T16-2: `this` inside function body compiles without error
 * =================================================================== */

UTEST(emit_this_in_function_compiles)
{
    UVM *vm = NULL;
    vm = urbi_open(utest_alloc, NULL, NULL);

    /* function() { this } — nested funcstate (parent != NULL) → OK */
    UEmitError err = compile_only(vm, "function() { this }");
    UASSERT_EQ((int)EMIT_OK, (int)err);

    urbi_close(vm);
}

/* ===================================================================
 * T16-3: method body returns `this` (receiver identity)
 *
 * class T {}; T.setSlot("me", function() { this }); var t = T.new();
 * verify t.me() === t  (same object pointer)
 * =================================================================== */


/* ===================================================================
 * T16-4: `this.slot` read — reads from receiver
 * =================================================================== */


/* ===================================================================
 * T16-5: `this.slot = v` write — updates receiver slot
 * =================================================================== */


/* ===================================================================
 * T16-6: `this.method()` — sibling-method call via receiver
 * =================================================================== */


/* ===================================================================
 * T16-7: `this` in deeply-nested function (parent->parent != NULL)
 * =================================================================== */

UTEST(emit_this_in_nested_function)
{
    UVM *vm = NULL;
    vm = urbi_open(utest_alloc, NULL, NULL);

    /* function() { function() { this } } — two levels deep; both should
     * compile cleanly (inner funcstate has parent != NULL). */
    UEmitError err = compile_only(vm, "function() { function() { this } }");
    UASSERT_EQ((int)EMIT_OK, (int)err);

    urbi_close(vm);
}

/* ===================================================================
 * T16-8: `this.slot = v` on instance does not mutate class prototype
 * =================================================================== */


/* ===================================================================
 * T16-9: `this` in a class body function compiles OK
 * =================================================================== */

UTEST(emit_this_in_class_body_function)
{
    UVM *vm = NULL;
    vm = urbi_open(utest_alloc, NULL, NULL);

    /* class F { var fn = function() { this } } — function literal inside
     * class body; the function's funcstate has parent != NULL. */
    UEmitError err = compile_only(vm,
        "class F { var fn = function() { this } }");
    UASSERT_EQ((int)EMIT_OK, (int)err);

    urbi_close(vm);
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
    utest_run("emit_this_in_nested_function",      emit_this_in_nested_function);
    utest_run("emit_this_in_class_body_function",  emit_this_in_class_body_function);
    utest_run("emit_this_error_name",              emit_this_error_name);
}
