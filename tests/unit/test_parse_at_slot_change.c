/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: AST_WATCHER (source=UWSRC_SLOT_CHANGE) parser disambiguation
 * (spec #4 §4.3-§4.6).
 *
 * Cases:
 *   1. 3+ segments + .changed? → source=UWSRC_SLOT_CHANGE, mode=UWATCHER_AT
 *   2. at sync + 3 segments  → source=UWSRC_SLOT_CHANGE, mode=UWATCHER_AT_SYNC
 *   3. 2 segments .changed?  → source=UWSRC_EVENT (falls through)
 *   4. bare obj.x.changed    → PARSE_SLOT_CHANGED_BARE_V1 error
 *   5. obj.x.changed!        → PARSE_SLOT_CHANGED_EMIT_V1 error
 */

#include "utest.h"

#include <string.h>
#include <stddef.h>

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
} Ctx62;

static void ctx_init(Ctx62 *c, const char *src) {
    ulex_init(&c->lex, src, strlen(src));
    uarena_init(&c->arena, 0);
    uparse_init(&c->p, &c->lex, &c->arena);
}

static void ctx_destroy(Ctx62 *c) {
    uarena_destroy(&c->arena);
}

/* -----------------------------------------------------------------------
 * Test 1: 3+ segments + .changed? → source=UWSRC_SLOT_CHANGE, mode=UWATCHER_AT
 * ----------------------------------------------------------------------- */

UTEST(parse_3plus_segments_with_changed_q_yields_slot_change)
{
    Ctx62 c;
    ctx_init(&c, "at (myCat.x.changed?) body");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_WATCHER, (int)n->kind);
    UASSERT_EQ((int)UWSRC_SLOT_CHANGE, (int)n->u.watcher.source);
    UASSERT_EQ(UWATCHER_AT, n->u.watcher.mode);
    /* slot_name must be "x" (not "changed") */
    UASSERT_EQ(1, n->u.watcher.slot_name_len);
    UASSERT(n->u.watcher.slot_name[0] == 'x');
    /* receiver (cond) must be non-NULL */
    UASSERT(n->u.watcher.cond != NULL);
    ctx_destroy(&c);
}

/* -----------------------------------------------------------------------
 * Test 2: at sync + 3+ segments → source=UWSRC_SLOT_CHANGE, mode=UWATCHER_AT_SYNC
 * ----------------------------------------------------------------------- */

UTEST(parse_at_sync_slot_change)
{
    Ctx62 c;
    ctx_init(&c, "at sync (a.b.c.changed?) body");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_WATCHER, (int)n->kind);
    UASSERT_EQ((int)UWSRC_SLOT_CHANGE, (int)n->u.watcher.source);
    UASSERT_EQ(UWATCHER_AT_SYNC, n->u.watcher.mode);
    /* slot_name must be "c" */
    UASSERT_EQ(1, n->u.watcher.slot_name_len);
    UASSERT(n->u.watcher.slot_name[0] == 'c');
    ctx_destroy(&c);
}

/* -----------------------------------------------------------------------
 * Test 3: 2-segment .changed? falls through to source=UWSRC_EVENT
 * ----------------------------------------------------------------------- */

UTEST(parse_2_segments_changed_q_falls_through_to_at_event)
{
    Ctx62 c;
    ctx_init(&c, "at (myCat.changed?) body");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_WATCHER, (int)n->kind);
    UASSERT_EQ((int)UWSRC_EVENT, (int)n->u.watcher.source);
    ctx_destroy(&c);
}

/* -----------------------------------------------------------------------
 * Test 4: bare obj.x.changed → PARSE_SLOT_CHANGED_BARE_V1 error
 * ----------------------------------------------------------------------- */

UTEST(parse_bare_obj_x_changed_errors)
{
    Ctx62 c;
    ctx_init(&c, "var v = obj.x.changed");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_ERROR, (int)n->kind);
    UASSERT_EQ((int)PARSE_SLOT_CHANGED_BARE_V1, n->u.err.code);
    ctx_destroy(&c);
}

/* -----------------------------------------------------------------------
 * Test 5: obj.x.changed! → PARSE_SLOT_CHANGED_EMIT_V1 error
 * ----------------------------------------------------------------------- */

UTEST(parse_obj_x_changed_emit_errors)
{
    Ctx62 c;
    ctx_init(&c, "obj.x.changed!");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_ERROR, (int)n->kind);
    UASSERT_EQ((int)PARSE_SLOT_CHANGED_EMIT_V1, n->u.err.code);
    ctx_destroy(&c);
}

/* -----------------------------------------------------------------------
 * Suite entry point
 * ----------------------------------------------------------------------- */

void
test_parse_at_slot_change_suite(void)
{
    printf("test_parse_at_slot_change\n");
    utest_run("parse_3plus_segments_with_changed_q_yields_slot_change",
              parse_3plus_segments_with_changed_q_yields_slot_change);
    utest_run("parse_at_sync_slot_change",
              parse_at_sync_slot_change);
    utest_run("parse_2_segments_changed_q_falls_through_to_at_event",
              parse_2_segments_changed_q_falls_through_to_at_event);
    utest_run("parse_bare_obj_x_changed_errors",
              parse_bare_obj_x_changed_errors);
    utest_run("parse_obj_x_changed_emit_errors",
              parse_obj_x_changed_emit_errors);
}
