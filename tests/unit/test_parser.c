/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for the streaming Pratt parser. */

#include "utest.h"
#include "uarena.h"
#include "uast.h"
#include "ulex.h"
#include "uparse.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* --- S-expression dump helper.  Bounds-guarded; truncates on overflow. --- */

static void dump_rec(const AstNode *n, char **cur, char *end) {
    if (*cur >= end) return;
    if (!n) { *cur += snprintf(*cur, (size_t)(end - *cur), "(null)"); return; }
    switch (n->kind) {
    case AST_INT:
        *cur += snprintf(*cur, (size_t)(end - *cur), "%" PRId64, n->u.i);
        return;
    case AST_IDENT:
        *cur += snprintf(*cur, (size_t)(end - *cur), "%.*s",
                         n->u.ident.len, n->u.ident.start);
        return;
    case AST_UNARY:
        *cur += snprintf(*cur, (size_t)(end - *cur), "(- ");
        dump_rec(n->u.unary.operand, cur, end);
        if (*cur < end) *cur += snprintf(*cur, (size_t)(end - *cur), ")");
        return;
    case AST_BINARY: {
        const char *op = "?";
        switch (n->u.binary.op) {
        case BOP_ADD: op = "+"; break;
        case BOP_SUB: op = "-"; break;
        case BOP_MUL: op = "*"; break;
        case BOP_DIV: op = "/"; break;
        }
        *cur += snprintf(*cur, (size_t)(end - *cur), "(%s ", op);
        dump_rec(n->u.binary.lhs, cur, end);
        if (*cur < end) *cur += snprintf(*cur, (size_t)(end - *cur), " ");
        dump_rec(n->u.binary.rhs, cur, end);
        if (*cur < end) *cur += snprintf(*cur, (size_t)(end - *cur), ")");
        return;
    }
    case AST_ERROR:
        *cur += snprintf(*cur, (size_t)(end - *cur), "(error %s)",
                         uparse_error_name((ParseErrorCode)n->u.err.code));
        return;
    }
}

static void ast_dump(const AstNode *n, char *buf, size_t bufsz) {
    if (bufsz == 0) return;
    char *cur = buf;
    char *end = buf + bufsz - 1; /* leave room for terminator */
    dump_rec(n, &cur, end);
    if (cur > end) cur = end;
    *cur = '\0';
}

/* --- Convenience: parse a source string, return the first statement. --- */

typedef struct {
    Lexer lex;
    Arena arena;
    Parser p;
} ParseCtx;

static void ctx_init(ParseCtx *c, const char *src) {
    ulex_init(&c->lex, src, strlen(src));
    uarena_init(&c->arena, 0);
    uparse_init(&c->p, &c->lex, &c->arena);
}

static void ctx_destroy(ParseCtx *c) {
    uarena_destroy(&c->arena);
}

/* --- Scaffolding tests. --- */

UTEST(parse_ast_dump_int_smoke) {
    AstNode n = { AST_INT, 1, 1, { .i = 42 } };
    char buf[32];
    ast_dump(&n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "42");
}

UTEST(parse_empty_input_returns_null) {
    ParseCtx c;
    ctx_init(&c, "");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n == NULL);
    ctx_destroy(&c);
}

UTEST(parse_error_name_known_codes) {
    UASSERT_STR_EQ(uparse_error_name(PARSE_OK),                 "PARSE_OK");
    UASSERT_STR_EQ(uparse_error_name(PARSE_UNEXPECTED_TOKEN),   "PARSE_UNEXPECTED_TOKEN");
    UASSERT_STR_EQ(uparse_error_name(PARSE_EXPECTED_RPAREN),    "PARSE_EXPECTED_RPAREN");
    UASSERT_STR_EQ(uparse_error_name(PARSE_OOM),                "PARSE_OOM");
}

UTEST(parse_error_name_out_of_range) {
    UASSERT_STR_EQ(uparse_error_name((ParseErrorCode)999),      "PARSE_UNKNOWN");
    UASSERT_STR_EQ(uparse_error_name((ParseErrorCode)-1),       "PARSE_UNKNOWN");
}

