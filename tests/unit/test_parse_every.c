/* SPDX-License-Identifier: BSD-3-Clause */
/* Task 5.2: parser desugar of `every (E) S`.
 *
 * Pure parser-level desugar — no new AST node kind, no new opcode.
 * The keyword `every` is consumed and re-emitted as a call to a free
 * identifier `every` with two arguments: the period expression and a
 * zero-parameter function (closure) wrapping the body statement.  The
 * runtime resolution to the stdlib C-native `every` function is the
 * responsibility of Task 5.3.
 */

#include "utest.h"

#include <string.h>

#include "lex/ulex.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "value/uarena.h"

#define UTEST(name) static void name(void)

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

/* every (100) f()  →  every(100, function() { f() })
 *
 * Root node: AST_CALL with callee=IDENT("every"), 2 args.
 *   arg[0] = AST_INT 100        (period expression)
 *   arg[1] = AST_FUNCTION       (body closure, zero params)
 */
UTEST(parse_every_desugars_to_call) {
    ParseCtx c;
    ctx_init(&c, "every (100) f()");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    if (n == NULL || n->kind != AST_CALL) {
        if (n != NULL) UASSERT_EQ(AST_CALL, n->kind);
        ctx_destroy(&c);
        return;
    }

    /* Callee is the free identifier `every` — runtime resolves it
     * to the stdlib C-native function registered by urbi_stdlib_boot. */
    UAstNode *callee = n->u.call.callee;
    UASSERT(callee != NULL);
    if (callee != NULL) {
        UASSERT_EQ(AST_IDENT, callee->kind);
        if (callee->kind == AST_IDENT) {
            UASSERT_EQ(5, callee->u.ident.len);
            UASSERT(memcmp(callee->u.ident.start, "every", 5) == 0);
        }
    }

    /* Two arguments. */
    UASSERT_EQ(2, n->u.call.arg_count);
    if (n->u.call.arg_count != 2 || n->u.call.args == NULL) {
        ctx_destroy(&c);
        return;
    }

    /* arg[0] = the period — integer literal 100. */
    UAstNode *period = n->u.call.args[0];
    UASSERT(period != NULL);
    if (period != NULL) {
        UASSERT_EQ(AST_INT, period->kind);
        if (period->kind == AST_INT) {
            UASSERT_EQ(100, period->u.i);
        }
    }

    /* arg[1] = body wrapped in a zero-param function literal. */
    UAstNode *body_fn = n->u.call.args[1];
    UASSERT(body_fn != NULL);
    if (body_fn != NULL) {
        UASSERT_EQ(AST_FUNCTION, body_fn->kind);
        if (body_fn->kind == AST_FUNCTION) {
            UASSERT_EQ(0, body_fn->u.func.param_count);
            UASSERT(body_fn->u.func.body != NULL);
        }
    }
    ctx_destroy(&c);
}

/* every (E) S accepts any statement form as the body.  `f()` body parses
 * as an AST_CALL; verify the wrapped body is the same call. */
UTEST(parse_every_body_is_user_statement) {
    ParseCtx c;
    ctx_init(&c, "every (100) f()");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    if (n == NULL || n->kind != AST_CALL || n->u.call.arg_count < 2
        || n->u.call.args == NULL) {
        if (n != NULL) UASSERT_EQ(AST_CALL, n->kind);
        ctx_destroy(&c);
        return;
    }

    UAstNode *body_fn = n->u.call.args[1];
    UASSERT(body_fn != NULL);
    if (body_fn != NULL && body_fn->kind == AST_FUNCTION) {
        /* The function body is whatever shape emit_function_literal can
         * compile (does not need to be AST_BLOCK).  For `f()` the body
         * is the call expression itself. */
        UAstNode *body = body_fn->u.func.body;
        UASSERT(body != NULL);
        if (body != NULL) {
            UASSERT_EQ(AST_CALL, body->kind);
        }
    } else {
        if (body_fn != NULL) UASSERT_EQ(AST_FUNCTION, body_fn->kind);
    }
    ctx_destroy(&c);
}

/* every accepts a brace-block body: `every (100) { a(); b() }`. */
UTEST(parse_every_with_block_body) {
    ParseCtx c;
    ctx_init(&c, "every (100) { a(); b() }");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    if (n == NULL || n->kind != AST_CALL || n->u.call.arg_count < 2
        || n->u.call.args == NULL) {
        if (n != NULL) UASSERT_EQ(AST_CALL, n->kind);
        ctx_destroy(&c);
        return;
    }
    UASSERT_EQ(2, n->u.call.arg_count);

    UAstNode *body_fn = n->u.call.args[1];
    UASSERT(body_fn != NULL);
    if (body_fn != NULL && body_fn->kind == AST_FUNCTION) {
        UASSERT(body_fn->u.func.body != NULL);
        if (body_fn->u.func.body != NULL) {
            UASSERT_EQ(AST_BLOCK, body_fn->u.func.body->kind);
        }
    } else {
        if (body_fn != NULL) UASSERT_EQ(AST_FUNCTION, body_fn->kind);
    }
    ctx_destroy(&c);
}

/* `every X` (no parens) is a syntax error — clean diagnostic, not a
 * silent reinterpret as a call to a bare identifier. */
UTEST(parse_every_without_parens_errors) {
    ParseCtx c;
    ctx_init(&c, "every 100");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_ERROR, n->kind);
    ctx_destroy(&c);
}

void test_parse_every_suite(void) {
    utest_run("parse_every_desugars_to_call",
              parse_every_desugars_to_call);
    utest_run("parse_every_body_is_user_statement",
              parse_every_body_is_user_statement);
    utest_run("parse_every_with_block_body",
              parse_every_with_block_body);
    utest_run("parse_every_without_parens_errors",
              parse_every_without_parens_errors);
}
