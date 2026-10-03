/* SPDX-License-Identifier: BSD-3-Clause */
/* The parser is the only place that desugars: every sugar form must
 * arrive at the emitter as one of the core AST kinds. */
#include "utest.h"
#include "util/uarena.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "parse/uast.h"
#include <string.h>

#define UTEST(name) static void name(void)

typedef struct { ULexer lex; UArena arena; UParser p; } PFix;

static UAstNode *parse_one(PFix *f, const char *src) {
    ulex_init(&f->lex, src, strlen(src));
    uarena_init(&f->arena, 4096);
    uparse_init(&f->p, &f->lex, &f->arena);
    return uparse_next_statement(&f->p);
}
static void pfix_close(PFix *f) { uarena_destroy(&f->arena); }

static int ident_is(const UAstNode *n, const char *s) {
    return n && n->kind == AST_IDENT && (int)strlen(s) == n->u.ident.len
        && memcmp(n->u.ident.start, s, (size_t)n->u.ident.len) == 0;
}
static int member_is(const UAstNode *n, const char *s) {
    return n && n->kind == AST_MEMBER_GET && (int)strlen(s) == n->u.member.name_len
        && memcmp(n->u.member.name_start, s, (size_t)n->u.member.name_len) == 0;
}

UTEST(list_literal_is_a_call_to_list_new) {
    PFix f; UAstNode *n = parse_one(&f, "[1, 2, 3]");
    UASSERT(n && n->kind == AST_CALL);
    UASSERT(member_is(n->u.call.callee, "new"));
    UASSERT(ident_is(n->u.call.callee->u.member.recv, "List"));
    UASSERT_EQ(3, n->u.call.arg_count);
    UASSERT_EQ((int)AST_INT, (int)n->u.call.args[2]->kind);
    pfix_close(&f);
}

UTEST(empty_list_literal_is_list_new_with_no_args) {
    PFix f; UAstNode *n = parse_one(&f, "[]");
    UASSERT(n && n->kind == AST_CALL && n->u.call.arg_count == 0);
    pfix_close(&f);
}

UTEST(dict_literal_is_a_block_that_builds_and_yields_the_dict) {
    PFix f; UAstNode *n = parse_one(&f, "[\"a\" => 1, \"b\" => 2]");
    UASSERT(n && n->kind == AST_BLOCK);
    UASSERT_EQ(4, n->u.block.count);                 /* var $d = Dict.new(); set; set; $d */
    UASSERT_EQ((int)AST_VAR_DECL, (int)n->u.block.stmts[0]->kind);
    UASSERT_EQ((int)AST_CALL,     (int)n->u.block.stmts[1]->kind);
    UASSERT(member_is(n->u.block.stmts[1]->u.call.callee, "set"));
    UASSERT_EQ(2, n->u.block.stmts[1]->u.call.arg_count);
    UASSERT_EQ((int)AST_IDENT,    (int)n->u.block.stmts[3]->kind);
    UASSERT_EQ('\x01', n->u.block.stmts[3]->u.ident.start[0]);
    pfix_close(&f);
}

UTEST(subscript_get_is_recv_get) {
    PFix f; UAstNode *n = parse_one(&f, "l[i]");
    UASSERT(n && n->kind == AST_CALL && member_is(n->u.call.callee, "get"));
    UASSERT(ident_is(n->u.call.callee->u.member.recv, "l"));
    UASSERT_EQ(1, n->u.call.arg_count);
    pfix_close(&f);
}

UTEST(subscript_set_is_recv_set) {
    PFix f; UAstNode *n = parse_one(&f, "l[i] = 5");
    UASSERT(n && n->kind == AST_CALL && member_is(n->u.call.callee, "set"));
    UASSERT_EQ(2, n->u.call.arg_count);
    pfix_close(&f);
}

UTEST(compound_subscript_evaluates_receiver_and_index_once) {
    PFix f; UAstNode *n = parse_one(&f, "l[i] += 1");
    UASSERT(n && n->kind == AST_BLOCK && n->u.block.count == 3);   /* var $r; var $i; $r.set($i, $r.get($i) + 1) */
    UASSERT_EQ((int)AST_VAR_DECL, (int)n->u.block.stmts[0]->kind);
    UASSERT_EQ((int)AST_VAR_DECL, (int)n->u.block.stmts[1]->kind);
    UAstNode *set = n->u.block.stmts[2];
    UASSERT(set->kind == AST_CALL && member_is(set->u.call.callee, "set"));
    UASSERT_EQ((int)AST_BINARY, (int)set->u.call.args[1]->kind);
    pfix_close(&f);
}

UTEST(assert_paren_form_is_if_not_then_throw_string) {
    PFix f; UAstNode *n = parse_one(&f, "assert(1 == 2)");
    UASSERT(n && n->kind == AST_IF);
    UASSERT(n->u.if_stmt.cond->kind == AST_UNARY && n->u.if_stmt.cond->u.unary.op == UOP_NOT);
    UAstNode *thr = n->u.if_stmt.then_block->u.block.stmts[0];
    UASSERT(thr->kind == AST_THROW && thr->u.throw_expr.value->kind == AST_STR);
    UASSERT(memcmp(thr->u.throw_expr.value->u.str_lit.bytes, "assertion failed: 1 == 2", 24) == 0);
    UASSERT(n->u.if_stmt.else_block == NULL);
    pfix_close(&f);
}

UTEST(assert_block_form_throws_the_bare_message) {
    PFix f; UAstNode *n = parse_one(&f, "assert { 1 == 2 }");
    UAstNode *thr = n->u.if_stmt.then_block->u.block.stmts[0];
    UASSERT_EQ(16, thr->u.throw_expr.value->u.str_lit.len);
    pfix_close(&f);
}

