/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for the streaming Pratt parser. */

#include "utest.h"
#include "value/uarena.h"
#include "parse/uast.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* --- S-expression dump helper.  Bounds-guarded; truncates on overflow. --- */

static void dump_rec(const UAstNode *n, char **cur, char *end) {
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
                         uparse_error_name((UParseError)n->u.err.code));
        return;
    }
}

static void ast_dump(const UAstNode *n, char *buf, size_t bufsz) {
    if (bufsz == 0) return;
    char *cur = buf;
    char *end = buf + bufsz - 1; /* leave room for terminator */
    dump_rec(n, &cur, end);
    if (cur > end) cur = end;
    *cur = '\0';
}

/* --- Convenience: parse a source string, return the first statement. --- */

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

/* --- Scaffolding tests. --- */

UTEST(parse_ast_dump_int_smoke) {
    UAstNode n = { AST_INT, 1, 1, { .i = 42 } };
    char buf[32];
    ast_dump(&n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "42");
}

UTEST(parse_empty_input_returns_null) {
    ParseCtx c;
    ctx_init(&c, "");
    UAstNode *n = uparse_next_statement(&c.p);
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
    UASSERT_STR_EQ(uparse_error_name((UParseError)999),      "PARSE_UNKNOWN");
    UASSERT_STR_EQ(uparse_error_name((UParseError)-1),       "PARSE_UNKNOWN");
}

/* --- Atom happy-path tests. --- */

UTEST(parse_atom_int) {
    ParseCtx c;
    ctx_init(&c, "42");
    UAstNode *n = uparse_next_statement(&c.p);
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
    UAstNode *n = uparse_next_statement(&c.p);
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
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "42");
    ctx_destroy(&c);
}

UTEST(parse_unary_neg) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "-42");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(- 42)");
    ctx_destroy(&c);
}

UTEST(parse_unary_pos_is_noop) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "+42");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "42");
    ctx_destroy(&c);
}

UTEST(parse_unary_double_neg_right_assoc) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "--3");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(- (- 3))");
    ctx_destroy(&c);
}

UTEST(parse_unary_neg_ident) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "-x");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(- x)");
    ctx_destroy(&c);
}

UTEST(parse_binary_add) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "1 + 2");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(+ 1 2)");
    ctx_destroy(&c);
}

UTEST(parse_precedence_add_mul) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "1 + 2 * 3");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(+ 1 (* 2 3))");
    ctx_destroy(&c);
}

UTEST(parse_precedence_mul_add) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "1 * 2 + 3");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(+ (* 1 2) 3)");
    ctx_destroy(&c);
}

UTEST(parse_left_assoc_sub) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "1 - 2 - 3");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(- (- 1 2) 3)");
    ctx_destroy(&c);
}

UTEST(parse_left_assoc_div) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "8 / 4 / 2");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(/ (/ 8 4) 2)");
    ctx_destroy(&c);
}

UTEST(parse_parens_override_precedence) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "(1 + 2) * 3");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(* (+ 1 2) 3)");
    ctx_destroy(&c);
}

UTEST(parse_unary_binds_tighter_than_binary) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "-1 + 2");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(+ (- 1) 2)");
    ctx_destroy(&c);
}

UTEST(parse_unary_on_parenthesized) {
    char buf[64];
    ParseCtx c;
    ctx_init(&c, "-(1 + 2)");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    ast_dump(n, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "(- (+ 1 2))");
    ctx_destroy(&c);
}

UTEST(parse_trailing_pipe_consumed) {
    ParseCtx c;
    ctx_init(&c, "42 |");
    UAstNode *a = uparse_next_statement(&c.p);
    UASSERT(a != NULL);
    UASSERT_EQ(a->kind, AST_INT);
    /* After consuming '|', next call sees EOF. */
    UAstNode *b = uparse_next_statement(&c.p);
    UASSERT(b == NULL);
    ctx_destroy(&c);
}

UTEST(parse_trailing_pipe_optional) {
    ParseCtx c;
    ctx_init(&c, "42");
    UAstNode *a = uparse_next_statement(&c.p);
    UASSERT(a != NULL);
    UASSERT_EQ(a->kind, AST_INT);
    UAstNode *b = uparse_next_statement(&c.p);
    UASSERT(b == NULL);
    ctx_destroy(&c);
}

UTEST(parse_trailing_tokens_without_separator_rejected) {
    /* "1 2 3" is three integer literals with no '|' between them.
       The parser must reject the unexpected trailing token(s), not
       silently swallow them. Panic-mode recovery consumes through
       the next '|' or EOF — with no '|' in the input, the whole
       remaining stream is consumed, so the first call returns an
       error node and the second returns NULL (EOF). */
    ParseCtx c;
    ctx_init(&c, "1 2 3");
    UAstNode *a = uparse_next_statement(&c.p);
    UASSERT(a != NULL);
    UASSERT_EQ(a->kind, AST_ERROR);
    UAstNode *b = uparse_next_statement(&c.p);
    UASSERT(b == NULL);
    ctx_destroy(&c);
}

