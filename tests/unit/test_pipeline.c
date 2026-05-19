/* SPDX-License-Identifier: BSD-3-Clause */
/* End-to-end pipeline tests: lex → parse → emit → VM.
 * Covers VM spec §11 success criterion 7.
 * This is the first test file that exercises the full pipeline
 * composition rather than hand-fabricated UModules.
 *
 * Note: the M1 lexer produces only integer literals (TOK_INT); there is
 * no float-literal token.  Float results arise from division (OP_DIV always
 * produces Float) or from mixed-type arithmetic where one operand is already
 * Float — neither of which is reachable from source text alone at M1.
 * The spec §11 criterion "1 + 2.0 → Float 3.0" therefore cannot be tested
 * through the lexer; it is covered by hand-fabricated UModule tests in
 * test_vm.c (vm_add_int_float_promotes).  The three source-text cases
 * exercised here are those achievable with the M1 grammar. */

#include "utest.h"

#include "value/uarena.h"
#include "parse/uast.h"
#include "chunk/umodule.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "vm/uvm.h"

#include <string.h>

#define UTEST(name) static void name(void)

/* Run one source string end-to-end through the pipeline.
   Returns UVM_OK with *out set on success, or the first non-OK error.
   All pipeline allocations are freed before return. */
static UVMError pipeline_eval(const char *src, UValue *out) {
    UVM vm;
    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UArena arena;
    urbi_vm_init(&vm, NULL, NULL);
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
        /* Reset arena between statements so AST memory is reclaimed.
           The emitter has already consumed the tree at this point. */
        uarena_reset(&arena);
    }

    UValue nil = {0};
    *out = nil;
    UVMError vm_rc = UVM_OK;

    if (uemit_finish(&e) == EMIT_OK) {
        vm_rc = urbi_vm_run(&vm, NULL, &module, out);
    }

    umodule_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
    return vm_rc;
}

