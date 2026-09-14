/* SPDX-License-Identifier: BSD-3-Clause */
/* test_lshift_parse.c — v0.10.11 W3 lexer + parser tests for `<<`.
 *
 * Four tests:
 *   1. Lexer emits TOK_LSHIFT for `<<`.
 *   2. Lexer still produces TOK_LT for single `<` (regression guard).
 *   3. Lexer still produces TOK_LE for `<=` (regression guard).
 *
 * The `<<` dispatch itself is a runtime question, pinned by the .chk
 * corpus rather than here.
 */

#include "utest.h"
#include "lex/ulex.h"
#include "urbi/urbi.h"
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

void test_lshift_parse_suite(void) {
    utest_run("lshift_lexer_produces_tok_lshift",  lshift_lexer_produces_tok_lshift);
    utest_run("lshift_lexer_single_lt_unchanged",  lshift_lexer_single_lt_unchanged);
    utest_run("lshift_lexer_le_unchanged",          lshift_lexer_le_unchanged);
}
