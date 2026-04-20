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

static void int_zero(void) {
    Lexer l; ulex_init(&l, "0", 1);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 0);
    UASSERT_EQ(t.len, 1);
}

static void int_single_digit(void) {
    Lexer l; ulex_init(&l, "7", 1);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 7);
}

static void int_multi_digit(void) {
    Lexer l; ulex_init(&l, "42", 2);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 42);
    UASSERT_EQ(t.len, 2);
}

static void int_max_i64(void) {
    const char *s = "9223372036854775807";
    Lexer l; ulex_init(&l, s, 19);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 9223372036854775807LL);
}

static void int_overflow(void) {
    const char *s = "9223372036854775808";
    Lexer l; ulex_init(&l, s, 19);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_INT_OVERFLOW);
}

static void int_with_underscore(void) {
    Lexer l; ulex_init(&l, "1_000", 5);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 1000);
    UASSERT_EQ(t.len, 5);
}

static void int_with_underscores_multi(void) {
    Lexer l; ulex_init(&l, "1_000_000", 9);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 1000000);
}

static void int_trailing_underscore_errors(void) {
    Lexer l; ulex_init(&l, "42_", 3);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_TRAILING_UNDERSCORE);
}

static void int_adjacent_underscores_error(void) {
    Lexer l; ulex_init(&l, "1__000", 6);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_ADJACENT_UNDERSCORES);
}

static void int_leading_zero_ambiguous(void) {
    Lexer l; ulex_init(&l, "042", 3);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_AMBIGUOUS_LEADING_ZERO);
}

static void int_zero_alone_is_legal(void) {
    Lexer l; ulex_init(&l, "0", 1);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 0);
}

static void int_double_zero_is_ambiguous(void) {
    Lexer l; ulex_init(&l, "00", 2);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_AMBIGUOUS_LEADING_ZERO);
}

static void hex_lower_prefix(void) {
    Lexer l; ulex_init(&l, "0x2A", 4);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 42);
}

static void hex_upper_prefix(void) {
    Lexer l; ulex_init(&l, "0XFF", 4);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 255);
}

static void hex_with_underscores(void) {
    Lexer l; ulex_init(&l, "0xDEAD_BEEF", 11);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 0xDEADBEEF);
}

static void hex_i64_max(void) {
    Lexer l; ulex_init(&l, "0x7FFF_FFFF_FFFF_FFFF", 21);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 0x7FFFFFFFFFFFFFFFLL);
}

static void hex_empty_radix(void) {
    Lexer l; ulex_init(&l, "0x", 2);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_EMPTY_RADIX);
}

static void hex_malformed_digit(void) {
    Lexer l; ulex_init(&l, "0xG", 3);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_MALFORMED_HEX);
}

static void hex_leading_underscore(void) {
    Lexer l; ulex_init(&l, "0x_42", 5);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_LEADING_UNDERSCORE);
}

static void hex_trailing_underscore(void) {
    Lexer l; ulex_init(&l, "0x42_", 5);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_TRAILING_UNDERSCORE);
}

static void bin_simple(void) {
    Lexer l; ulex_init(&l, "0b1010", 6);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 10);
}

static void bin_upper_prefix(void) {
    Lexer l; ulex_init(&l, "0B1111", 6);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 15);
}

static void bin_with_underscores(void) {
    Lexer l; ulex_init(&l, "0b1010_0101", 11);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 0xA5);
}

static void bin_empty_radix(void) {
    Lexer l; ulex_init(&l, "0b", 2);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_EMPTY_RADIX);
}

static void bin_malformed_digit(void) {
    Lexer l; ulex_init(&l, "0b2", 3);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_MALFORMED_BIN);
}

static void bin_leading_underscore(void) {
    Lexer l; ulex_init(&l, "0b_1010", 7);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_LEADING_UNDERSCORE);
}

static void oct_simple(void) {
    Lexer l; ulex_init(&l, "0o42", 4);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 34);
}

static void oct_upper_prefix(void) {
    Lexer l; ulex_init(&l, "0O755", 5);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 493);
}

static void oct_with_underscores(void) {
    Lexer l; ulex_init(&l, "0o177_777", 9);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 0177777);
}

static void oct_empty_radix(void) {
    Lexer l; ulex_init(&l, "0o", 2);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_EMPTY_RADIX);
}

static void oct_malformed_eight(void) {
    Lexer l; ulex_init(&l, "0o8", 3);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_MALFORMED_OCT);
}

