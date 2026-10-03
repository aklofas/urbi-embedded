/* SPDX-License-Identifier: BSD-3-Clause */
/* Compile-time tests for the merged AST_WATCHER node: every reactive form
 * (at / at sync / whenever / waituntil, crossed with cond / event /
 * slot-change source) shares one node kind, discriminated by
 * u.watcher.mode and u.watcher.source.
 *
 * Tests confirm the union payload struct's fields compile when assigned
 * through n.u.watcher.<field> — catches typos or missing struct members.
 * (Prior to the merge this file also checked 4 reactive AST kinds were
 * pairwise distinct; that check no longer applies now that there is one
 * kind — distinctness across mode/source combinations is covered by
 * tests/unit/test_parse_watcher.c instead.) */

#include "utest.h"
#include "parse/uast.h"

static void ast_watcher_payload_compiles(void) {
    UAstNode n = {0};
    n.kind = AST_WATCHER;
    n.u.watcher.mode            = UWATCHER_AT;
    n.u.watcher.source          = UWSRC_COND;
    n.u.watcher.cond            = NULL;
    n.u.watcher.slot_name       = NULL;
    n.u.watcher.slot_name_len   = 0;
    n.u.watcher.body            = NULL;
    n.u.watcher.else_body       = NULL;
    n.u.watcher.onleave         = NULL;
    n.u.watcher.payload_var     = NULL;
    n.u.watcher.payload_var_len = 0;
    UASSERT_EQ(n.kind, AST_WATCHER);
}

static void ast_watcher_modes_are_distinct(void) {
    UASSERT(UWATCHER_AT != UWATCHER_WHENEVER);
    UASSERT(UWATCHER_AT != UWATCHER_AT_SYNC);
    UASSERT(UWATCHER_AT != UWATCHER_WAITUNTIL);
    UASSERT(UWATCHER_WHENEVER != UWATCHER_AT_SYNC);
    UASSERT(UWATCHER_WHENEVER != UWATCHER_WAITUNTIL);
    UASSERT(UWATCHER_AT_SYNC != UWATCHER_WAITUNTIL);
}

static void ast_watcher_sources_are_distinct(void) {
    UASSERT(UWSRC_COND != UWSRC_EVENT);
    UASSERT(UWSRC_COND != UWSRC_SLOT_CHANGE);
    UASSERT(UWSRC_EVENT != UWSRC_SLOT_CHANGE);
}

void test_ast_alloc_suite(void) {
    utest_run("ast_watcher_payload_compiles",    ast_watcher_payload_compiles);
    utest_run("ast_watcher_modes_are_distinct",  ast_watcher_modes_are_distinct);
    utest_run("ast_watcher_sources_are_distinct", ast_watcher_sources_are_distinct);
}