/* --- Atom happy-path tests. --- */

UTEST(parse_atom_int) {
    ParseCtx c;
    ctx_init(&c, "42");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_INT);
    UASSERT_EQ(n->u.i, 42);
    UASSERT_EQ(n->line, 1);
    UASSERT_EQ(n->col, 1);
    ctx_destroy(&c);
}

UTEST(parse_atom_ident) {
    ParseCtx c;
    ctx_init(&c, "foo");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_IDENT);
    UASSERT_EQ(n->u.ident.len, 3);
    UASSERT(n->u.ident.start != NULL);
    UASSERT_EQ(n->u.ident.start[0], 'f');
    UASSERT_EQ(n->u.ident.start[1], 'o');
    UASSERT_EQ(n->u.ident.start[2], 'o');
    ctx_destroy(&c);
}

UTEST(parse_atom_parens_no_wrapper_node) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "(42)");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "42");
    ctx_destroy(&c);
}

UTEST(parse_unary_neg) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "-42");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(- 42)");
    ctx_destroy(&c);
}

UTEST(parse_unary_pos_is_noop) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "+42");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "42");
    ctx_destroy(&c);
}

UTEST(parse_unary_double_neg_right_assoc) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "--3");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(- (- 3))");
    ctx_destroy(&c);
}

UTEST(parse_unary_neg_ident) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "-x");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(- x)");
    ctx_destroy(&c);
}

UTEST(parse_binary_add) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "1 + 2");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(+ 1 2)");
    ctx_destroy(&c);
}

UTEST(parse_precedence_add_mul) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "1 + 2 * 3");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(+ 1 (* 2 3))");
    ctx_destroy(&c);
}

UTEST(parse_precedence_mul_add) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "1 * 2 + 3");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(+ (* 1 2) 3)");
    ctx_destroy(&c);
}

UTEST(parse_left_assoc_sub) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "1 - 2 - 3");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(- (- 1 2) 3)");
    ctx_destroy(&c);
}

UTEST(parse_left_assoc_div) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "8 / 4 / 2");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(/ (/ 8 4) 2)");
    ctx_destroy(&c);
}

UTEST(parse_parens_override_precedence) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "(1 + 2) * 3");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(* (+ 1 2) 3)");
    ctx_destroy(&c);
}

UTEST(parse_unary_binds_tighter_than_binary) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "-1 + 2");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(+ (- 1) 2)");
    ctx_destroy(&c);
}

UTEST(parse_unary_on_parenthesized) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "-(1 + 2)");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(- (+ 1 2))");
    ctx_destroy(&c);
}

UTEST(parse_trailing_pipe_consumed) {
    ParseCtx c;
    ctx_init(&c, "42 |");
    AstNode *a = uparse_next_statement(&c.p);
    UASSERT(a != NULL);
    UASSERT_EQ(a->kind, AST_INT);
    /* After consuming '|', next call sees EOF. */
    AstNode *b = uparse_next_statement(&c.p);
    UASSERT(b == NULL);
    ctx_destroy(&c);
}

UTEST(parse_trailing_pipe_optional) {
    ParseCtx c;
    ctx_init(&c, "42");
    AstNode *a = uparse_next_statement(&c.p);
    UASSERT(a != NULL);
    UASSERT_EQ(a->kind, AST_INT);
    AstNode *b = uparse_next_statement(&c.p);
    UASSERT(b == NULL);
    ctx_destroy(&c);
}

UTEST(parse_two_statements_with_pipe) {
    char buf1[32], buf2[32];
    ParseCtx c;
    ctx_init(&c, "1 + 2 | 3 * 4 |");
    AstNode *a = uparse_next_statement(&c.p);
    UASSERT(a != NULL);
    ast_dump(a, buf1, sizeof buf1);
    UASSERT_STR_EQ(buf1, "(+ 1 2)");
    AstNode *b = uparse_next_statement(&c.p);
    UASSERT(b != NULL);
    ast_dump(b, buf2, sizeof buf2);
    UASSERT_STR_EQ(buf2, "(* 3 4)");
    AstNode *eof = uparse_next_statement(&c.p);
    UASSERT(eof == NULL);
    ctx_destroy(&c);
}