static void oct_malformed_nine(void) {
    Lexer l; ulex_init(&l, "0o9", 3);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_MALFORMED_OCT);
}

static void ident_single_letter(void) {
    Lexer l; ulex_init(&l, "x", 1);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_IDENT);
    UASSERT_EQ(t.u.str.len, 1);
    UASSERT_EQ(t.u.str.start[0], 'x');
}

static void ident_underscore_alone(void) {
    Lexer l; ulex_init(&l, "_", 1);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_IDENT);
    UASSERT_EQ(t.u.str.len, 1);
}

static void ident_underscore_prefixed(void) {
    Lexer l; ulex_init(&l, "_private", 8);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_IDENT);
    UASSERT_EQ(t.u.str.len, 8);
}

static void ident_with_digits(void) {
    Lexer l; ulex_init(&l, "foo123", 6);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_IDENT);
    UASSERT_EQ(t.u.str.len, 6);
}

static void ident_camel_case(void) {
    Lexer l; ulex_init(&l, "camelCase", 9);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_IDENT);
    UASSERT_EQ(t.u.str.len, 9);
}

static void ident_snake_case(void) {
    Lexer l; ulex_init(&l, "snake_case", 10);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_IDENT);
    UASSERT_EQ(t.u.str.len, 10);
}

static void leading_underscore_digit_is_ident(void) {
    /* Identifiers win at leading '_'. */
    Lexer l; ulex_init(&l, "_42", 3);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_IDENT);
    UASSERT_EQ(t.u.str.len, 3);
}

static void ident_start_points_into_source(void) {
    const char *src = "foo";
    Lexer l; ulex_init(&l, src, 3);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_IDENT);
    /* zero-copy: t.u.str.start is exactly src */
    UASSERT(t.u.str.start == src);
}

static void digit_then_ident_is_two_tokens(void) {
    /* 123foo lexes as TOK_INT(123) then TOK_IDENT(foo). */
    Lexer l; ulex_init(&l, "123foo", 6);
    Token t1 = ulex_next(&l);
    Token t2 = ulex_next(&l);
    UASSERT_EQ(t1.type, TOK_INT);
    UASSERT_EQ(t1.u.i, 123);
    UASSERT_EQ(t2.type, TOK_IDENT);
    UASSERT_EQ(t2.u.str.len, 3);
}

static void expr_one_plus_two_pipe(void) {
    /* The M1 walking-skeleton REPL target. */
    Lexer l; ulex_init(&l, "1 + 2 |", 7);
    Token t1 = ulex_next(&l);
    Token t2 = ulex_next(&l);
    Token t3 = ulex_next(&l);
    Token t4 = ulex_next(&l);
    Token t5 = ulex_next(&l);
    UASSERT_EQ(t1.type, TOK_INT);   UASSERT_EQ(t1.u.i, 1);
    UASSERT_EQ(t2.type, TOK_PLUS);
    UASSERT_EQ(t3.type, TOK_INT);   UASSERT_EQ(t3.u.i, 2);
    UASSERT_EQ(t4.type, TOK_PIPE);
    UASSERT_EQ(t5.type, TOK_EOF);
}

static void parenthesized_expression(void) {
    Lexer l; ulex_init(&l, "(1 + 2) * 3", 11);
    TokenType expected[] = {
        TOK_LPAREN, TOK_INT, TOK_PLUS, TOK_INT, TOK_RPAREN,
        TOK_STAR, TOK_INT, TOK_EOF
    };
    for (size_t i = 0; i < sizeof(expected)/sizeof(expected[0]); i++) {
        Token t = ulex_next(&l);
        UASSERT_EQ(t.type, expected[i]);
    }
}

static void ident_plus_ident(void) {
    Lexer l; ulex_init(&l, "foo + bar", 9);
    Token t1 = ulex_next(&l);
    Token t2 = ulex_next(&l);
    Token t3 = ulex_next(&l);
    UASSERT_EQ(t1.type, TOK_IDENT); UASSERT_EQ(t1.u.str.len, 3);
    UASSERT_EQ(t2.type, TOK_PLUS);
    UASSERT_EQ(t3.type, TOK_IDENT); UASSERT_EQ(t3.u.str.len, 3);
}