UTEST(parse_pipe_inner_tier_single_statement) {
    /* '|' is now an inner-tier separator: "1 + 2 | 3 * 4 |" is one statement
       (AST_BIN_SEP) followed by a trailing '|' that is consumed as the
       REPL statement-boundary marker, leaving EOF. */
    ParseCtx c;
    ctx_init(&c, "1 + 2 | 3 * 4 |");
    UAstNode *a = uparse_next_statement(&c.p);
    UASSERT(a != NULL);
    UASSERT_EQ(a->kind, AST_BIN_SEP);
    UASSERT_EQ(a->u.bin_sep.separator, SEP_PIPE);
    UASSERT_EQ(a->u.bin_sep.lhs->kind, AST_BINARY);
    UASSERT_EQ(a->u.bin_sep.rhs->kind, AST_BINARY);
    UAstNode *eof = uparse_next_statement(&c.p);
    UASSERT(eof == NULL);
    ctx_destroy(&c);
}

UTEST(parse_pipe_no_trailing_pipe_is_one_stmt) {
    /* "1 | 2" — inner-tier '|'; one statement AST_BIN_SEP(1, 2). */
    ParseCtx c;
    ctx_init(&c, "1 | 2");
    UAstNode *a = uparse_next_statement(&c.p);
    UASSERT(a != NULL);
    UASSERT_EQ(a->kind, AST_BIN_SEP);
    UASSERT_EQ(a->u.bin_sep.separator, SEP_PIPE);
    UASSERT_EQ(a->u.bin_sep.lhs->kind, AST_INT);
    UASSERT_EQ(a->u.bin_sep.rhs->kind, AST_INT);
    UAstNode *eof = uparse_next_statement(&c.p);
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
    UAstNode *n = uparse_next_statement(&c.p);
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
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_ERROR);
    UASSERT_EQ(n->u.err.code, PARSE_EXPECTED_EXPRESSION);
    ctx_destroy(&c);
}

UTEST(parse_error_expected_rparen) {
    ParseCtx c;
    ctx_init(&c, "(1 + 2");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_ERROR);
    UASSERT_EQ(n->u.err.code, PARSE_EXPECTED_RPAREN);
    ctx_destroy(&c);
}

UTEST(parse_error_unexpected_token_at_statement_boundary) {
    ParseCtx c;
    ctx_init(&c, "1 2");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_ERROR);
    UASSERT_EQ(n->u.err.code, PARSE_UNEXPECTED_TOKEN);
    ctx_destroy(&c);
}

UTEST(parse_error_lex_error_passthrough) {
    ParseCtx c;
    ctx_init(&c, "@");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_ERROR);
    UASSERT_EQ(n->u.err.code, PARSE_LEX_ERROR);
    ctx_destroy(&c);
}

UTEST(parse_error_pipe_alone) {
    ParseCtx c;
    ctx_init(&c, "|");
    UAstNode *n = uparse_next_statement(&c.p);
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
    UAstNode *err = uparse_next_statement(&c.p);
    UASSERT(err != NULL);
    UASSERT_EQ(err->kind, AST_ERROR);
    UAstNode *ok = uparse_next_statement(&c.p);
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
    UAstNode *err = uparse_next_statement(&c.p);
    UASSERT(err != NULL);
    UASSERT_EQ(err->kind, AST_ERROR);
    UASSERT_EQ(err->u.err.code, PARSE_LEX_ERROR);
    UAstNode *ok = uparse_next_statement(&c.p);
    UASSERT(ok != NULL);
    ast_dump(ok, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "42");
    UASSERT(uparse_next_statement(&c.p) == NULL);
    ctx_destroy(&c);
}

UTEST(parse_recovery_consecutive_errors_terminate) {
    ParseCtx c;
    ctx_init(&c, "+ | + |");
    UAstNode *a = uparse_next_statement(&c.p);
    UASSERT(a != NULL);
    UASSERT_EQ(a->kind, AST_ERROR);
    UAstNode *b = uparse_next_statement(&c.p);
    UASSERT(b != NULL);
    UASSERT_EQ(b->kind, AST_ERROR);
    UASSERT(uparse_next_statement(&c.p) == NULL);
    ctx_destroy(&c);
}

/* Allocator that fails after N successful calls.  fail_at = N means
   the first N calls succeed and call N+1 fails.  (Post-increment the
   counter, compare >= fail_at.) */
typedef struct { int calls; int fail_at; } OomSpy;

static void *oom_alloc(size_t n, void *ud) {
    OomSpy *s = ud;
    if (s->calls++ >= s->fail_at) return NULL;
    void *p = malloc(n);
    return p;
}