UTEST(parse_two_statements_no_final_pipe) {
    char buf1[32], buf2[32];
    ParseCtx c;
    ctx_init(&c, "1 | 2");
    AstNode *a = uparse_next_statement(&c.p);
    UASSERT(a != NULL);
    ast_dump(a, buf1, sizeof buf1);
    UASSERT_STR_EQ(buf1, "1");
    AstNode *b = uparse_next_statement(&c.p);
    UASSERT(b != NULL);
    ast_dump(b, buf2, sizeof buf2);
    UASSERT_STR_EQ(buf2, "2");
    AstNode *eof = uparse_next_statement(&c.p);
    UASSERT(eof == NULL);
    ctx_destroy(&c);
}

UTEST(parse_eof_is_idempotent) {
    ParseCtx c;
    ctx_init(&c, "");
    UASSERT(uparse_next_statement(&c.p) == NULL);
    UASSERT(uparse_next_statement(&c.p) == NULL);
    UASSERT(uparse_next_statement(&c.p) == NULL);
    ctx_destroy(&c);
}

UTEST(parse_whitespace_only_is_eof) {
    ParseCtx c;
    ctx_init(&c, "   \n\n  ");
    UASSERT(uparse_next_statement(&c.p) == NULL);
    ctx_destroy(&c);
}

UTEST(parse_error_unexpected_eof_after_operator) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "1 +");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_ERROR);
    UASSERT_EQ(n->u.err.code, PARSE_UNEXPECTED_EOF);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(error PARSE_UNEXPECTED_EOF)");
    ctx_destroy(&c);
}

UTEST(parse_error_expected_expression_closing_paren) {
    ParseCtx c;
    ctx_init(&c, "1 + )");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_ERROR);
    UASSERT_EQ(n->u.err.code, PARSE_EXPECTED_EXPRESSION);
    ctx_destroy(&c);
}

UTEST(parse_error_expected_rparen) {
    ParseCtx c;
    ctx_init(&c, "(1 + 2");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_ERROR);
    UASSERT_EQ(n->u.err.code, PARSE_EXPECTED_RPAREN);
    ctx_destroy(&c);
}

UTEST(parse_error_unexpected_token_at_statement_boundary) {
    ParseCtx c;
    ctx_init(&c, "1 2");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_ERROR);
    UASSERT_EQ(n->u.err.code, PARSE_UNEXPECTED_TOKEN);
    ctx_destroy(&c);
}

UTEST(parse_error_lex_error_passthrough) {
    ParseCtx c;
    ctx_init(&c, "@");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_ERROR);
    UASSERT_EQ(n->u.err.code, PARSE_LEX_ERROR);
    ctx_destroy(&c);
}

UTEST(parse_error_pipe_alone) {
    ParseCtx c;
    ctx_init(&c, "|");
    AstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_ERROR);
    UASSERT_EQ(n->u.err.code, PARSE_EXPECTED_EXPRESSION);
    ctx_destroy(&c);
}

UTEST(parse_recovery_sync_to_pipe_then_success) {
    char buf[64];
    ParseCtx c;
    /* '*' has no prefix form, so '1 + * 2' triggers PARSE_EXPECTED_EXPRESSION.
       Recovery must then sync past the first '|' and parse '3 * 4' cleanly. */
    ctx_init(&c, "1 + * 2 | 3 * 4 |");
    AstNode *err = uparse_next_statement(&c.p);
    UASSERT(err != NULL);
    UASSERT_EQ(err->kind, AST_ERROR);
    AstNode *ok = uparse_next_statement(&c.p);
    UASSERT(ok != NULL);
    ast_dump(ok, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(* 3 4)");
    UASSERT(uparse_next_statement(&c.p) == NULL);
    ctx_destroy(&c);
}

UTEST(parse_recovery_lex_error_followed_by_clean_statement) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "@ | 42 |");
    AstNode *err = uparse_next_statement(&c.p);
    UASSERT(err != NULL);
    UASSERT_EQ(err->kind, AST_ERROR);
    UASSERT_EQ(err->u.err.code, PARSE_LEX_ERROR);
    AstNode *ok = uparse_next_statement(&c.p);
    UASSERT(ok != NULL);
    ast_dump(ok, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "42");
    UASSERT(uparse_next_statement(&c.p) == NULL);
    ctx_destroy(&c);
}