/* 1 + 2: two integer literals, addition, Integer result. */
UTEST(pipeline_int_plus_int) {
    UValue out;
    UASSERT_EQ(UVM_OK, pipeline_eval("1 + 2", &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(3, out.v.i);
}

/* var x = 7 followed by x: exercises var-decl + local read via OP_MOVE.
   Uses ; (sequential) so the last statement value propagates to *out. */
UTEST(pipeline_var_decl_and_read) {
    UValue out;
    UASSERT_EQ(UVM_OK, pipeline_eval("var x = 7 ; x", &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(7, out.v.i);
}

/* true literal: exercises OP_LOADBOOL through the pipeline. */
UTEST(pipeline_bool_true) {
    UValue out;
    UASSERT_EQ(UVM_OK, pipeline_eval("true", &out));
    UASSERT_EQ(UVAL_BOOL, out.kind);
    UASSERT(out.v.i != 0);
}

/* nil literal: exercises OP_LOADNIL through the pipeline. */
UTEST(pipeline_nil_literal) {
    UValue out;
    UASSERT_EQ(UVM_OK, pipeline_eval("nil", &out));
    UASSERT_EQ(UVAL_NIL, out.kind);
}

/* if-then-else: exercises OP_EQ, OP_TEST, OP_JMP through the pipeline. */
UTEST(pipeline_if_else_taken) {
    UValue out;
    UASSERT_EQ(UVM_OK, pipeline_eval("if (1 == 1) { 42 } else { 99 }", &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(42, out.v.i);
}

UTEST(pipeline_if_else_not_taken) {
    UValue out;
    UASSERT_EQ(UVM_OK, pipeline_eval("if (1 == 2) { 42 } else { 99 }", &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(99, out.v.i);
}

/* != comparison: exercises OP_NEQ path through the full pipeline. */
UTEST(pipeline_neq_true) {
    UValue out;
    UASSERT_EQ(UVM_OK, pipeline_eval("if (1 != 2) { 11 } else { 22 }", &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(11, out.v.i);
}

/* while with counter: exercises OP_LT + loop back-edge through the pipeline. */
UTEST(pipeline_while_count) {
    UValue out;
    UASSERT_EQ(UVM_OK, pipeline_eval(
        "var n = 0 ; while (n < 3) { n = n + 1 } ; n", &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(3, out.v.i);
}

/* Immediate-invoke zero-arg function: exercises OP_CLOSURE + OP_CALL. */
UTEST(pipeline_immediate_invoke) {
    UValue out;
    UASSERT_EQ(UVM_OK, pipeline_eval("(function() { 55 })()", &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(55, out.v.i);
}

/* Single-arg function call: exercises OP_CALL arg passing + OP_GETUPVAL path
   for the parameter register. */
UTEST(pipeline_single_arg_call) {
    UValue out;
    UASSERT_EQ(UVM_OK, pipeline_eval("(function(x) { x + 1 })(10)", &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(11, out.v.i);
}

/* Closure capturing outer local: exercises OP_GETUPVAL (upvalue read) and
   OP_SETUPVAL (upvalue write) through the pipeline.
   Note: chunk-top vars are realm globals under T72 (accessed via GETSLOT/SETSLOT,
   not captured as upvalues).  The local-upvalue write path requires the outer
   variable to live inside a function body.  We wrap the pattern accordingly. */
UTEST(pipeline_closure_upvalue_write) {
    UValue out;
    UASSERT_EQ(UVM_OK, pipeline_eval(
        "(function() { var x = 1 ; (function() { x = 2 })() ; x })()", &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(2, out.v.i);
}

/* 5 / 2: integer division always produces Float per LANG-CONVENTIONS §1.3. */
UTEST(pipeline_int_div_int) {
    UValue out;
    UASSERT_EQ(UVM_OK, pipeline_eval("5 / 2", &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f > 2.49 && out.v.f < 2.51);
}

/* 1 + 5 / 2: division yields Float 2.5; adding Integer 1 promotes to Float 3.5.
   This exercises the Int+Float promotion path through the full pipeline. */
UTEST(pipeline_int_plus_div_result_promotes_to_float) {
    UValue out;
    UASSERT_EQ(UVM_OK, pipeline_eval("1 + 5 / 2", &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f > 3.49 && out.v.f < 3.51);
}

void test_pipeline_suite(void) {
    utest_run("pipeline: 1 + 2 → Integer 3", pipeline_int_plus_int);
    utest_run("pipeline: 5 / 2 → Float 2.5 (DIV always Float)",
              pipeline_int_div_int);
    utest_run("pipeline: 1 + 5 / 2 → Float 3.5 (Int+Float promotion)",
              pipeline_int_plus_div_result_promotes_to_float);
    utest_run("pipeline: var x = 7 | x → 7 (var-decl + local read)",
              pipeline_var_decl_and_read);
    utest_run("pipeline: true → bool true (LOADBOOL)", pipeline_bool_true);
    utest_run("pipeline: nil → nil (LOADNIL)", pipeline_nil_literal);
    utest_run("pipeline: if (1==1) {42} else {99} → 42 (branch taken)",
              pipeline_if_else_taken);
    utest_run("pipeline: if (1==2) {42} else {99} → 99 (branch not taken)",
              pipeline_if_else_not_taken);
    utest_run("pipeline: if (1!=2) {11} else {22} → 11 (NEQ path)",
              pipeline_neq_true);
    utest_run("pipeline: while (n<3) { n=n+1 } → n=3 (LT + loop)",
              pipeline_while_count);
    utest_run("pipeline: (function(){55})() → 55 (CLOSURE + CALL)",
              pipeline_immediate_invoke);
    utest_run("pipeline: (function(x){x+1})(10) → 11 (arg pass)",
              pipeline_single_arg_call);
    utest_run("pipeline: closure upvalue write x=1; fn(){x=2}(); x → 2",
              pipeline_closure_upvalue_write);
}