static void oom_free(void *p, void *ud) {
    (void)ud;
    free(p);
}

UTEST(parse_oom_returns_sentinel_and_sticks) {
    OomSpy s = { 0, 0 }; /* fail on the very first backing alloc */
    ULexer lex;
    UArena arena;
    UParser p;
    const char *src = "1 + 2";
    ulex_init(&lex, src, strlen(src));
    uarena_init_ex(&arena, 0, oom_alloc, oom_free, &s);
    uparse_init(&p, &lex, &arena);

    UAstNode *n1 = uparse_next_statement(&p);
    UASSERT(n1 != NULL);
    UASSERT_EQ(n1->kind, AST_ERROR);
    UASSERT_EQ(n1->u.err.code, PARSE_OOM);

    /* Sticky: next call still returns the sentinel. */
    UAstNode *n2 = uparse_next_statement(&p);
    UASSERT(n2 != NULL);
    UASSERT_EQ(n2->kind, AST_ERROR);
    UASSERT_EQ(n2->u.err.code, PARSE_OOM);
    /* Same sentinel — pointer equality. */
    UASSERT(n1 == n2);

    uarena_destroy(&arena);
}

UTEST(parse_oom_mid_expression) {
    /* Permit a few allocations, then fail partway through a compound
       expression.  The pluggable allocator counts chunk allocations; a
       chunk holds many AST nodes, so we fail the first chunk alloc. */
    OomSpy s = { 0, 0 };
    ULexer lex;
    UArena arena;
    UParser p;
    const char *src = "1 + 2 * (3 - 4)";
    ulex_init(&lex, src, strlen(src));
    uarena_init_ex(&arena, 0, oom_alloc, oom_free, &s);
    uparse_init(&p, &lex, &arena);

    UAstNode *n = uparse_next_statement(&p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_ERROR);
    UASSERT_EQ(n->u.err.code, PARSE_OOM);

    uarena_destroy(&arena);
}

UTEST(parse_syncline_int_line_col) {
    ParseCtx c;
    ctx_init(&c, "  42");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_INT);
    UASSERT_EQ(n->line, 1);
    UASSERT_EQ(n->col, 3);
    ctx_destroy(&c);
}

UTEST(parse_syncline_binary_points_at_operator) {
    ParseCtx c;
    ctx_init(&c, "1\n+ 2");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_BINARY);
    /* '+' is on line 2, column 1. */
    UASSERT_EQ(n->line, 2);
    UASSERT_EQ(n->col, 1);
    ctx_destroy(&c);
}

UTEST(parse_syncline_unary_points_at_sign) {
    ParseCtx c;
    ctx_init(&c, "\n  -3");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_UNARY);
    /* '-' is on line 2, column 3. */
    UASSERT_EQ(n->line, 2);
    UASSERT_EQ(n->col, 3);
    ctx_destroy(&c);
}

UTEST(parse_syncline_error_at_detection_point) {
    ParseCtx c;
    ctx_init(&c, "1 +\n)"); /* ')' on line 2, col 1 */
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(n->kind, AST_ERROR);
    UASSERT_EQ(n->u.err.code, PARSE_EXPECTED_EXPRESSION);
    UASSERT_EQ(n->line, 2);
    UASSERT_EQ(n->col, 1);
    ctx_destroy(&c);
}

/* --- var-decl and assign parse tests (T10) --- */

UTEST(parse_var_decl_basic) {
    /* "var x = 7" -> AST_VAR_DECL, name="x", init=AST_INT(7) */
    ParseCtx c;
    ctx_init(&c, "var x = 7");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_VAR_DECL, (int)n->kind);
    UASSERT_EQ(1, n->u.var_decl.name_len);
    UASSERT_EQ('x', n->u.var_decl.name_start[0]);
    UASSERT(n->u.var_decl.init != NULL);
    UASSERT_EQ((int)AST_INT, (int)n->u.var_decl.init->kind);
    UASSERT_EQ((int64_t)7, n->u.var_decl.init->u.i);
    ctx_destroy(&c);
}

UTEST(parse_var_decl_requires_init) {
    /* "var x" (no '=') -> AST_ERROR PARSE_EXPECTED_EQ */
    ParseCtx c;
    ctx_init(&c, "var x");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_ERROR, (int)n->kind);
    UASSERT_EQ((int)PARSE_EXPECTED_EQ, n->u.err.code);
    ctx_destroy(&c);
}

UTEST(parse_var_decl_requires_ident) {
    /* "var = 7" (no name) -> AST_ERROR PARSE_EXPECTED_IDENT */
    ParseCtx c;
    ctx_init(&c, "var = 7");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_ERROR, (int)n->kind);
    UASSERT_EQ((int)PARSE_EXPECTED_IDENT, n->u.err.code);
    ctx_destroy(&c);
}