static void sync_line_accuracy_multi_line(void) {
    /* Token on line 3, col 5. */
    const char *src = "\n\n    x";
    Lexer l; ulex_init(&l, src, 7);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_IDENT);
    UASSERT_EQ(t.line, 3);
    UASSERT_EQ(t.col, 5);
}

static void sync_line_across_block_comment(void) {
    const char *src = "/* a\n   b */ x";
    Lexer l; ulex_init(&l, src, 14);
    Token t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_IDENT);
    UASSERT_EQ(t.line, 2);
    /* x is the 9th column on line 2: 3 spaces + b + space + star + slash + space */
    UASSERT_EQ(t.col, 9);
}

static void sequence_after_error_continues(void) {
    /* After an error, lexer advances past bad input; next token is valid. */
    Lexer l; ulex_init(&l, "042 + 1", 7);
    Token t1 = ulex_next(&l);
    UASSERT_EQ(t1.type, TOK_ERROR);
    UASSERT_EQ(t1.u.err.code, LEX_AMBIGUOUS_LEADING_ZERO);
    Token t2 = ulex_next(&l);
    UASSERT_EQ(t2.type, TOK_PLUS);
    Token t3 = ulex_next(&l);
    UASSERT_EQ(t3.type, TOK_INT);
    UASSERT_EQ(t3.u.i, 1);
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
    utest_run("int_zero", int_zero);
    utest_run("int_single_digit", int_single_digit);
    utest_run("int_multi_digit", int_multi_digit);
    utest_run("int_max_i64", int_max_i64);
    utest_run("int_overflow", int_overflow);
    utest_run("int_with_underscore", int_with_underscore);
    utest_run("int_with_underscores_multi", int_with_underscores_multi);
    utest_run("int_trailing_underscore_errors", int_trailing_underscore_errors);
    utest_run("int_adjacent_underscores_error", int_adjacent_underscores_error);
    utest_run("int_leading_zero_ambiguous", int_leading_zero_ambiguous);
    utest_run("int_zero_alone_is_legal", int_zero_alone_is_legal);
    utest_run("int_double_zero_is_ambiguous", int_double_zero_is_ambiguous);
    utest_run("hex_lower_prefix", hex_lower_prefix);
    utest_run("hex_upper_prefix", hex_upper_prefix);
    utest_run("hex_with_underscores", hex_with_underscores);
    utest_run("hex_i64_max", hex_i64_max);
    utest_run("hex_empty_radix", hex_empty_radix);
    utest_run("hex_malformed_digit", hex_malformed_digit);
    utest_run("hex_leading_underscore", hex_leading_underscore);
    utest_run("hex_trailing_underscore", hex_trailing_underscore);
    utest_run("bin_simple", bin_simple);
    utest_run("bin_upper_prefix", bin_upper_prefix);
    utest_run("bin_with_underscores", bin_with_underscores);
    utest_run("bin_empty_radix", bin_empty_radix);
    utest_run("bin_malformed_digit", bin_malformed_digit);
    utest_run("bin_leading_underscore", bin_leading_underscore);
    utest_run("oct_simple", oct_simple);
    utest_run("oct_upper_prefix", oct_upper_prefix);
    utest_run("oct_with_underscores", oct_with_underscores);
    utest_run("oct_empty_radix", oct_empty_radix);
    utest_run("oct_malformed_eight", oct_malformed_eight);
    utest_run("oct_malformed_nine", oct_malformed_nine);
    utest_run("ident_single_letter", ident_single_letter);
    utest_run("ident_underscore_alone", ident_underscore_alone);
    utest_run("ident_underscore_prefixed", ident_underscore_prefixed);
    utest_run("ident_with_digits", ident_with_digits);
    utest_run("ident_camel_case", ident_camel_case);
    utest_run("ident_snake_case", ident_snake_case);
    utest_run("leading_underscore_digit_is_ident", leading_underscore_digit_is_ident);
    utest_run("ident_start_points_into_source", ident_start_points_into_source);
    utest_run("digit_then_ident_is_two_tokens", digit_then_ident_is_two_tokens);
    utest_run("expr_one_plus_two_pipe", expr_one_plus_two_pipe);
    utest_run("parenthesized_expression", parenthesized_expression);
    utest_run("ident_plus_ident", ident_plus_ident);
    utest_run("sync_line_accuracy_multi_line", sync_line_accuracy_multi_line);
    utest_run("sync_line_across_block_comment", sync_line_across_block_comment);
    utest_run("sequence_after_error_continues", sequence_after_error_continues);
}
