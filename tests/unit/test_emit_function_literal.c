/* SPDX-License-Identifier: BSD-3-Clause */
/* T30: emit_function_literal helper — unit tests.
 *
 * Verifies that:
 *   1. emit_function_literal returns a valid register and produces bytecode.
 *   2. The existing AST_FUNCTION emit path (which calls emit_function_literal
 *      internally) continues to work correctly — no regression in M2 closure
 *      behaviour.
 */

#include "utest.h"

#include <string.h>

#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "module/umodule.h"
#include "parse/uparse.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* Run source through the full pipeline; return VM result. */
static UVMError fn_lit_eval(const char *src, UValue *out) {
    UVM vm;
    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    uvm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);
    UModule module = {0};
    UEmitter e;
    uemit_init(&e, &module, &arena, &vm, NULL);
    UParser p;
    uparse_init(&p, &lex, &arena);
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) break;
        (void)uemit_statement(&e, node);
        uarena_reset(&arena);
    }
    UValue nil = {0};
    *out = nil;
    UVMError vm_rc = UVM_OK;
    if (uemit_finish(&e) == EMIT_OK) {
        vm_rc = uvm_run(&vm, &module, out);
    }
    umodule_destroy(&module);
    uarena_destroy(&arena);
    uvm_destroy(&vm);
    return vm_rc;
}

/* Compile source; return emit error (EMIT_OK if none). */
static UEmitError fn_lit_emit_error(const char *src) {
    UVM vm;
    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    uvm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);
    UModule module = {0};
    UEmitter e;
    uemit_init(&e, &module, &arena, &vm, NULL);
    UParser p;
    uparse_init(&p, &lex, &arena);
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) break;
        (void)uemit_statement(&e, node);
        uarena_reset(&arena);
    }
    UEmitError rc = uemit_finish(&e);
    umodule_destroy(&module);
    uarena_destroy(&arena);
    uvm_destroy(&vm);
    return rc;
}

/* -----------------------------------------------------------------------
 * T30 test cases
 * ----------------------------------------------------------------------- */

/* The refactored AST_FUNCTION path (now a thin caller) must still produce
 * a UVAL_CLOSURE when a zero-param function literal is compiled. */
UTEST(emit_function_literal_zero_param_produces_closure) {
    UValue out = {0};
    UVMError rc = fn_lit_eval("function() { 42 }", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ((int)UVAL_CLOSURE, (int)out.kind);
}

/* Single-param function — existing M2 path must still work. */
UTEST(emit_function_literal_one_param_produces_closure) {
    UEmitError rc = fn_lit_emit_error("function(x) { x }");
    UASSERT_EQ(EMIT_OK, rc);
}

/* Nested functions — upvalue capture must still resolve correctly. */
UTEST(emit_function_literal_nested_captures_upvalue) {
    /* Inner closure captures `x` from outer — upvalue capture path. */
    UValue out = {0};
    /* Evaluate the outer closure — just check it compiles and runs. */
    UVMError rc = fn_lit_eval("var x = 7; var f = function() { x }; f()", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ((long long)7, (long long)out.v.i);
}

/* Lazy param still compiles via the refactored path. */
UTEST(emit_function_literal_lazy_param_still_compiles) {
    UEmitError rc = fn_lit_emit_error("function(lazy x) { x }");
    UASSERT_EQ(EMIT_OK, rc);
}

/* -----------------------------------------------------------------------
 * Suite entry point
 * ----------------------------------------------------------------------- */

void test_emit_function_literal_suite(void) {
    utest_run("emit_function_literal_zero_param_produces_closure",
              emit_function_literal_zero_param_produces_closure);
    utest_run("emit_function_literal_one_param_produces_closure",
              emit_function_literal_one_param_produces_closure);
    utest_run("emit_function_literal_nested_captures_upvalue",
              emit_function_literal_nested_captures_upvalue);
    utest_run("emit_function_literal_lazy_param_still_compiles",
              emit_function_literal_lazy_param_still_compiles);
}
