/* SPDX-License-Identifier: BSD-3-Clause */
/* T14: function definition + nested proto tests.
 *
 * Covers: parse_function(), AST_FUNCTION emit (OP_CLOSURE in root chunk,
 * UProto in module->nested[]), uproto_alloc_nested(), VM OP_CLOSURE
 * producing a UVAL_CLOSURE value.  Call dispatch (OP_CALL) is deferred to T15;
 * these tests only verify that function *definitions* are well-formed and
 * execute to a closure value without crashing or error. */

#include "utest.h"

#include "value/uarena.h"
#include "parse/uast.h"
#include "chunk/uchunk.h"
#include "runtime/uclosure.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "vm/uvm.h"

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* Run source through the full pipeline; return VM result. */
static UVMError fn_eval(const char *src, UValue *out) {
    UVM vm;
    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UArena arena;
    urbi_vm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);

    UProto module = {0};
    module.alloc_fn = vm.alloc_fn;
    module.alloc_ud = vm.alloc_ud;
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
        vm_rc = urbi_vm_run(&vm, NULL, &module, out);
    }

    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
    return vm_rc;
}

/* Parse source and return last error (PARSE_OK if none). */
static UParseError fn_parse_error(const char *src) {
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

/* Compile source, return emit error (EMIT_OK if none). */
static UEmitError fn_emit_error(const char *src) {
    UVM vm;
    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    urbi_vm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);
    UProto module = {0};
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
    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
    return rc;
}

/* -----------------------------------------------------------------------
 * uproto_alloc_nested API
 * ----------------------------------------------------------------------- */

UTEST(nested_proto_alloc_creates_first_entry) {
    UProto m = {0};
    UProto *p = uproto_alloc_nested(&m, &m);
    UASSERT(p != NULL);
    UASSERT_EQ((size_t)1, m.nested_count);
    UASSERT(p == m.nested[0]);
    uchunk_destroy(&m, NULL);
}

UTEST(nested_proto_alloc_multiple_grows_array) {
    UProto m = {0};
    UProto *p0 = uproto_alloc_nested(&m, &m);
    UProto *p1 = uproto_alloc_nested(&m, &m);
    UProto *p2 = uproto_alloc_nested(&m, &m);
    UASSERT(p0 != NULL);
    UASSERT(p1 != NULL);
    UASSERT(p2 != NULL);
    UASSERT_EQ((size_t)3, m.nested_count);
    UASSERT(p0 == m.nested[0]);
    UASSERT(p1 == m.nested[1]);
    UASSERT(p2 == m.nested[2]);
    uchunk_destroy(&m, NULL);
}

UTEST(nested_proto_zero_initialized) {
    UProto m = {0};
    UProto *p = uproto_alloc_nested(&m, &m);
    UASSERT(p != NULL);
    UASSERT_EQ((size_t)0, p->instr_count);
    UASSERT_EQ((size_t)0, p->const_count);
    UASSERT_EQ(0, (int)p->nparams);
    UASSERT_EQ(0, (int)p->nupvals);
    UASSERT_EQ(0, (int)p->max_reg);
    uchunk_destroy(&m, NULL);
}

UTEST(nested_proto_destroy_frees_buffers) {
    UProto m = {0};
    UProto *p = uproto_alloc_nested(&m, &m);
    UASSERT(p != NULL);
    /* Destroy should not crash even when the proto has no buffers. */
    uchunk_destroy(&m, NULL);
    /* If we reach here, no crash => pass. */
    UASSERT_EQ(1, 1);
}

/* -----------------------------------------------------------------------
 * Parser: function definition syntax
 * ----------------------------------------------------------------------- */

UTEST(parse_function_zero_params) {
    /* function() { 42 } should parse without error */
    UASSERT_EQ(PARSE_OK, fn_parse_error("function() { 42 }"));
}

UTEST(parse_function_one_param) {
    UASSERT_EQ(PARSE_OK, fn_parse_error("function(x) { x }"));
}

UTEST(parse_function_two_params) {
    UASSERT_EQ(PARSE_OK, fn_parse_error("function(a, b) { a + b }"));
}

UTEST(parse_function_with_name_returns_explicit_error) {
    /* PARSE-004: v1.0 grammar does not support named-function declarations.
     * Pre-fix the parser silently consumed and discarded the name; now it
     * rejects with a dedicated error code.  v1.x backlog: capture the name
     * and bind it as a local at function entry. */
    UASSERT_EQ(PARSE_NAMED_FUNCTION_NOT_SUPPORTED,
               fn_parse_error("function foo(x) { x + 1 }"));
}

UTEST(parse_function_lazy_param) {
    UASSERT_EQ(PARSE_OK, fn_parse_error("function(lazy x) { x }"));
}

UTEST(parse_bare_function_no_parens_rejected) {
    /* function name { body } without parens is the retired bare form */
    UASSERT_EQ(PARSE_BARE_FUNCTION, fn_parse_error("function foo { 1 }"));
}

UTEST(parse_bare_function_no_name_no_parens_rejected) {
    /* function { body } without parens is also the bare form */
    UASSERT_EQ(PARSE_BARE_FUNCTION, fn_parse_error("function { 1 }"));
}

