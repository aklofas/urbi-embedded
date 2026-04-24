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

#include "uarena.h"
#include "uast.h"
#include "umodule.h"
#include "uemit.h"
#include "ulex.h"
#include "uparse.h"
#include "uvm.h"

#include <string.h>

#define UTEST(name) static void name(void)

/* Run one source string end-to-end through the pipeline.
   Returns UVM_OK with *out set on success, or the first non-OK error.
   All pipeline allocations are freed before return. */
static UVMError pipeline_eval(const char *src, UValue *out) {
    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    Arena arena;
    uarena_init(&arena, 4096);

    UModule module = {0};
    Emitter e;
    uemit_init(&e, &module, &arena, NULL);

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
        UVM vm;
        uvm_init(&vm, NULL, NULL);
        vm_rc = uvm_run(&vm, &module, out);
        uvm_destroy(&vm);
    }

    umodule_destroy(&module);
    uarena_destroy(&arena);
    return vm_rc;
}

/* 1 + 2: two integer literals, addition, Integer result. */
UTEST(pipeline_int_plus_int) {
    UValue out;
    UASSERT_EQ(UVM_OK, pipeline_eval("1 + 2", &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(3, out.v.i);
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
}
