/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: AST_AT_SLOT_CHANGE parser disambiguation (T62, spec #4 §4.3-§4.6).
 *
 * Cases:
 *   1. 3+ segments + .changed? → AST_AT_SLOT_CHANGE (is_sync = false)
 *   2. at sync + 3 segments  → AST_AT_SLOT_CHANGE (is_sync = true)
 *   3. 2 segments .changed?  → AST_AT_EVENT (falls through)
 *   4. bare obj.x.changed    → PARSE_SLOT_CHANGED_BARE_V1 error
 *   5. obj.x.changed!        → PARSE_SLOT_CHANGED_EMIT_V1 error
 */

#include "utest.h"

#include <string.h>
#include <stddef.h>

#include "value/uarena.h"
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
 * Test 1: 3+ segments + .changed? → AST_AT_SLOT_CHANGE (sync=false)
 * ----------------------------------------------------------------------- */

UTEST(parse_3plus_segments_with_changed_q_yields_slot_change)
{
    Ctx62 c;
    ctx_init(&c, "at (myCat.x.changed?) body");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_AT_SLOT_CHANGE, (int)n->kind);
    UASSERT_EQ(0, (int)n->u.at_slot_change.is_sync);
    /* slot_name must be "x" (not "changed") */
    UASSERT_EQ(1, (int)n->u.at_slot_change.slot_name_len);
    UASSERT(n->u.at_slot_change.slot_name[0] == 'x');
    /* receiver must be non-NULL */
    UASSERT(n->u.at_slot_change.receiver != NULL);
    ctx_destroy(&c);
}

/* -----------------------------------------------------------------------
 * Test 2: at sync + 3+ segments → AST_AT_SLOT_CHANGE (sync=true)
 * ----------------------------------------------------------------------- */

UTEST(parse_at_sync_slot_change)
{
    Ctx62 c;
    ctx_init(&c, "at sync (a.b.c.changed?) body");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_AT_SLOT_CHANGE, (int)n->kind);
    UASSERT_EQ(1, (int)n->u.at_slot_change.is_sync);
    /* slot_name must be "c" */
    UASSERT_EQ(1, (int)n->u.at_slot_change.slot_name_len);
    UASSERT(n->u.at_slot_change.slot_name[0] == 'c');
    ctx_destroy(&c);
}

/* -----------------------------------------------------------------------
 * Test 3: 2-segment .changed? falls through to AST_AT_EVENT
 * ----------------------------------------------------------------------- */

UTEST(parse_2_segments_changed_q_falls_through_to_at_event)
{
    Ctx62 c;
    ctx_init(&c, "at (myCat.changed?) body");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ((int)AST_AT_EVENT, (int)n->kind);
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
