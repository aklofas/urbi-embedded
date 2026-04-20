/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"
#include "ulex.h"
#include <string.h>

static void eof_on_empty_input(void) {
    Lexer l;
    ulex_init(&l, "", 0);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_EOF);
    UASSERT_EQ(t.line, 1);
    UASSERT_EQ(t.col, 1);
}

static void eof_is_idempotent(void) {
    Lexer l;
    ulex_init(&l, "", 0);
    Token t1 = ulex_next(&l);
    Token t2 = ulex_next(&l);
    Token t3 = ulex_next(&l);
    UASSERT_EQ(t1.type, TOK_EOF);
    UASSERT_EQ(t2.type, TOK_EOF);
    UASSERT_EQ(t3.type, TOK_EOF);
}

static void token_name_returns_static_strings(void) {
    UASSERT_STR_EQ(ulex_token_name(TOK_EOF), "TOK_EOF");
    UASSERT_STR_EQ(ulex_token_name(TOK_ERROR), "TOK_ERROR");
}

void test_lexer_suite(void) {
    utest_run("eof_on_empty_input", eof_on_empty_input);
    utest_run("eof_is_idempotent", eof_is_idempotent);
    utest_run("token_name_returns_static_strings", token_name_returns_static_strings);
}
