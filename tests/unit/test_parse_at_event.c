/* SPDX-License-Identifier: BSD-3-Clause */
/* AST_WATCHER (source=UWSRC_EVENT) parse tests.
 *
 * Covers postfix `?` recognition inside at(...).
 */

#include "utest.h"

#include <string.h>

#include "util/uarena.h"
#include "parse/uast.h"
#include "lex/ulex.h"
#include "parse/uparse.h"

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

/* -----------------------------------------------------------------------
 * T45 parse tests
 * ----------------------------------------------------------------------- */

/* at (e?) body  →  AST_WATCHER, source=UWSRC_EVENT, mode=UWATCHER_AT */
UTEST(parse_at_event_with_question_postfix) {
    ParseCtx c;
    ctx_init(&c, "at (e?) body");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_WATCHER, n->kind);
    UASSERT_EQ((int)UWSRC_EVENT, (int)n->u.watcher.source);
    UASSERT_EQ(UWATCHER_AT, n->u.watcher.mode);
    UASSERT(n->u.watcher.cond != NULL);
    UASSERT_EQ(AST_IDENT, n->u.watcher.cond->kind);
    ctx_destroy(&c);
}

/* at sync (e?) body  →  AST_WATCHER, source=UWSRC_EVENT, mode=UWATCHER_AT_SYNC */
UTEST(parse_at_sync_event) {
    ParseCtx c;
    ctx_init(&c, "at sync (e?) body");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_WATCHER, n->kind);
    UASSERT_EQ((int)UWSRC_EVENT, (int)n->u.watcher.source);
    UASSERT_EQ(UWATCHER_AT_SYNC, n->u.watcher.mode);
    ctx_destroy(&c);
}

/* at (e?) body onleave handler  →  onleave populated */
UTEST(parse_at_event_with_onleave) {
    ParseCtx c;
    ctx_init(&c, "at (e?) body onleave handler");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_WATCHER, n->kind);
    UASSERT(n->u.watcher.onleave != NULL);
    ctx_destroy(&c);
}

/* at (e?) body without onleave  →  onleave is NULL */
UTEST(parse_at_event_no_onleave) {
    ParseCtx c;
    ctx_init(&c, "at (e?) body");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_WATCHER, n->kind);
    UASSERT(n->u.watcher.onleave == NULL);
    ctx_destroy(&c);
}

/* at (cond) body (no ?) still produces AST_WATCHER */
UTEST(parse_at_no_question_still_watcher) {
    ParseCtx c;
    ctx_init(&c, "at (x > 0) body");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_WATCHER, n->kind);
    ctx_destroy(&c);
}

/* var x = e?  →  PARSE_QUESTION_OUTSIDE_AT error */
UTEST(parse_question_outside_at_errors) {
    ParseCtx c;
    ctx_init(&c, "var x = e?");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_ERROR, n->kind);
    UASSERT_EQ(PARSE_QUESTION_OUTSIDE_AT, (UParseError)n->u.err.code);
    ctx_destroy(&c);
}

/* Standalone e? in expression position  →  PARSE_QUESTION_OUTSIDE_AT */
UTEST(parse_question_outside_at_standalone) {
    ParseCtx c;
    ctx_init(&c, "e?");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_ERROR, n->kind);
    UASSERT_EQ(PARSE_QUESTION_OUTSIDE_AT, (UParseError)n->u.err.code);
    ctx_destroy(&c);
}

/* -----------------------------------------------------------------------
 * Suite entry point
 * ----------------------------------------------------------------------- */

void test_parse_at_event_suite(void) {
    utest_run("parse_at_event_with_question_postfix",
              parse_at_event_with_question_postfix);
    utest_run("parse_at_sync_event",
              parse_at_sync_event);
    utest_run("parse_at_event_with_onleave",
              parse_at_event_with_onleave);
    utest_run("parse_at_event_no_onleave",
              parse_at_event_no_onleave);
    utest_run("parse_at_no_question_still_watcher",
              parse_at_no_question_still_watcher);
    utest_run("parse_question_outside_at_errors",
              parse_question_outside_at_errors);
    utest_run("parse_question_outside_at_standalone",
              parse_question_outside_at_standalone);
}
