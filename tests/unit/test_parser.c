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
}