/* class Foo : public A, B { var x = 1; get y() { 2 } } desugars to a
 * BLOCK of 6 statements, not the 5 a naive "clone + inserts + slot
 * sets" reading would suggest:
 *
 *   var $cls = Object.clone();
 *   $cls.protos().insertFront(B); $cls.protos().insertFront(A);
 *   $cls.x = 1;
 *   $cls.setProperty("y", "oget", fn);
 *   Realm.Foo = $cls;
 *
 * The trailing `Realm.Foo = $cls` is load-bearing, not cosmetic: this
 * whole declaration is itself a BLOCK, which opens its own local scope,
 * so a plain `var Foo = ...` declared inside it would be popped when
 * the block closes and would never be visible to a sibling statement
 * like `Foo.x` (confirmed empirically against pre-existing `.chk`
 * fixtures: `class Point { var x = 1 }; Point.x` depends on it).  Every
 * proto/body reference inside the block goes through the hidden $cls
 * name instead of `Foo`, and `Foo` itself is bound exactly once, LAST,
 * via an explicit member-set on Realm — the one write form that
 * survives past the block regardless of nesting.  Binding it last (not
 * first) is also what keeps `class a : public a { ... }` resolving its
 * proto to the OUTER `a`: every reference before this final statement
 * still resolves against the pre-existing `a`, never the one under
 * construction. */
UTEST(class_declaration_is_a_block_of_clone_protos_and_slot_sets) {
    PFix f; UAstNode *n = parse_one(&f, "class Foo : public A, B { var x = 1; get y() { 2 } }");
    UASSERT(n && n->kind == AST_BLOCK);
    UASSERT_EQ(6, n->u.block.count);
    UASSERT_EQ((int)AST_VAR_DECL, (int)n->u.block.stmts[0]->kind);
    UASSERT(member_is(n->u.block.stmts[1]->u.call.callee, "insertFront"));
    UASSERT(ident_is(n->u.block.stmts[1]->u.call.args[0], "B"));
    UASSERT_EQ((int)AST_MEMBER_SET, (int)n->u.block.stmts[3]->kind);
    UASSERT(member_is(n->u.block.stmts[4]->u.call.callee, "setProperty"));
    UASSERT_EQ(3, n->u.block.stmts[4]->u.call.arg_count);
    UASSERT_EQ((int)AST_FUNCTION, (int)n->u.block.stmts[4]->u.call.args[2]->kind);
    UAstNode *export_stmt = n->u.block.stmts[5];
    UASSERT_EQ((int)AST_MEMBER_SET, (int)export_stmt->kind);
    UASSERT(ident_is(export_stmt->u.member.recv, "Realm"));
    UASSERT_EQ(3, export_stmt->u.member.name_len);
    UASSERT(memcmp(export_stmt->u.member.name_start, "Foo", 3) == 0);
    pfix_close(&f);
}

UTEST(property_declaration_with_receiver_is_set_property) {
    PFix f; UAstNode *n = parse_one(&f, "class K { set v(x) { x } }");
    UAstNode *call = n->u.block.stmts[1];
    UASSERT(call->kind == AST_CALL && member_is(call->u.call.callee, "setProperty"));
    UASSERT(memcmp(call->u.call.args[1]->u.str_lit.bytes, "oset", 4) == 0);
    pfix_close(&f);
}

UTEST(arrow_access_is_a_parse_error) {
    PFix f; UAstNode *n = parse_one(&f, "o.x->prop");
    UASSERT(n && n->kind == AST_ERROR);
    pfix_close(&f);
}

UTEST(semicolon_sequence_is_one_seq_node) {
    PFix f; UAstNode *n = parse_one(&f, "1; 2; 3");
    UASSERT(n && n->kind == AST_SEQ && n->u.seq.separator == SEP_SEMI && n->u.seq.count == 3);
    pfix_close(&f);
}

UTEST(pipe_amp_pair_is_a_two_child_seq) {
    PFix f; UAstNode *n = parse_one(&f, "1 & 2");
    UASSERT(n && n->kind == AST_SEQ && n->u.seq.separator == SEP_AMP && n->u.seq.count == 2);
    pfix_close(&f);
}

void test_parse_desugar_suite(void) {
    utest_run("list_literal_is_a_call_to_list_new", list_literal_is_a_call_to_list_new);
    utest_run("empty_list_literal_is_list_new_with_no_args", empty_list_literal_is_list_new_with_no_args);
    utest_run("dict_literal_is_a_block_that_builds_and_yields_the_dict", dict_literal_is_a_block_that_builds_and_yields_the_dict);
    utest_run("subscript_get_is_recv_get", subscript_get_is_recv_get);
    utest_run("subscript_set_is_recv_set", subscript_set_is_recv_set);
    utest_run("compound_subscript_evaluates_receiver_and_index_once", compound_subscript_evaluates_receiver_and_index_once);
    utest_run("assert_paren_form_is_if_not_then_throw_string", assert_paren_form_is_if_not_then_throw_string);
    utest_run("assert_block_form_throws_the_bare_message", assert_block_form_throws_the_bare_message);
    utest_run("class_declaration_is_a_block_of_clone_protos_and_slot_sets", class_declaration_is_a_block_of_clone_protos_and_slot_sets);
    utest_run("property_declaration_with_receiver_is_set_property", property_declaration_with_receiver_is_set_property);
    utest_run("arrow_access_is_a_parse_error", arrow_access_is_a_parse_error);
    utest_run("semicolon_sequence_is_one_seq_node", semicolon_sequence_is_one_seq_node);
    utest_run("pipe_amp_pair_is_a_two_child_seq", pipe_amp_pair_is_a_two_child_seq);
}
