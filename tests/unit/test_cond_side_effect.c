/* SPDX-License-Identifier: BSD-3-Clause */
/* T31: urbi_emit_cond_has_direct_side_effect compile-time walker — unit tests.
 *
 * Tests are built by constructing AST nodes directly on the stack rather
 * than going through the parser, so they are independent of parser feature
 * availability and make the expected AST structure explicit.
 */

#include "utest.h"

#include <string.h>

#include "parse/uast.h"
#include "emit/uemit.h"

#define UTEST(name) static void name(void)

/* -----------------------------------------------------------------------
 * Helpers — stack-allocate minimal AST nodes
 * ----------------------------------------------------------------------- */

static UAstNode make_int_node(int64_t val) {
    UAstNode n;
    memset(&n, 0, sizeof(n));
    n.kind  = AST_INT;
    n.line  = 1;
    n.col   = 1;
    n.u.i   = val;
    return n;
}

static UAstNode make_assign_node(void) {
    UAstNode n;
    memset(&n, 0, sizeof(n));
    n.kind             = AST_ASSIGN;
    n.line             = 1;
    n.col              = 1;
    n.u.assign.name_start = "x";
    n.u.assign.name_len   = 1;
    /* value is not inspected by the walker — leave NULL */
    n.u.assign.value   = NULL;
    return n;
}

static UAstNode make_var_decl_node(void) {
    UAstNode n;
    memset(&n, 0, sizeof(n));
    n.kind                  = AST_VAR_DECL;
    n.line                  = 1;
    n.col                   = 1;
    n.u.var_decl.name_start = "y";
    n.u.var_decl.name_len   = 1;
    n.u.var_decl.init       = NULL;
    return n;
}

static UAstNode make_compare_node(UAstNode *lhs, UAstNode *rhs) {
    UAstNode n;
    memset(&n, 0, sizeof(n));
    n.kind       = AST_COMPARE;
    n.line       = 1;
    n.col        = 1;
    n.u.cmp.op   = CMP_GT;
    n.u.cmp.lhs  = lhs;
    n.u.cmp.rhs  = rhs;
    return n;
}

static UAstNode make_binary_node(UAstNode *lhs, UAstNode *rhs) {
    UAstNode n;
    memset(&n, 0, sizeof(n));
    n.kind          = AST_BINARY;
    n.line          = 1;
    n.col           = 1;
    n.u.binary.op   = BOP_ADD;
    n.u.binary.lhs  = lhs;
    n.u.binary.rhs  = rhs;
    return n;
}

/* -----------------------------------------------------------------------
 * T31 test cases
 * ----------------------------------------------------------------------- */

/* Direct AST_ASSIGN node — must be detected. */
UTEST(cond_side_effect_detects_assign) {
    UAstNode n = make_assign_node();
    UASSERT(urbi_emit_cond_has_direct_side_effect(&n));
}

/* Direct AST_VAR_DECL node — must be detected. */
UTEST(cond_side_effect_detects_var_decl) {
    UAstNode n = make_var_decl_node();
    UASSERT(urbi_emit_cond_has_direct_side_effect(&n));
}

/* AST_ASSIGN nested inside an AST_BINARY — must recurse and detect. */
UTEST(cond_side_effect_detects_assign_inside_binary) {
    UAstNode zero  = make_int_node(0);
    UAstNode assign = make_assign_node();
    UAstNode bin   = make_binary_node(&assign, &zero);
    UASSERT(urbi_emit_cond_has_direct_side_effect(&bin));
}

/* AST_ASSIGN nested inside AST_COMPARE — must recurse and detect. */
UTEST(cond_side_effect_detects_assign_inside_compare) {
    UAstNode zero   = make_int_node(0);
    UAstNode assign = make_assign_node();
    UAstNode cmp    = make_compare_node(&assign, &zero);
    UASSERT(urbi_emit_cond_has_direct_side_effect(&cmp));
}

/* AST_CALL — opaque, must return false (best-effort). */
UTEST(cond_side_effect_treats_call_as_opaque) {
    UAstNode n;
    memset(&n, 0, sizeof(n));
    n.kind              = AST_CALL;
    n.line              = 1;
    n.col               = 1;
    n.u.call.callee     = NULL;
    n.u.call.args       = NULL;
    n.u.call.arg_count  = 0;
    UASSERT(!urbi_emit_cond_has_direct_side_effect(&n));
}

/* Pure compare expression — no side effect. */
UTEST(cond_side_effect_clean_for_compare) {
    UAstNode lhs = make_int_node(5);
    UAstNode rhs = make_int_node(10);
    UAstNode cmp = make_compare_node(&lhs, &rhs);
    UASSERT(!urbi_emit_cond_has_direct_side_effect(&cmp));
}

/* NULL pointer — must return false, not crash. */
UTEST(cond_side_effect_null_returns_false) {
    UASSERT(!urbi_emit_cond_has_direct_side_effect(NULL));
}

/* TIDY-008: AST_CALL and any unhandled AST kind (e.g. AST_INT, AST_IDENT)
 * must return false — collapsed `case AST_CALL:` into the `default:` arm
 * (the two branches were byte-identical).  Asserting both kinds share the
 * same return value locks the equivalence so a future divergence (where
 * AST_CALL's semantics actually differ from default) surfaces as a test
 * failure rather than silently re-cloning the branch. */
UTEST(cond_side_effect_call_and_unhandled_kinds_share_default) {
    UAstNode call;
    memset(&call, 0, sizeof(call));
    call.kind             = AST_CALL;
    call.line             = 1;
    call.col              = 1;
    call.u.call.callee    = NULL;
    call.u.call.args      = NULL;
    call.u.call.arg_count = 0;

    UAstNode int_node = make_int_node(42);

    /* Both fall through the same return-false path. */
    UASSERT(!urbi_emit_cond_has_direct_side_effect(&call));
    UASSERT(!urbi_emit_cond_has_direct_side_effect(&int_node));
}

/* -----------------------------------------------------------------------
 * Suite entry point
 * ----------------------------------------------------------------------- */

void test_cond_side_effect_suite(void) {
    utest_run("cond_side_effect_detects_assign",
              cond_side_effect_detects_assign);
    utest_run("cond_side_effect_detects_var_decl",
              cond_side_effect_detects_var_decl);
    utest_run("cond_side_effect_detects_assign_inside_binary",
              cond_side_effect_detects_assign_inside_binary);
    utest_run("cond_side_effect_detects_assign_inside_compare",
              cond_side_effect_detects_assign_inside_compare);
    utest_run("cond_side_effect_treats_call_as_opaque",
              cond_side_effect_treats_call_as_opaque);
    utest_run("cond_side_effect_clean_for_compare",
              cond_side_effect_clean_for_compare);
    utest_run("cond_side_effect_null_returns_false",
              cond_side_effect_null_returns_false);
    utest_run("cond_side_effect_call_and_unhandled_kinds_share_default",
              cond_side_effect_call_and_unhandled_kinds_share_default);
}
