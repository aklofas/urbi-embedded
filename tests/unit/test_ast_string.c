/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ast_string.c — AST_STR node kind for string literals.
 *
 * Phase 1: parser folds escape sequences and adjacent-string concat
 * (REVIVAL §14.1 L3) into a heap-allocated buffer in the parser arena.
 * The AST_STR payload carries a non-owning view (bytes + len) into that
 * arena buffer; intern happens at emit time. */

#include "utest.h"
#include "lex/ulex.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "value/uarena.h"
#include <string.h>

typedef struct {
    ULexer lex;
    UArena arena;
    UParser p;
} ParseCtx;

static void ctx_init(ParseCtx *c, const char *src) {
    ulex_init(&c->lex, src, strlen(src));
    uarena_init(&c->arena, 0);
    uparse_init(&c->p, &c->lex, &c->arena);
}

static void ctx_destroy(ParseCtx *c) {
    uarena_destroy(&c->arena);
}

static void parse_basic_string_literal(void) {
    ParseCtx c;
    ctx_init(&c, "\"foo\"");
    UAstNode *expr = uparse_next_statement(&c.p);
    UASSERT(expr != NULL);
    UASSERT_EQ(expr->kind, AST_STR);
    UASSERT_EQ(expr->u.str_lit.len, 3);
    UASSERT(memcmp(expr->u.str_lit.bytes, "foo", 3) == 0);
    ctx_destroy(&c);
}

static void parse_string_with_escape(void) {
    /* "a\nb" — parser folds \n to byte 0x0A. */
    ParseCtx c;
    ctx_init(&c, "\"a\\nb\"");
    UAstNode *expr = uparse_next_statement(&c.p);
    UASSERT(expr != NULL);
    UASSERT_EQ(expr->kind, AST_STR);
    UASSERT_EQ(expr->u.str_lit.len, 3);
    UASSERT_EQ(expr->u.str_lit.bytes[0], 'a');
    UASSERT_EQ(expr->u.str_lit.bytes[1], '\n');
    UASSERT_EQ(expr->u.str_lit.bytes[2], 'b');
    ctx_destroy(&c);
}

static void parse_string_all_escapes(void) {
    /* "\n\t\\\"" — four escapes, four output bytes. */
    ParseCtx c;
    ctx_init(&c, "\"\\n\\t\\\\\\\"\"");
    UAstNode *expr = uparse_next_statement(&c.p);
    UASSERT(expr != NULL);
    UASSERT_EQ(expr->kind, AST_STR);
    UASSERT_EQ(expr->u.str_lit.len, 4);
    UASSERT_EQ(expr->u.str_lit.bytes[0], '\n');
    UASSERT_EQ(expr->u.str_lit.bytes[1], '\t');
    UASSERT_EQ(expr->u.str_lit.bytes[2], '\\');
    UASSERT_EQ(expr->u.str_lit.bytes[3], '"');
    ctx_destroy(&c);
}

static void parse_adjacent_strings_concat(void) {
    /* REVIVAL §14.1 L3: "a" "b" produces "ab" as a single AST_STR. */
    ParseCtx c;
    ctx_init(&c, "\"a\" \"b\"");
    UAstNode *expr = uparse_next_statement(&c.p);
    UASSERT(expr != NULL);
    UASSERT_EQ(expr->kind, AST_STR);
    UASSERT_EQ(expr->u.str_lit.len, 2);
    UASSERT(memcmp(expr->u.str_lit.bytes, "ab", 2) == 0);
    ctx_destroy(&c);
}

static void parse_three_adjacent_strings_concat(void) {
    /* "x" "y" "z" → "xyz". */
    ParseCtx c;
    ctx_init(&c, "\"x\" \"y\" \"z\"");
    UAstNode *expr = uparse_next_statement(&c.p);
    UASSERT(expr != NULL);
    UASSERT_EQ(expr->kind, AST_STR);
    UASSERT_EQ(expr->u.str_lit.len, 3);
    UASSERT(memcmp(expr->u.str_lit.bytes, "xyz", 3) == 0);
    ctx_destroy(&c);
}

static void parse_concat_with_escapes(void) {
    /* "a\n" "b" → 3 bytes: 'a', '\n', 'b'. */
    ParseCtx c;
    ctx_init(&c, "\"a\\n\" \"b\"");
    UAstNode *expr = uparse_next_statement(&c.p);
    UASSERT(expr != NULL);
    UASSERT_EQ(expr->kind, AST_STR);
    UASSERT_EQ(expr->u.str_lit.len, 3);
    UASSERT_EQ(expr->u.str_lit.bytes[0], 'a');
    UASSERT_EQ(expr->u.str_lit.bytes[1], '\n');
    UASSERT_EQ(expr->u.str_lit.bytes[2], 'b');
    ctx_destroy(&c);
}

void test_ast_string_suite(void) {
    utest_run("parse_basic_string_literal", parse_basic_string_literal);
    utest_run("parse_string_with_escape", parse_string_with_escape);
    utest_run("parse_string_all_escapes", parse_string_all_escapes);
    utest_run("parse_adjacent_strings_concat", parse_adjacent_strings_concat);
    utest_run("parse_three_adjacent_strings_concat", parse_three_adjacent_strings_concat);
    utest_run("parse_concat_with_escapes", parse_concat_with_escapes);
}
