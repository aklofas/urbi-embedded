/* SPDX-License-Identifier: BSD-3-Clause */
/* T46: Postfix `e!` desugar and prefix `!x` logical-NOT tests.
 *
 * `e!`       → AST_CALL { callee=AST_MEMBER_GET(e, "emit"), args=[] }
 * `e!(p)`    → AST_CALL { callee=AST_MEMBER_GET(e, "emit"), args=[p] }
 * `e!(x,y,z)`→ PARSE_EMIT_MULTI_ARG_V1 error
 * `!x`       → AST_UNARY { op=UOP_NOT, operand=x }  (prefix, unaffected)
 */

#include "utest.h"

#include <string.h>

#include "uarena.h"
#include "uast.h"
#include "ulex.h"
#include "uparse.h"

#define UTEST(name) static void name(void)

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

typedef struct {
    ULexer  lex;
    UArena  arena;
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

/* Return the method name from an AST_CALL node whose callee is AST_MEMBER_GET. */
static const char *call_member_name(const UAstNode *n) {
    if (n == NULL || n->kind != AST_CALL) return NULL;
    const UAstNode *callee = n->u.call.callee;
    if (callee == NULL || callee->kind != AST_MEMBER_GET) return NULL;
    return callee->u.member.name_start;  /* zero-copy; null-terminated only
                                          * if the source had a NUL; use len */
}

static int call_member_name_len(const UAstNode *n) {
    if (n == NULL || n->kind != AST_CALL) return -1;
    const UAstNode *callee = n->u.call.callee;
    if (callee == NULL || callee->kind != AST_MEMBER_GET) return -1;
    return callee->u.member.name_len;
}

/* -----------------------------------------------------------------------
 * T46 tests
 * ----------------------------------------------------------------------- */

/* `e!`  →  AST_CALL with callee = AST_MEMBER_GET(e, "emit"), 0 args */
UTEST(postfix_bang_desugars_to_emit_call_no_args) {
    ParseCtx c;
    ctx_init(&c, "e!");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_CALL, n->kind);
    UASSERT_EQ(0, n->u.call.arg_count);
    /* Callee must be AST_MEMBER_GET with name "emit" */
    UASSERT(n->u.call.callee != NULL);
    UASSERT_EQ(AST_MEMBER_GET, n->u.call.callee->kind);
    UASSERT_EQ(4, call_member_name_len(n));
    UASSERT(strncmp(call_member_name(n), "emit", 4) == 0);
    ctx_destroy(&c);
}

/* `e!(p)`  →  AST_CALL, 1 arg */
UTEST(postfix_bang_with_arg_desugars_with_arg) {
    ParseCtx c;
    ctx_init(&c, "e!(p)");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_CALL, n->kind);
    UASSERT_EQ(1, n->u.call.arg_count);
    UASSERT_EQ(4, call_member_name_len(n));
    UASSERT(strncmp(call_member_name(n), "emit", 4) == 0);
    ctx_destroy(&c);
}

/* `e!(x, y, z)`  →  PARSE_EMIT_MULTI_ARG_V1 */
UTEST(postfix_bang_multi_arg_errors) {
    ParseCtx c;
    ctx_init(&c, "e!(x, y, z)");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_ERROR, n->kind);
    UASSERT_EQ(PARSE_EMIT_MULTI_ARG_V1, (UParseError)n->u.err.code);
    ctx_destroy(&c);
}

/* `!x`  →  AST_UNARY { op=UOP_NOT }  (prefix logical NOT) */
UTEST(prefix_bang_remains_logical_not) {
    ParseCtx c;
    ctx_init(&c, "!x");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_UNARY, n->kind);
    UASSERT_EQ(UOP_NOT, n->u.unary.op);
    ctx_destroy(&c);
}

/* `e!` in IDENT fast-path (parse_statement_or_expr ident branch) */
UTEST(postfix_bang_ident_fastpath_no_args) {
    ParseCtx c;
    /* parse_statement_or_expr ident fast-path triggers on bare ident at stmt level */
    ctx_init(&c, "myEvent!");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_CALL, n->kind);
    UASSERT_EQ(0, n->u.call.arg_count);
    UASSERT_EQ(4, call_member_name_len(n));
    UASSERT(strncmp(call_member_name(n), "emit", 4) == 0);
    ctx_destroy(&c);
}

/* `myEvent!(payload)` in IDENT fast-path */
UTEST(postfix_bang_ident_fastpath_with_arg) {
    ParseCtx c;
    ctx_init(&c, "myEvent!(42)");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_CALL, n->kind);
    UASSERT_EQ(1, n->u.call.arg_count);
    ctx_destroy(&c);
}

/* `myEvent!(x, y)` in IDENT fast-path  →  error */
UTEST(postfix_bang_ident_fastpath_multi_arg_errors) {
    ParseCtx c;
    ctx_init(&c, "myEvent!(x, y)");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_ERROR, n->kind);
    UASSERT_EQ(PARSE_EMIT_MULTI_ARG_V1, (UParseError)n->u.err.code);
    ctx_destroy(&c);
}

/* -----------------------------------------------------------------------
 * Suite entry point
 * ----------------------------------------------------------------------- */

void test_parse_emit_postfix_suite(void) {
    utest_run("postfix_bang_desugars_to_emit_call_no_args",
              postfix_bang_desugars_to_emit_call_no_args);
    utest_run("postfix_bang_with_arg_desugars_with_arg",
              postfix_bang_with_arg_desugars_with_arg);
    utest_run("postfix_bang_multi_arg_errors",
              postfix_bang_multi_arg_errors);
    utest_run("prefix_bang_remains_logical_not",
              prefix_bang_remains_logical_not);
    utest_run("postfix_bang_ident_fastpath_no_args",
              postfix_bang_ident_fastpath_no_args);
    utest_run("postfix_bang_ident_fastpath_with_arg",
              postfix_bang_ident_fastpath_with_arg);
    utest_run("postfix_bang_ident_fastpath_multi_arg_errors",
              postfix_bang_ident_fastpath_multi_arg_errors);
}
