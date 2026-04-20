/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"
#include "ulex.h"

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

static void whitespace_only_yields_eof_at_correct_position(void) {
    Lexer l;
    ulex_init(&l, "   ", 3);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_EOF);
    UASSERT_EQ(t.line, 1);
    UASSERT_EQ(t.col, 4);
}

static void newline_lf_advances_line(void) {
    Lexer l;
    ulex_init(&l, "\n\n", 2);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_EOF);
    UASSERT_EQ(t.line, 3);
    UASSERT_EQ(t.col, 1);
}

static void newline_crlf_advances_line_once(void) {
    Lexer l;
    ulex_init(&l, "\r\n\r\n", 4);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_EOF);
    UASSERT_EQ(t.line, 3);
    UASSERT_EQ(t.col, 1);
}

static void tabs_are_whitespace(void) {
    Lexer l;
    ulex_init(&l, "\t\t\t", 3);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_EOF);
    UASSERT_EQ(t.line, 1);
    /* tab counts as one column per spec §8 */
    UASSERT_EQ(t.col, 4);
}

void test_lexer_suite(void) {
    utest_run("eof_on_empty_input", eof_on_empty_input);
    utest_run("eof_is_idempotent", eof_is_idempotent);
    utest_run("token_name_returns_static_strings", token_name_returns_static_strings);
    utest_run("whitespace_only_yields_eof_at_correct_position", whitespace_only_yields_eof_at_correct_position);
    utest_run("newline_lf_advances_line", newline_lf_advances_line);
    utest_run("newline_crlf_advances_line_once", newline_crlf_advances_line_once);
    utest_run("tabs_are_whitespace", tabs_are_whitespace);
}