UTEST(parse_assign_basic) {
    /* "x = 42" -> AST_ASSIGN, name="x", value=AST_INT(42) */
    ParseCtx c;
    ctx_init(&c, "x = 42");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_ASSIGN, (int)n->kind);
    UASSERT_EQ(1, n->u.assign.name_len);
    UASSERT_EQ('x', n->u.assign.name_start[0]);
    UASSERT(n->u.assign.value != NULL);
    UASSERT_EQ((int)AST_INT, (int)n->u.assign.value->kind);
    UASSERT_EQ((int64_t)42, n->u.assign.value->u.i);
    ctx_destroy(&c);
}

UTEST(parse_ident_not_followed_by_eq_is_expression) {
    /* "x + 1" should NOT be parsed as an assign — remains AST_BINARY */
    ParseCtx c;
    ctx_init(&c, "x + 1");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_BINARY, (int)n->kind);
    UASSERT_EQ((int)BOP_ADD, (int)n->u.binary.op);
    UASSERT_EQ((int)AST_IDENT, (int)n->u.binary.lhs->kind);
    ctx_destroy(&c);
}

/* --- Comparison operators --- */

UTEST(parse_eqeq) {
    ParseCtx c;
    ctx_init(&c, "1 == 2");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_COMPARE, (int)n->kind);
    UASSERT_EQ((int)CMP_EQ, (int)n->u.cmp.op);
    UASSERT(n->u.cmp.lhs != NULL && n->u.cmp.lhs->kind == AST_INT);
    UASSERT(n->u.cmp.rhs != NULL && n->u.cmp.rhs->kind == AST_INT);
    ctx_destroy(&c);
}

UTEST(parse_neq) {
    ParseCtx c;
    ctx_init(&c, "1 != 2");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_COMPARE, (int)n->kind);
    UASSERT_EQ((int)CMP_NEQ, (int)n->u.cmp.op);
    ctx_destroy(&c);
}

UTEST(parse_lt) {
    ParseCtx c;
    ctx_init(&c, "1 < 2");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_COMPARE, (int)n->kind);
    UASSERT_EQ((int)CMP_LT, (int)n->u.cmp.op);
    ctx_destroy(&c);
}

UTEST(parse_le) {
    ParseCtx c;
    ctx_init(&c, "1 <= 2");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_COMPARE, (int)n->kind);
    UASSERT_EQ((int)CMP_LE, (int)n->u.cmp.op);
    ctx_destroy(&c);
}

UTEST(parse_gt) {
    ParseCtx c;
    ctx_init(&c, "1 > 2");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_COMPARE, (int)n->kind);
    UASSERT_EQ((int)CMP_GT, (int)n->u.cmp.op);
    ctx_destroy(&c);
}

UTEST(parse_ge) {
    ParseCtx c;
    ctx_init(&c, "1 >= 2");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_COMPARE, (int)n->kind);
    UASSERT_EQ((int)CMP_GE, (int)n->u.cmp.op);
    ctx_destroy(&c);
}

UTEST(parse_compare_binds_looser_than_add) {
    /* "1 + 2 == 3" → COMPARE(BINARY(+, 1, 2), 3) not BINARY(+, 1, COMPARE(2, 3)) */
    ParseCtx c;
    ctx_init(&c, "1 + 2 == 3");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_COMPARE, (int)n->kind);
    UASSERT_EQ((int)CMP_EQ, (int)n->u.cmp.op);
    UASSERT(n->u.cmp.lhs != NULL && n->u.cmp.lhs->kind == AST_BINARY);
    UASSERT(n->u.cmp.rhs != NULL && n->u.cmp.rhs->kind == AST_INT);
    ctx_destroy(&c);
}

/* --- Boolean and nil literals --- */

UTEST(parse_true_literal) {
    ParseCtx c;
    ctx_init(&c, "true");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_BOOL, (int)n->kind);
    UASSERT(n->u.b == true);
    ctx_destroy(&c);
}

UTEST(parse_false_literal) {
    ParseCtx c;
    ctx_init(&c, "false");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_BOOL, (int)n->kind);
    UASSERT(n->u.b == false);
    ctx_destroy(&c);
}

UTEST(parse_nil_literal) {
    ParseCtx c;
    ctx_init(&c, "nil");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_NIL, (int)n->kind);
    ctx_destroy(&c);
}

/* --- if / else --- */