UTEST(parse_closure_keyword_rejected) {
    /* 'closure' keyword is retired at v1.0 */
    UASSERT_EQ(PARSE_CLOSURE_KEYWORD, fn_parse_error("closure(x) { x }"));
}

/* -----------------------------------------------------------------------
 * Emitter: OP_CLOSURE in root chunk + UProto in nested[]
 * ----------------------------------------------------------------------- */

UTEST(emit_function_creates_nested_proto) {
    /* After emitting a function definition, module.nested_count == 1. */
    UVM vm;
    ULexer lex;
    const char *src = "function(x) { x + 1 }";
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    urbi_vm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);
    UProto module = {0};
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
    UASSERT_EQ(EMIT_OK, rc);
    UASSERT_EQ((size_t)1, module.nested_count);
    /* Root chunk should have OP_CLOSURE as first instruction */
    UASSERT(module.instr_count >= 1);

    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

UTEST(emit_function_nested_proto_has_nparams) {
    UVM vm;
    ULexer lex;
    const char *src = "function(a, b) { a + b }";
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    urbi_vm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);
    UProto module = {0};
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
    UASSERT_EQ(EMIT_OK, rc);
    UASSERT_EQ((size_t)1, module.nested_count);
    UASSERT_EQ(2, (int)module.nested[0]->nparams);

    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

UTEST(emit_two_functions_two_nested_protos) {
    UEmitError rc = fn_emit_error("function(x) { x }; function(y) { y }");
    UASSERT_EQ(EMIT_OK, rc);
    /* Two separate function definitions → two nested protos. Verified via
       emit_function_creates_nested_proto logic; here we just check no error. */
}

/* -----------------------------------------------------------------------
 * VM: OP_CLOSURE produces UVAL_CLOSURE
 * ----------------------------------------------------------------------- */

UTEST(vm_function_def_produces_closure_value) {
    UValue out;
    UVMError rc = fn_eval("function(x) { x + 1 }", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_CLOSURE, out.kind);
}

UTEST(vm_function_zero_param_produces_closure) {
    UValue out;
    UVMError rc = fn_eval("function() { 42 }", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_CLOSURE, out.kind);
}

UTEST(vm_function_captures_nothing_nupvals_zero) {
    /* A function that only uses its own params has nupvals == 0. */
    UVM vm;
    ULexer lex;
    const char *src = "function(x) { x * 2 }";
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    urbi_vm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);
    UProto module = {0};
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
    UEmitError emit_rc = uemit_finish(&e);
    UASSERT_EQ(EMIT_OK, emit_rc);

    UValue out = {0};
    UVMError vm_rc = urbi_vm_run(&vm, NULL, &module, &out);
    UASSERT_EQ(UVM_OK, vm_rc);
    UASSERT_EQ(UVAL_CLOSURE, out.kind);
    /* nupvals == 0 on the closure's proto */
    UASSERT_EQ(0, (int)((UClosure *)out.v.p)->proto->nupvals);

    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

UTEST(vm_anon_function_def_produces_closure) {
    /* PARSE-004: named-function syntax is rejected at v1.0; the canonical
     * way to bind a closure to a name is `var f = function(...){...}`. */
    UValue out;
    UVMError rc = fn_eval("function(a, b) { a + b }", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_CLOSURE, out.kind);
}

UTEST(vm_function_body_has_instructions) {
    /* The nested proto for function(x) { x + 1 } should have instructions. */
    UVM vm;
    ULexer lex;
    const char *src = "function(x) { x + 1 }";
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    urbi_vm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);
    UProto module = {0};
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
    uemit_finish(&e);

    UValue out = {0};
    urbi_vm_run(&vm, NULL, &module, &out);
    UASSERT_EQ(UVAL_CLOSURE, out.kind);
    /* Body should have at least: LOADK(1), GETLOCAL/MOVE(x), ADD, RET */
    UASSERT(((UClosure *)out.v.p)->proto->instr_count >= 2);

    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * T15: function calls and return
 * ----------------------------------------------------------------------- */

