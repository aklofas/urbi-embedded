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

static void line_comment_to_eof(void) {
    Lexer l;
    ulex_init(&l, "// hello", 8);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_EOF);
}

static void line_comment_to_newline(void) {
    Lexer l;
    const char *s = "// hello\n";
    ulex_init(&l, s, 9);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_EOF);
    UASSERT_EQ(t.line, 2);
}

static void block_comment_single_line(void) {
    Lexer l;
    ulex_init(&l, "/* hi */", 8);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_EOF);
}

static void block_comment_spans_lines(void) {
    Lexer l;
    const char *s = "/* a\nb\nc */";
    ulex_init(&l, s, 11);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_EOF);
    UASSERT_EQ(t.line, 3);
}

static void unterminated_block_comment_emits_error(void) {
    Lexer l;
    ulex_init(&l, "/* oops", 7);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_UNTERMINATED_BLOCK_COMMENT);
    UASSERT_EQ(t.line, 1);
    UASSERT_EQ(t.col, 1);
    UASSERT(t.u.err.message != NULL);
    UASSERT_STR_EQ(t.u.err.message, "unterminated block comment");
}

static void plus_token(void) {
    Lexer l; ulex_init(&l, "+", 1);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_PLUS);
    UASSERT_EQ(t.len, 1);
    UASSERT_EQ(t.col, 1);
}

static void minus_token(void) {
    Lexer l; ulex_init(&l, "-", 1);
    UASSERT_EQ(ulex_next(&l).type, TOK_MINUS);
}

static void star_token(void) {
    Lexer l; ulex_init(&l, "*", 1);
    UASSERT_EQ(ulex_next(&l).type, TOK_STAR);
}

static void slash_token(void) {
    Lexer l; ulex_init(&l, "/", 1);
    UASSERT_EQ(ulex_next(&l).type, TOK_SLASH);
}

static void lparen_token(void) {
    Lexer l; ulex_init(&l, "(", 1);
    UASSERT_EQ(ulex_next(&l).type, TOK_LPAREN);
}

static void rparen_token(void) {
    Lexer l; ulex_init(&l, ")", 1);
    UASSERT_EQ(ulex_next(&l).type, TOK_RPAREN);
}

static void pipe_token(void) {
    Lexer l; ulex_init(&l, "|", 1);
    UASSERT_EQ(ulex_next(&l).type, TOK_PIPE);
}

static void plus_position_on_second_line(void) {
    Lexer l;
    const char *s = "  \n +";
    ulex_init(&l, s, 5);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_PLUS);
    UASSERT_EQ(t.line, 2);
    UASSERT_EQ(t.col, 2);
}

void test_lexer_suite(void) {
    utest_run("eof_on_empty_input", eof_on_empty_input);
    utest_run("eof_is_idempotent", eof_is_idempotent);
    utest_run("token_name_returns_static_strings", token_name_returns_static_strings);
    utest_run("whitespace_only_yields_eof_at_correct_position", whitespace_only_yields_eof_at_correct_position);
    utest_run("newline_lf_advances_line", newline_lf_advances_line);
    utest_run("newline_crlf_advances_line_once", newline_crlf_advances_line_once);
    utest_run("tabs_are_whitespace", tabs_are_whitespace);
    utest_run("line_comment_to_eof", line_comment_to_eof);
    utest_run("line_comment_to_newline", line_comment_to_newline);
    utest_run("block_comment_single_line", block_comment_single_line);
    utest_run("block_comment_spans_lines", block_comment_spans_lines);
    utest_run("unterminated_block_comment_emits_error", unterminated_block_comment_emits_error);
    utest_run("plus_token", plus_token);
    utest_run("minus_token", minus_token);
    utest_run("star_token", star_token);
    utest_run("slash_token", slash_token);
    utest_run("lparen_token", lparen_token);
    utest_run("rparen_token", rparen_token);
    utest_run("pipe_token", pipe_token);
    utest_run("plus_position_on_second_line", plus_position_on_second_line);
}