UTEST(parse_if_then) {
    /* "if (1 < 2) { 42 }" → AST_IF with cond=AST_COMPARE, then=AST_BLOCK,
       else_block=NULL */
    ParseCtx c;
    ctx_init(&c, "if (1 < 2) { 42 }");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_IF, (int)n->kind);
    UASSERT(n->u.if_stmt.cond != NULL);
    UASSERT_EQ((int)AST_COMPARE, (int)n->u.if_stmt.cond->kind);
    UASSERT_EQ((int)CMP_LT, (int)n->u.if_stmt.cond->u.cmp.op);
    UASSERT(n->u.if_stmt.then_block != NULL);
    UASSERT_EQ((int)AST_BLOCK, (int)n->u.if_stmt.then_block->kind);
    UASSERT_EQ(1, n->u.if_stmt.then_block->u.block.count);
    UASSERT(n->u.if_stmt.else_block == NULL);
    ctx_destroy(&c);
}

UTEST(parse_if_then_else) {
    /* "if (1 < 2) { 42 } else { 99 }" → AST_IF with else_block != NULL */
    ParseCtx c;
    ctx_init(&c, "if (1 < 2) { 42 } else { 99 }");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_IF, (int)n->kind);
    UASSERT(n->u.if_stmt.cond != NULL);
    UASSERT(n->u.if_stmt.then_block != NULL);
    UASSERT_EQ((int)AST_BLOCK, (int)n->u.if_stmt.then_block->kind);
    UASSERT(n->u.if_stmt.else_block != NULL);
    UASSERT_EQ((int)AST_BLOCK, (int)n->u.if_stmt.else_block->kind);
    UASSERT_EQ(1, n->u.if_stmt.else_block->u.block.count);
    UASSERT_EQ((int)AST_INT, (int)n->u.if_stmt.else_block->u.block.stmts[0]->kind);
    UASSERT_EQ((int64_t)99, n->u.if_stmt.else_block->u.block.stmts[0]->u.i);
    ctx_destroy(&c);
}

UTEST(parse_if_no_paren_is_error) {
    /* "if 1 < 2 { 42 }" → AST_ERROR PARSE_EXPECTED_LPAREN */
    ParseCtx c;
    ctx_init(&c, "if 1 < 2 { 42 }");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_ERROR, (int)n->kind);
    UASSERT_EQ((int)PARSE_EXPECTED_LPAREN, n->u.err.code);
    ctx_destroy(&c);
}

UTEST(parse_while_basic) {
    /* "while (1 < 10) { 42 }" → AST_WHILE with cond=AST_COMPARE, body=AST_BLOCK */
    ParseCtx c;
    ctx_init(&c, "while (1 < 10) { 42 }");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_WHILE, (int)n->kind);
    UASSERT(n->u.while_stmt.cond != NULL);
    UASSERT_EQ((int)AST_COMPARE, (int)n->u.while_stmt.cond->kind);
    UASSERT_EQ((int)CMP_LT, (int)n->u.while_stmt.cond->u.cmp.op);
    UASSERT(n->u.while_stmt.body != NULL);
    UASSERT_EQ((int)AST_BLOCK, (int)n->u.while_stmt.body->kind);
    UASSERT_EQ(1, n->u.while_stmt.body->u.block.count);
    ctx_destroy(&c);
}

UTEST(parse_while_no_paren_is_error) {
    /* "while 1 < 10 { 42 }" → AST_ERROR PARSE_EXPECTED_LPAREN */
    ParseCtx c;
    ctx_init(&c, "while 1 < 10 { 42 }");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_ERROR, (int)n->kind);
    UASSERT_EQ((int)PARSE_EXPECTED_LPAREN, n->u.err.code);
    ctx_destroy(&c);
}

UTEST(parse_bare_function_no_name_errors) {
    ParseCtx c;
    ctx_init(&c, "function { 1 }");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_ERROR, (int)n->kind);
    UASSERT_EQ((int)PARSE_BARE_FUNCTION, (int)n->u.err.code);
    UASSERT(strstr(n->u.err.message, "add empty parens") != NULL);
    ctx_destroy(&c);
}

UTEST(parse_bare_function_with_name_errors) {
    ParseCtx c;
    ctx_init(&c, "function foo { 1 }");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_ERROR, (int)n->kind);
    UASSERT_EQ((int)PARSE_BARE_FUNCTION, (int)n->u.err.code);
    ctx_destroy(&c);
}

UTEST(parse_closure_keyword_errors) {
    ParseCtx c;
    ctx_init(&c, "closure(x) { x + 1 }");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_ERROR, (int)n->kind);
    UASSERT_EQ((int)PARSE_CLOSURE_KEYWORD, (int)n->u.err.code);
    UASSERT(strstr(n->u.err.message, "MIGRATION TRAP") != NULL);
    ctx_destroy(&c);
}

/* T10 — try/catch/finally + throw parser tests. */

