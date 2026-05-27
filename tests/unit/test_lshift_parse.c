/* SPDX-License-Identifier: BSD-3-Clause */
/* test_lshift_parse.c — v0.10.11 W3 lexer + parser tests for `<<`.
 *
 * Four tests:
 *   1. Lexer emits TOK_LSHIFT for `<<`.
 *   2. Lexer still produces TOK_LT for single `<` (regression guard).
 *   3. Lexer still produces TOK_LE for `<=` (regression guard).
 *   4. End-to-end: `<<` desugars to method call via urbi_eval / pipeline.
 */

#include "utest.h"
#include "utest_e2e_helpers.h"
#include "lex/ulex.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"

#include <string.h>

#define UTEST(name) static void name(void)

/* Test 1: lexer emits TOK_LSHIFT for `<<`. */
UTEST(lshift_lexer_produces_tok_lshift)
{
    ULexer l;
    ulex_init(&l, "<<", 2);
    const UToken t = ulex_next(&l);
    UASSERT_EQ((int)t.type, (int)TOK_LSHIFT);
    UASSERT_EQ(t.col, 1);
    /* Consume remaining token — should be EOF. */
    const UToken eof = ulex_next(&l);
    UASSERT_EQ((int)eof.type, (int)TOK_EOF);
}

/* Test 2: single `<` still produces TOK_LT. */
UTEST(lshift_lexer_single_lt_unchanged)
{
    ULexer l;
    ulex_init(&l, "<", 1);
    const UToken t = ulex_next(&l);
    UASSERT_EQ((int)t.type, (int)TOK_LT);
}

/* Test 3: `<=` still produces TOK_LE (= peek takes priority over < peek). */
UTEST(lshift_lexer_le_unchanged)
{
    ULexer l;
    ulex_init(&l, "<=", 2);
    const UToken t = ulex_next(&l);
    UASSERT_EQ((int)t.type, (int)TOK_LE);
    const UToken eof = ulex_next(&l);
    UASSERT_EQ((int)eof.type, (int)TOK_EOF);
}

/* Test 4: end-to-end — `<<` dispatches to the '<<' slot on lhs.
 *
 * Uses a class with a '<<' slot installed via setSlot that accumulates
 * the argument into a module-level counter.  Tests that:
 *   (a) the token lexes correctly,
 *   (b) the parser builds an AST_CALL on the quoted-ident selector,
 *   (c) the VM dispatches to the slot and returns the expected value. */
UTEST(lshift_e2e_method_dispatch)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out;
    /* Class with a '<<' slot that returns the argument doubled. */
    int rc = utest_e2e_compile_and_run(&vm,
        "class LShiftTest {};"
        " LShiftTest.setSlot(\"<<\", function(x) { x * 2 });"
        " var lt = LShiftTest.new();"
        " lt << 7",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(14, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

/* Test 5: left-associativity — `a << b << c` chains correctly.
 *
 * The slot returns `this` so each call wraps back to the receiver.
 * After three chained calls the result must equal the original object. */
UTEST(lshift_e2e_left_associative_chain)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out;
    int rc = utest_e2e_compile_and_run(&vm,
        "class ChainTest {};"
        " var ct = ChainTest.new();"
        " ChainTest.setSlot(\"<<\", function(x) { this });"
        " (ct << 1 << 2 << 3) == ct",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_BOOL, (int)out.kind);
    UASSERT_EQ(1, (int)out.v.i);  /* true */
    urbi_vm_destroy(&vm);
}

void test_lshift_parse_suite(void) {
    utest_run("lshift_lexer_produces_tok_lshift",  lshift_lexer_produces_tok_lshift);
    utest_run("lshift_lexer_single_lt_unchanged",  lshift_lexer_single_lt_unchanged);
    utest_run("lshift_lexer_le_unchanged",          lshift_lexer_le_unchanged);
    utest_run("lshift_e2e_method_dispatch",         lshift_e2e_method_dispatch);
    utest_run("lshift_e2e_left_associative_chain",  lshift_e2e_left_associative_chain);
}
