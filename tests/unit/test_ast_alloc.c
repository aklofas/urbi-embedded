/* SPDX-License-Identifier: BSD-3-Clause */
/* Compile-time + distinctness tests for the 4 M5 reactive AST node kinds.
 *
 * Tests confirm:
 *   1. All 4 new enum values are pairwise distinct.
 *   2. Each union payload struct's fields compile when assigned through
 *      n.u.<member>.<field> — catches typos or missing struct members.
 */

#include "utest.h"
#include "parse/uast.h"

static void ast_watcher_distinct(void) {
    UASSERT(AST_WATCHER != AST_WAITUNTIL);
    UASSERT(AST_WATCHER != AST_AT_EVENT);
    UASSERT(AST_WATCHER != AST_AT_SLOT_CHANGE);
    UASSERT(AST_WAITUNTIL != AST_AT_EVENT);
    UASSERT(AST_WAITUNTIL != AST_AT_SLOT_CHANGE);
    UASSERT(AST_AT_EVENT != AST_AT_SLOT_CHANGE);
}

static void ast_watcher_payload_compiles(void) {
    UAstNode n = {0};
    n.kind = AST_WATCHER;
    n.u.watcher.cond    = NULL;
    n.u.watcher.body    = NULL;
    n.u.watcher.onleave = NULL;
    n.u.watcher.mode    = 0;  /* UWatcherMode enum lands in T12; int for now */
    UASSERT_EQ(n.kind, AST_WATCHER);
}

static void ast_waituntil_payload_compiles(void) {
    UAstNode n = {0};
    n.kind = AST_WAITUNTIL;
    n.u.waituntil.cond = NULL;
    UASSERT_EQ(n.kind, AST_WAITUNTIL);
}

static void ast_at_event_payload_compiles(void) {
    UAstNode n = {0};
    n.kind = AST_AT_EVENT;
    n.u.at_event.event_expr = NULL;
    n.u.at_event.body       = NULL;
    n.u.at_event.onleave    = NULL;
    n.u.at_event.is_sync    = false;
    UASSERT_EQ(n.kind, AST_AT_EVENT);
}

static void ast_at_slot_change_payload_compiles(void) {
    UAstNode n = {0};
    n.kind = AST_AT_SLOT_CHANGE;
    n.u.at_slot_change.receiver      = NULL;
    n.u.at_slot_change.slot_name     = NULL;
    n.u.at_slot_change.slot_name_len = 0;
    n.u.at_slot_change.body          = NULL;
    n.u.at_slot_change.onleave       = NULL;
    n.u.at_slot_change.is_sync       = false;
    UASSERT_EQ(n.kind, AST_AT_SLOT_CHANGE);
}

void test_ast_alloc_suite(void) {
    utest_run("ast_watcher_distinct",             ast_watcher_distinct);
    utest_run("ast_watcher_payload_compiles",     ast_watcher_payload_compiles);
    utest_run("ast_waituntil_payload_compiles",   ast_waituntil_payload_compiles);
    utest_run("ast_at_event_payload_compiles",    ast_at_event_payload_compiles);
    utest_run("ast_at_slot_change_payload_compiles", ast_at_slot_change_payload_compiles);
}