UTEST(parse_try_finally_basic) {
    /* "try { 42 } finally { 1 }" → AST_TRY, no catch, has finally_body */
    ParseCtx c;
    ctx_init(&c, "try { 42 } finally { 1 }");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_TRY, (int)n->kind);
    UASSERT(n->u.try_stmt.body != NULL);
    UASSERT_EQ((int)AST_BLOCK, (int)n->u.try_stmt.body->kind);
    UASSERT(n->u.try_stmt.catch_body == NULL);
    UASSERT(n->u.try_stmt.catch_var_start == NULL);
    UASSERT(n->u.try_stmt.finally_body != NULL);
    UASSERT_EQ((int)AST_BLOCK, (int)n->u.try_stmt.finally_body->kind);
    ctx_destroy(&c);
}

UTEST(parse_try_catch_finally_full) {
    /* "try { 1 } catch (e) { 2 } finally { 3 }" → both catch and finally */
    ParseCtx c;
    ctx_init(&c, "try { 1 } catch (e) { 2 } finally { 3 }");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_TRY, (int)n->kind);
    UASSERT(n->u.try_stmt.body != NULL);
    UASSERT(n->u.try_stmt.catch_body != NULL);
    UASSERT(n->u.try_stmt.catch_var_start != NULL);
    UASSERT_EQ(1, n->u.try_stmt.catch_var_len);  /* "e" */
    UASSERT_EQ('e', n->u.try_stmt.catch_var_start[0]);
    UASSERT(n->u.try_stmt.finally_body != NULL);
    ctx_destroy(&c);
}

UTEST(parse_try_no_catch_no_finally_is_error) {
    /* "try { 1 }" with neither catch nor finally → PARSE_TRY_NEEDS_CATCH_OR_FINALLY */
    ParseCtx c;
    ctx_init(&c, "try { 1 }");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_ERROR, (int)n->kind);
    UASSERT_EQ((int)PARSE_TRY_NEEDS_CATCH_OR_FINALLY, n->u.err.code);
    ctx_destroy(&c);
}

UTEST(parse_throw_basic) {
    /* "throw 99" → AST_THROW with AST_INT value */
    ParseCtx c;
    ctx_init(&c, "throw 99");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_THROW, (int)n->kind);
    UASSERT(n->u.throw_expr.value != NULL);
    UASSERT_EQ((int)AST_INT, (int)n->u.throw_expr.value->kind);
    UASSERT_EQ((int64_t)99, n->u.throw_expr.value->u.i);
    ctx_destroy(&c);
}

/* T11 — tag-prefix scope parser tests. */

UTEST(parse_tag_prefix_basic) {
    /* "mytag: { 1 }" → AST_TAG_PREFIX with tag_expr=AST_IDENT("mytag"),
     * body=AST_BLOCK with one statement, onleave=NULL. */
    ParseCtx c;
    ctx_init(&c, "mytag: { 1 }");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_TAG_PREFIX, (int)n->kind);
    /* tag_expr: AST_IDENT "mytag" */
    UASSERT(n->u.tag_prefix.tag_expr != NULL);
    UASSERT_EQ((int)AST_IDENT, (int)n->u.tag_prefix.tag_expr->kind);
    UASSERT_EQ(5, n->u.tag_prefix.tag_expr->u.ident.len);
    UASSERT(n->u.tag_prefix.tag_expr->u.ident.start[0] == 'm');
    /* body: AST_BLOCK with one statement */
    UASSERT(n->u.tag_prefix.body != NULL);
    UASSERT_EQ((int)AST_BLOCK, (int)n->u.tag_prefix.body->kind);
    UASSERT_EQ(1, n->u.tag_prefix.body->u.block.count);
    UASSERT_EQ((int)AST_INT, (int)n->u.tag_prefix.body->u.block.stmts[0]->kind);
    UASSERT_EQ((int64_t)1, n->u.tag_prefix.body->u.block.stmts[0]->u.i);
    /* onleave: NULL at M3 */
    UASSERT(n->u.tag_prefix.onleave == NULL);
    ctx_destroy(&c);
}

UTEST(parse_tag_prefix_then_normal_stmt) {
    /* "mytag: { 1 } | 42" — tag-prefix as first statement of outer-tier.
     * After the tag-prefix the `|` separator is consumed; next statement is 42. */
    ParseCtx c;
    ctx_init(&c, "mytag: { 1 } | 42");
    /* First statement: the tag-prefix. */
    UAstNode *first = uparse_next_statement(&c.p);
    UASSERT(first != NULL);
    UASSERT_EQ((int)AST_TAG_PREFIX, (int)first->kind);
    /* Second statement: the integer 42. */
    UAstNode *second = uparse_next_statement(&c.p);
    UASSERT(second != NULL);
    UASSERT_EQ((int)AST_INT, (int)second->kind);
    UASSERT_EQ((int64_t)42, second->u.i);
    ctx_destroy(&c);
}

