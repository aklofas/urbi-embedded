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

void test_parser_suite(void) {
    utest_run("parse_empty_input_returns_null",  parse_empty_input_returns_null);
    utest_run("parse_error_name_known_codes",    parse_error_name_known_codes);
    utest_run("parse_error_name_out_of_range",   parse_error_name_out_of_range);
}
