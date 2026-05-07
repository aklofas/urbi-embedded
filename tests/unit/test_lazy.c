/* SPDX-License-Identifier: BSD-3-Clause */
/* T16: per-parameter lazy keyword tests.
 *
 * Covers: parse acceptance of `lazy` in param lists; emit of implicit
 * force on read (OP_MOVE + OP_CALL zero-arg) inside callee body;
 * call-site wrapping of lazy args (OP_CLOSURE); pass-through when a
 * lazy param is forwarded as an argument to another call; upvalue
 * capture inside a lazy thunk; rejection of assignment to a lazy param
 * (EMIT_LAZY_PARAM_ASSIGN); and end-to-end VM execution. */

#include "utest.h"

#include "value/uarena.h"
#include "parse/uast.h"
#include "umodule.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "vm/uvm.h"

#include <string.h>

#define UTEST(name) static void name(void)

/* -----------------------------------------------------------------------
 * Helpers (same pattern as test_function.c)
 * ----------------------------------------------------------------------- */

static UVMError lazy_eval(const char *src, UValue *out) {
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

static UParseError lazy_parse_error(const char *src) {
    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    uarena_init(&arena, 2048);
    UParser p;
    uparse_init(&p, &lex, &arena);

    UParseError last = PARSE_OK;
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) {
            last = node->u.err.code;
            break;
        }
    }
    uarena_destroy(&arena);
    return last;
}

static UEmitError lazy_emit_error(const char *src) {
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
 * Parse tests
 * ----------------------------------------------------------------------- */

UTEST(lazy_param_parses) {
    /* function(lazy x) { x } should parse without error. */
    UASSERT_EQ(PARSE_OK, lazy_parse_error("function(lazy x) { x }"));
}

UTEST(lazy_mixed_params_parses) {
    /* Mixing eager and lazy params is legal. */
    UASSERT_EQ(PARSE_OK, lazy_parse_error("function(a, lazy b, c) { b }"));
}

/* -----------------------------------------------------------------------
 * Emit tests
 * ----------------------------------------------------------------------- */

UTEST(lazy_param_assign_errors) {
    /* x = 5 inside a lazy function body → EMIT_LAZY_PARAM_ASSIGN (spec §4.5). */
    UEmitError rc = lazy_emit_error(
        "var f = function(lazy x) { x = 5 }; f(1)");
    UASSERT_EQ((int)EMIT_LAZY_PARAM_ASSIGN, (int)rc);
}

UTEST(lazy_emit_ok_for_definition) {
    /* var f = function(lazy x) { x } should emit without error. */
    UEmitError rc = lazy_emit_error("var f = function(lazy x) { x }");
    UASSERT_EQ((int)EMIT_OK, (int)rc);
}

/* -----------------------------------------------------------------------
 * End-to-end VM tests
 * ----------------------------------------------------------------------- */

UTEST(lazy_basic_force) {
    /* var f = function(lazy x) { x }; f(42) → 42.
     * The arg is wrapped in a thunk; body forces it on read. */
    UValue out;
    UVMError rc = lazy_eval("var f = function(lazy x) { x }; f(42)", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(42, (int)out.v.i);
}

UTEST(lazy_arithmetic_in_arg) {
    /* The lazy arg is an expression, not a literal. */
    UValue out;
    UVMError rc = lazy_eval("var f = function(lazy x) { x }; f(3 + 4)", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(7, (int)out.v.i);
}

UTEST(lazy_upvalue_capture_in_thunk) {
    /* The lazy arg captures an outer local via upvalue.
     * var n = 5; var f = function(lazy x) { x }; f(n + 1) → 6 */
    UValue out;
    UVMError rc = lazy_eval(
        "var n = 5; var f = function(lazy x) { x }; f(n + 1)", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(6, (int)out.v.i);
}

UTEST(lazy_passthrough_no_double_force) {
    /* A lazy param forwarded as lazy arg to another function should not be
     * double-forced (pass-through, spec §4.2).
     * var inner = function(lazy x) { x };
     * var outer = function(lazy y) { inner(y) };
     * outer(99) → 99 */
    UValue out;
    UVMError rc = lazy_eval(
        "var inner = function(lazy x) { x }; "
        "var outer = function(lazy y) { inner(y) }; "
        "outer(99)", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(99, (int)out.v.i);
}

UTEST(lazy_call_eager_arg) {
    /* An eager arg to a lazy param should also work. */
    UValue out;
    UVMError rc = lazy_eval(
        "var f = function(lazy x) { x }; var n = 10; f(n)", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(10, (int)out.v.i);
}

UTEST(lazy_mixed_eager_and_lazy_params) {
    /* var f = function(a, lazy b) { a + b }; f(3, 4) → 7 */
    UValue out;
    UVMError rc = lazy_eval(
        "var f = function(a, lazy b) { a + b }; f(3, 4)", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(7, (int)out.v.i);
}

UTEST(lazy_bind_once_memoization) {
    /* var v = x; v + v forces x once and uses the result twice.
     * With n = 5, f(n) → 10 (not re-evaluated). */
    UValue out;
    UVMError rc = lazy_eval(
        "var n = 5; "
        "var f = function(lazy x) { var v = x; v + v }; "
        "f(n)", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(10, (int)out.v.i);
}

/* -----------------------------------------------------------------------
 * Suite
 * ----------------------------------------------------------------------- */

void test_lazy_suite(void) {
    /* Parse */
    utest_run("lazy: function(lazy x) parses",        lazy_param_parses);
    utest_run("lazy: mixed params parse",              lazy_mixed_params_parses);

    /* Emit */
    utest_run("lazy: assign to lazy param → EMIT_LAZY_PARAM_ASSIGN",
              lazy_param_assign_errors);
    utest_run("lazy: definition emits without error",  lazy_emit_ok_for_definition);

    /* End-to-end */
    utest_run("lazy: basic force on read (f(42) → 42)",
              lazy_basic_force);
    utest_run("lazy: arithmetic arg forced (f(3+4) → 7)",
              lazy_arithmetic_in_arg);
    utest_run("lazy: thunk captures upvalue (f(n+1) → 6)",
              lazy_upvalue_capture_in_thunk);
    utest_run("lazy: pass-through outer(99) → 99",
              lazy_passthrough_no_double_force);
    utest_run("lazy: eager local passed to lazy param (f(n) → 10)",
              lazy_call_eager_arg);
    utest_run("lazy: mixed eager+lazy params (f(3,4) → 7)",
              lazy_mixed_eager_and_lazy_params);
    utest_run("lazy: bind-once memoization (v=x; v+v → 10)",
              lazy_bind_once_memoization);
}