UTEST(parse_tag_prefix_empty_body) {
    /* "t: { }" — tag-prefix with an empty block body. */
    ParseCtx c;
    ctx_init(&c, "t: { }");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_TAG_PREFIX, (int)n->kind);
    UASSERT(n->u.tag_prefix.tag_expr != NULL);
    UASSERT_EQ((int)AST_IDENT, (int)n->u.tag_prefix.tag_expr->kind);
    UASSERT_EQ(1, n->u.tag_prefix.tag_expr->u.ident.len);
    UASSERT(n->u.tag_prefix.body != NULL);
    UASSERT_EQ((int)AST_BLOCK, (int)n->u.tag_prefix.body->kind);
    UASSERT_EQ(0, n->u.tag_prefix.body->u.block.count);
    UASSERT(n->u.tag_prefix.onleave == NULL);
    ctx_destroy(&c);
}

/* --- T19 — slot member access (obj.x, obj.x = v) and slot-property
       access (obj.x->prop, obj.x->prop = v). --- */

UTEST(parse_member_get_basic) {
    /* "obj.x" — postfix dot yields AST_MEMBER_GET with recv=ident(obj),
       name=x, value=NULL. */
    ParseCtx c;
    ctx_init(&c, "obj.x");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_MEMBER_GET, (int)n->kind);
    UASSERT(n->u.member.recv != NULL);
    UASSERT_EQ((int)AST_IDENT, (int)n->u.member.recv->kind);
    UASSERT_EQ(3, n->u.member.recv->u.ident.len);
    UASSERT_EQ(1, n->u.member.name_len);
    UASSERT_EQ('x', n->u.member.name_start[0]);
    UASSERT(n->u.member.value == NULL);
    ctx_destroy(&c);
}

UTEST(parse_member_set_basic) {
    /* "obj.x = 42" — yields AST_MEMBER_SET with value=AST_INT(42). */
    ParseCtx c;
    ctx_init(&c, "obj.x = 42");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_MEMBER_SET, (int)n->kind);
    UASSERT(n->u.member.recv != NULL);
    UASSERT_EQ((int)AST_IDENT, (int)n->u.member.recv->kind);
    UASSERT_EQ(1, n->u.member.name_len);
    UASSERT_EQ('x', n->u.member.name_start[0]);
    UASSERT(n->u.member.value != NULL);
    UASSERT_EQ((int)AST_INT, (int)n->u.member.value->kind);
    UASSERT_EQ(42, (int)n->u.member.value->u.i);
    ctx_destroy(&c);
}

UTEST(parse_prop_get_basic) {
    /* "obj.x->prop" — yields AST_PROP_GET wrapping an AST_MEMBER_GET. */
    ParseCtx c;
    ctx_init(&c, "obj.x->prop");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_PROP_GET, (int)n->kind);
    UASSERT(n->u.prop.recv != NULL);
    UASSERT_EQ((int)AST_MEMBER_GET, (int)n->u.prop.recv->kind);
    UASSERT_EQ(4, n->u.prop.prop_name_len);
    UASSERT_EQ('p', n->u.prop.prop_name_start[0]);
    UASSERT(n->u.prop.value == NULL);
    ctx_destroy(&c);
}

UTEST(parse_prop_set_basic) {
    /* "obj.x->prop = 1" — yields AST_PROP_SET with value=AST_INT(1). */
    ParseCtx c;
    ctx_init(&c, "obj.x->prop = 1");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_PROP_SET, (int)n->kind);
    UASSERT(n->u.prop.recv != NULL);
    UASSERT_EQ((int)AST_MEMBER_GET, (int)n->u.prop.recv->kind);
    UASSERT_EQ(4, n->u.prop.prop_name_len);
    UASSERT(n->u.prop.value != NULL);
    UASSERT_EQ((int)AST_INT, (int)n->u.prop.value->kind);
    UASSERT_EQ(1, (int)n->u.prop.value->u.i);
    ctx_destroy(&c);
}

