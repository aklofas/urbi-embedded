/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for two-tier separator parsing (';'/',' outer, '|'/'&' inner). */

#include "utest.h"
#include "value/uarena.h"
#include "parse/uast.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include <string.h>

#define UTEST(name) static void name(void)

typedef struct {
    ULexer lex;
    UArena arena;
    UParser p;
} SepCtx;

static void sep_ctx_init(SepCtx *c, const char *src) {
    ulex_init(&c->lex, src, strlen(src));
    uarena_init(&c->arena, 0);
    uparse_init(&c->p, &c->lex, &c->arena);
}

static void sep_ctx_destroy(SepCtx *c) {
    uarena_destroy(&c->arena);
}

/* Parse one statement from src. */
static UAstNode *parse_one(const char *src, SepCtx *c) {
    sep_ctx_init(c, src);
    return uparse_next_statement(&c->p);
}

UTEST(parse_pipe_left_assoc) {
    /* "1 | 2 | 3" must parse as (1|2)|3 — left-associative inner-tier. */
    SepCtx c;
    UAstNode *n = parse_one("1 | 2 | 3", &c);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_BIN_SEP, (int)n->kind);
    UASSERT_EQ((int)SEP_PIPE, (int)n->u.bin_sep.separator);
    /* lhs is itself a BIN_SEP: (1|2) */
    UASSERT_EQ((int)AST_BIN_SEP, (int)n->u.bin_sep.lhs->kind);
    UASSERT_EQ((int)SEP_PIPE, (int)n->u.bin_sep.lhs->u.bin_sep.separator);
    /* rhs is the leaf 3 */
    UASSERT_EQ((int)AST_INT, (int)n->u.bin_sep.rhs->kind);
    UASSERT_EQ((int64_t)3, n->u.bin_sep.rhs->u.i);
    sep_ctx_destroy(&c);
}

UTEST(parse_amp_left_assoc) {
    /* "1 & 2 & 3" parses as (1&2)&3. */
    SepCtx c;
    UAstNode *n = parse_one("1 & 2 & 3", &c);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_BIN_SEP, (int)n->kind);
    UASSERT_EQ((int)SEP_AMP, (int)n->u.bin_sep.separator);
    UASSERT_EQ((int)AST_BIN_SEP, (int)n->u.bin_sep.lhs->kind);
    UASSERT_EQ((int)SEP_AMP, (int)n->u.bin_sep.lhs->u.bin_sep.separator);
    sep_ctx_destroy(&c);
}

UTEST(parse_pipe_amp_same_tier) {
    /* "1 | 2 & 3" — both inner-tier, left-associative: ((1|2)&3). */
    SepCtx c;
    UAstNode *n = parse_one("1 | 2 & 3", &c);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_BIN_SEP, (int)n->kind);
    UASSERT_EQ((int)SEP_AMP, (int)n->u.bin_sep.separator);
    /* lhs = 1|2 */
    UASSERT_EQ((int)AST_BIN_SEP, (int)n->u.bin_sep.lhs->kind);
    UASSERT_EQ((int)SEP_PIPE, (int)n->u.bin_sep.lhs->u.bin_sep.separator);
    /* rhs = 3 */
    UASSERT_EQ((int)AST_INT, (int)n->u.bin_sep.rhs->kind);
    sep_ctx_destroy(&c);
}

UTEST(parse_semi_two_children) {
    /* "1; 2" is one AST_NARY with two children. */
    SepCtx c;
    UAstNode *n = parse_one("1; 2", &c);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_NARY, (int)n->kind);
    UASSERT_EQ((int)SEP_SEMI, (int)n->u.nary.separator);
    UASSERT_EQ(2, n->u.nary.count);
    UASSERT_EQ((int)AST_INT, (int)n->u.nary.children[0]->kind);
    UASSERT_EQ((int)AST_INT, (int)n->u.nary.children[1]->kind);
    UASSERT_EQ((int64_t)1, n->u.nary.children[0]->u.i);
    UASSERT_EQ((int64_t)2, n->u.nary.children[1]->u.i);
    sep_ctx_destroy(&c);
}

UTEST(parse_trailing_pipe_dropped) {
    /* "1 |" — trailing '|' at EOF is silently dropped; result is just 1. */
    SepCtx c;
    UAstNode *n = parse_one("1 |", &c);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_INT, (int)n->kind);
    UASSERT_EQ((int64_t)1, n->u.i);
    sep_ctx_destroy(&c);
}

UTEST(parse_trailing_semi_dropped) {
    /* "1;" — trailing ';' at EOF is silently dropped; result is just 1. */
    SepCtx c;
    UAstNode *n = parse_one("1;", &c);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_INT, (int)n->kind);
    UASSERT_EQ((int64_t)1, n->u.i);
    sep_ctx_destroy(&c);
}

UTEST(parse_trailing_amp_is_error) {
    /* "1 &" — trailing '&' is a parse error (PARSE_TRAILING_AMP). */
    SepCtx c;
    UAstNode *n = parse_one("1 &", &c);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_ERROR, (int)n->kind);
    UASSERT_EQ((int)PARSE_TRAILING_AMP, n->u.err.code);
    sep_ctx_destroy(&c);
}

void test_separators_suite(void) {
    utest_run("parse | left-assoc",              parse_pipe_left_assoc);
    utest_run("parse & left-assoc",              parse_amp_left_assoc);
    utest_run("parse |/& same-tier left-assoc",  parse_pipe_amp_same_tier);
    utest_run("parse ; two children",            parse_semi_two_children);
    utest_run("parse trailing | dropped",        parse_trailing_pipe_dropped);
    utest_run("parse trailing ; dropped",        parse_trailing_semi_dropped);
    utest_run("parse trailing & is error",       parse_trailing_amp_is_error);
}