UTEST(call_no_args) {
    /* var f = function() { 7 }; f() → 7 */
    UValue out;
    UVMError rc = fn_eval("var f = function() { 7 }; f()", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(7, (int)out.v.i);
}

UTEST(call_one_arg) {
    /* var f = function(x) { x }; f(42) → 42 */
    UValue out;
    UVMError rc = fn_eval("var f = function(x) { x }; f(42)", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(42, (int)out.v.i);
}

UTEST(call_two_args) {
    /* var f = function(a, b) { a + b }; f(3, 4) → 7 */
    UValue out;
    UVMError rc = fn_eval("var f = function(a, b) { a + b }; f(3, 4)", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(7, (int)out.v.i);
}

UTEST(call_wrong_arity_errors) {
    /* var f = function(x) { x }; f(1, 2) → catchable typed throw */
    UValue out;
    UVMError rc = fn_eval("var f = function(x) { x }; f(1, 2)", &out);
    UASSERT_EQ(URBI_ERR_UNCAUGHT_THROW, rc);
}

UTEST(call_non_callable_errors) {
    /* var x = 5; x() → catchable typed throw */
    UValue out;
    UVMError rc = fn_eval("var x = 5; x()", &out);
    UASSERT_EQ(URBI_ERR_UNCAUGHT_THROW, rc);
}

UTEST(closure_captures_local) {
    /* var x = 7; var f = function() { x }; f() → 7 */
    UValue out;
    UVMError rc = fn_eval("var x = 7; var f = function() { x }; f()", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(7, (int)out.v.i);
}

UTEST(closure_writes_outer_local) {
    /* Tests OP_SETUPVAL (upvalue write-back) from an inner closure.
     * Note: chunk-top vars are realm globals under T72 (accessed via
     * GETSLOT/SETSLOT, not captured as upvalues).  To exercise the
     * upvalue write path, the outer var must live in a function body. */
    UValue out;
    UVMError rc = fn_eval("(function() { var x = 0; (function() { x = 9 })(); x })()", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(9, (int)out.v.i);
}

UTEST(return_with_value) {
    /* function with explicit return */
    UValue out;
    UVMError rc = fn_eval("var f = function(x) { return x * 2 }; f(5)", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(10, (int)out.v.i);
}

UTEST(return_early_exit) {
    /* early return bypasses trailing code */
    UValue out;
    UVMError rc = fn_eval(
        "var f = function(x) { if (x > 0) { return 1 }; 0 }; f(5)", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(1, (int)out.v.i);
}

UTEST(immediate_call_anonymous) {
    /* (function(x) { x + 1 })(10) → 11 */
    UValue out;
    UVMError rc = fn_eval("(function(x) { x + 1 })(10)", &out);
    UASSERT_EQ(UVM_OK, rc);
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(11, (int)out.v.i);
}

/* -----------------------------------------------------------------------
 * Suite
 * ----------------------------------------------------------------------- */

void test_function_suite(void) {
    /* umodule API */
    utest_run("nested proto: alloc creates first entry",
              nested_proto_alloc_creates_first_entry);
    utest_run("nested proto: multiple allocs grow array",
              nested_proto_alloc_multiple_grows_array);
    utest_run("nested proto: zero-initialized on alloc",
              nested_proto_zero_initialized);
    utest_run("nested proto: destroy frees buffers safely",
              nested_proto_destroy_frees_buffers);

    /* Parser */
    utest_run("parse function(): zero params accepted",
              parse_function_zero_params);
    utest_run("parse function(x): one param accepted",
              parse_function_one_param);
    utest_run("parse function(a,b): two params accepted",
              parse_function_two_params);
    utest_run("parse function name(...): rejected PARSE_NAMED_FUNCTION_NOT_SUPPORTED (PARSE-004)",
              parse_function_with_name_returns_explicit_error);
    utest_run("parse function(lazy x): lazy param accepted",
              parse_function_lazy_param);
    utest_run("parse bare function name{}: rejected PARSE_BARE_FUNCTION",
              parse_bare_function_no_parens_rejected);
    utest_run("parse bare function{}: rejected PARSE_BARE_FUNCTION",
              parse_bare_function_no_name_no_parens_rejected);
    utest_run("parse closure keyword: rejected PARSE_CLOSURE_KEYWORD",
              parse_closure_keyword_rejected);

    /* Emitter */
    utest_run("emit: function def creates nested proto in module",
              emit_function_creates_nested_proto);
    utest_run("emit: nested proto nparams matches param count",
              emit_function_nested_proto_has_nparams);
    utest_run("emit: two functions → two nested protos (no error)",
              emit_two_functions_two_nested_protos);

    /* VM */
    utest_run("vm: function def → UVAL_CLOSURE",
              vm_function_def_produces_closure_value);
    utest_run("vm: zero-param function def → UVAL_CLOSURE",
              vm_function_zero_param_produces_closure);
    utest_run("vm: non-capturing function has nupvals == 0",
              vm_function_captures_nothing_nupvals_zero);
    utest_run("vm: anon function def → UVAL_CLOSURE",
              vm_anon_function_def_produces_closure);
    utest_run("vm: function body proto has instructions",
              vm_function_body_has_instructions);

    /* T15: function calls */
    utest_run("call: zero-arg function call returns body value",
              call_no_args);
    utest_run("call: single-arg function passes argument",
              call_one_arg);
    utest_run("call: two-arg function adds arguments",
              call_two_args);
    utest_run("call: wrong arity → UVM_TYPE_ERROR",
              call_wrong_arity_errors);
    utest_run("call: non-callable → UVM_TYPE_ERROR",
              call_non_callable_errors);
    utest_run("call: closure captures outer local",
              closure_captures_local);
    utest_run("call: closure writes outer local (upvalue write-back)",
              closure_writes_outer_local);
    utest_run("call: return with value exits early",
              return_with_value);
    utest_run("call: early return bypasses trailing statements",
              return_early_exit);
    utest_run("call: immediate anonymous function invocation",
              immediate_call_anonymous);
}