UTEST(parse_method_call_preserved) {
    /* "obj.method()" — dot followed by '(' must still build an AST_CALL
       (callee = AST_MEMBER_GET).  This guards the existing method-call
       syntax against the new MEMBER_GET path. */
    ParseCtx c;
    ctx_init(&c, "obj.method()");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_CALL, (int)n->kind);
    UASSERT_EQ(0, n->u.call.arg_count);
    UASSERT(n->u.call.callee != NULL);
    UASSERT_EQ((int)AST_MEMBER_GET, (int)n->u.call.callee->kind);
    UASSERT_EQ(6, n->u.call.callee->u.member.name_len);
    UASSERT_EQ('m', n->u.call.callee->u.member.name_start[0]);
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
    utest_run("parse_trailing_tokens_without_separator_rejected",
                                                    parse_trailing_tokens_without_separator_rejected);
    utest_run("parse_pipe_inner_tier_single_statement", parse_pipe_inner_tier_single_statement);
    utest_run("parse_pipe_no_trailing_pipe_is_one_stmt", parse_pipe_no_trailing_pipe_is_one_stmt);
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
    utest_run("parse_oom_returns_sentinel_and_sticks", parse_oom_returns_sentinel_and_sticks);
    utest_run("parse_oom_mid_expression",              parse_oom_mid_expression);
    utest_run("parse_syncline_int_line_col",              parse_syncline_int_line_col);
    utest_run("parse_syncline_binary_points_at_operator", parse_syncline_binary_points_at_operator);
    utest_run("parse_syncline_unary_points_at_sign",      parse_syncline_unary_points_at_sign);
    utest_run("parse_syncline_error_at_detection_point",  parse_syncline_error_at_detection_point);
    utest_run("parse var decl: basic 'var x = 7'",        parse_var_decl_basic);
    utest_run("parse var decl: requires initializer",      parse_var_decl_requires_init);
    utest_run("parse var decl: requires identifier name",  parse_var_decl_requires_ident);
    utest_run("parse assign: basic 'x = 42'",             parse_assign_basic);
    utest_run("parse ident-not-assign: 'x + 1' is binary", parse_ident_not_followed_by_eq_is_expression);
    utest_run("parse: == produces AST_COMPARE CMP_EQ",    parse_eqeq);
    utest_run("parse: != produces AST_COMPARE CMP_NEQ",   parse_neq);
    utest_run("parse: < produces AST_COMPARE CMP_LT",     parse_lt);
    utest_run("parse: <= produces AST_COMPARE CMP_LE",    parse_le);
    utest_run("parse: > produces AST_COMPARE CMP_GT",     parse_gt);
    utest_run("parse: >= produces AST_COMPARE CMP_GE",    parse_ge);
    utest_run("parse: compare binds looser than add",     parse_compare_binds_looser_than_add);
    utest_run("parse: true literal",                      parse_true_literal);
    utest_run("parse: false literal",                     parse_false_literal);
    utest_run("parse: nil literal",                       parse_nil_literal);
    utest_run("parse: if-then produces AST_IF with AST_BLOCK then, no else",
              parse_if_then);
    utest_run("parse: if-then-else produces AST_IF with both blocks",
              parse_if_then_else);
    utest_run("parse: if without '(' is PARSE_EXPECTED_LPAREN",
              parse_if_no_paren_is_error);
    utest_run("parse: while (1 < 10) { 42 } → AST_WHILE with cond=AST_COMPARE",
              parse_while_basic);
    utest_run("parse: while without '(' is PARSE_EXPECTED_LPAREN",
              parse_while_no_paren_is_error);
    utest_run("parse: bare function 'function { 1 }' is PARSE_BARE_FUNCTION error",
              parse_bare_function_no_name_errors);
    utest_run("parse: bare function 'function foo { 1 }' is PARSE_BARE_FUNCTION error",
              parse_bare_function_with_name_errors);
    utest_run("parse: 'closure(x) { x + 1 }' is PARSE_CLOSURE_KEYWORD error",
              parse_closure_keyword_errors);
    /* T10 — try/catch/finally + throw */
    utest_run("parse: 'try { 42 } finally { 1 }' → AST_TRY with finally_body, no catch",
              parse_try_finally_basic);
    utest_run("parse: 'try { 1 } catch (e) { 2 } finally { 3 }' → both catch and finally",
              parse_try_catch_finally_full);
    utest_run("parse: 'try { 1 }' without catch/finally → PARSE_TRY_NEEDS_CATCH_OR_FINALLY",
              parse_try_no_catch_no_finally_is_error);
    utest_run("parse: 'throw 99' → AST_THROW with AST_INT value 99",
              parse_throw_basic);
    /* T11 — tag-prefix scope */
    utest_run("parse: 'mytag: { 1 }' → AST_TAG_PREFIX with ident tag_expr, AST_BLOCK body",
              parse_tag_prefix_basic);
    utest_run("parse: 'mytag: { 1 } | 42' — tag-prefix then normal statement",
              parse_tag_prefix_then_normal_stmt);
    utest_run("parse: 't: { }' — tag-prefix with empty block body",
              parse_tag_prefix_empty_body);
    /* T19 — slot member access (obj.x, obj.x = v) and slot-property
       access (obj.x->prop, obj.x->prop = v). */
    utest_run("parse: 'obj.x' → AST_MEMBER_GET",                parse_member_get_basic);
    utest_run("parse: 'obj.x = 42' → AST_MEMBER_SET",           parse_member_set_basic);
    utest_run("parse: 'obj.x->prop' → AST_PROP_GET on MEMBER_GET", parse_prop_get_basic);
    utest_run("parse: 'obj.x->prop = 1' → AST_PROP_SET on MEMBER_GET", parse_prop_set_basic);
    utest_run("parse: 'obj.method()' preserved as AST_CALL{callee=MEMBER_GET}",
              parse_method_call_preserved);
}