UTEST(parse_recovery_consecutive_errors_terminate) {
    ParseCtx c;
    ctx_init(&c, "+ | + |");
    AstNode *a = uparse_next_statement(&c.p);
    UASSERT(a != NULL);
    UASSERT_EQ(a->kind, AST_ERROR);
    AstNode *b = uparse_next_statement(&c.p);
    UASSERT(b != NULL);
    UASSERT_EQ(b->kind, AST_ERROR);
    UASSERT(uparse_next_statement(&c.p) == NULL);
    ctx_destroy(&c);
}

void test_parser_suite(void) {
    utest_run("parse_empty_input_returns_null",  parse_empty_input_returns_null);
    utest_run("parse_error_name_known_codes",    parse_error_name_known_codes);
    utest_run("parse_error_name_out_of_range",   parse_error_name_out_of_range);
    utest_run("parse_ast_dump_int_smoke",        parse_ast_dump_int_smoke);
    utest_run("parse_atom_int",                    parse_atom_int);
    utest_run("parse_atom_ident",                  parse_atom_ident);
    utest_run("parse_atom_parens_no_wrapper_node", parse_atom_parens_no_wrapper_node);
    utest_run("parse_unary_neg",                    parse_unary_neg);
    utest_run("parse_unary_pos_is_noop",            parse_unary_pos_is_noop);
    utest_run("parse_unary_double_neg_right_assoc", parse_unary_double_neg_right_assoc);
    utest_run("parse_unary_neg_ident",              parse_unary_neg_ident);
    utest_run("parse_binary_add",                      parse_binary_add);
    utest_run("parse_precedence_add_mul",              parse_precedence_add_mul);
    utest_run("parse_precedence_mul_add",              parse_precedence_mul_add);
    utest_run("parse_left_assoc_sub",                  parse_left_assoc_sub);
    utest_run("parse_left_assoc_div",                  parse_left_assoc_div);
    utest_run("parse_parens_override_precedence",      parse_parens_override_precedence);
    utest_run("parse_unary_binds_tighter_than_binary", parse_unary_binds_tighter_than_binary);
    utest_run("parse_unary_on_parenthesized",          parse_unary_on_parenthesized);
    utest_run("parse_trailing_pipe_consumed",       parse_trailing_pipe_consumed);
    utest_run("parse_trailing_pipe_optional",       parse_trailing_pipe_optional);
    utest_run("parse_two_statements_with_pipe",     parse_two_statements_with_pipe);
    utest_run("parse_two_statements_no_final_pipe", parse_two_statements_no_final_pipe);
    utest_run("parse_eof_is_idempotent",            parse_eof_is_idempotent);
    utest_run("parse_whitespace_only_is_eof",       parse_whitespace_only_is_eof);
    utest_run("parse_error_unexpected_eof_after_operator",     parse_error_unexpected_eof_after_operator);
    utest_run("parse_error_expected_expression_closing_paren", parse_error_expected_expression_closing_paren);
    utest_run("parse_error_expected_rparen",                   parse_error_expected_rparen);
    utest_run("parse_error_unexpected_token_at_statement_boundary",
              parse_error_unexpected_token_at_statement_boundary);
    utest_run("parse_error_lex_error_passthrough",             parse_error_lex_error_passthrough);
    utest_run("parse_error_pipe_alone",                        parse_error_pipe_alone);
    utest_run("parse_recovery_sync_to_pipe_then_success",      parse_recovery_sync_to_pipe_then_success);
    utest_run("parse_recovery_lex_error_followed_by_clean_statement",
              parse_recovery_lex_error_followed_by_clean_statement);
    utest_run("parse_recovery_consecutive_errors_terminate",   parse_recovery_consecutive_errors_terminate);
}
