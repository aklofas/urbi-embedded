/* SPDX-License-Identifier: BSD-3-Clause */
/* test_class_decl_parse.c — class declaration parsing.
 *
 * Verifies the `class Foo [: public A, B] { body }` surface desugars to
 * a BLOCK of ordinary statements (clone, proto inserts, slot sets, and
 * a trailing Realm export — see uparse_stmt.c's parse_class_declaration
 * for why the export must be last).  Also covers the S-class-name-scope
 * rule: `class a : public a { ... }` must not bind `a` to the inner
 * class before its proto list and body are built, so the proto
 * reference resolves to the OUTER `a`. */

#include "utest.h"
#include "lex/ulex.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "util/uarena.h"
#include <string.h>

static void lex_class_keyword(void) {
    ULexer l;
    ulex_init(&l, "class", 5);
    UToken t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_KW_CLASS);
    UASSERT_EQ(t.len, 5);
}

static void lex_public_keyword(void) {
    ULexer l;
    ulex_init(&l, "public", 6);
    UToken t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_KW_PUBLIC);
    UASSERT_EQ(t.len, 6);
}

typedef struct {
    ULexer lex;
    UArena arena;
    UParser p;
} ParseCtx;

static void ctx_init(ParseCtx *c, const char *src) {
    ulex_init(&c->lex, src, strlen(src));
    uarena_init(&c->arena, 4096);
    uparse_init(&c->p, &c->lex, &c->arena);
}

static void ctx_destroy(ParseCtx *c) {
    uarena_destroy(&c->arena);
}

static int member_is(const UAstNode *n, const char *s) {
    return n && n->kind == AST_MEMBER_GET && (int)strlen(s) == n->u.member.name_len
        && memcmp(n->u.member.name_start, s, (size_t)n->u.member.name_len) == 0;
}

/* The export statement (always last) is `Realm.<Name> = $cls`. */
static void assert_export_stmt(const UAstNode *stmt, const char *name) {
    UASSERT(stmt != NULL);
    UASSERT_EQ((int)AST_MEMBER_SET, (int)stmt->kind);
    UASSERT(stmt->u.member.recv != NULL);
    UASSERT_EQ((int)AST_IDENT, (int)stmt->u.member.recv->kind);
    UASSERT_EQ(5, stmt->u.member.recv->u.ident.len);
    UASSERT(memcmp(stmt->u.member.recv->u.ident.start, "Realm", 5) == 0);
    UASSERT_EQ((int)strlen(name), stmt->u.member.name_len);
    UASSERT(memcmp(stmt->u.member.name_start, name, strlen(name)) == 0);
}

static void parse_class_no_protos(void) {
    ParseCtx c;
    ctx_init(&c, "class Foo { var x = 1 }");
    UAstNode *stmt = uparse_next_statement(&c.p);
    UASSERT(stmt != NULL);
    UASSERT_EQ(stmt->kind, AST_BLOCK);
    /* var $cls = Object.clone(); $cls.x = 1; Realm.Foo = $cls */
    UASSERT_EQ(3, stmt->u.block.count);
    UASSERT_EQ((int)AST_VAR_DECL, (int)stmt->u.block.stmts[0]->kind);
    UASSERT_EQ((int)AST_MEMBER_SET, (int)stmt->u.block.stmts[1]->kind);
    UASSERT_EQ(1, stmt->u.block.stmts[1]->u.member.name_len);
    UASSERT_EQ('x', stmt->u.block.stmts[1]->u.member.name_start[0]);
    assert_export_stmt(stmt->u.block.stmts[2], "Foo");
    ctx_destroy(&c);
}

static void parse_class_empty_body(void) {
    ParseCtx c;
    ctx_init(&c, "class Foo {}");
    UAstNode *stmt = uparse_next_statement(&c.p);
    UASSERT(stmt != NULL);
    UASSERT_EQ(stmt->kind, AST_BLOCK);
    /* var $cls = Object.clone(); Realm.Foo = $cls */
    UASSERT_EQ(2, stmt->u.block.count);
    UASSERT_EQ((int)AST_VAR_DECL, (int)stmt->u.block.stmts[0]->kind);
    assert_export_stmt(stmt->u.block.stmts[1], "Foo");
    ctx_destroy(&c);
}

static void parse_class_single_proto(void) {
    ParseCtx c;
    ctx_init(&c, "class Foo : public Bar { var x = 1 }");
    UAstNode *stmt = uparse_next_statement(&c.p);
    UASSERT(stmt != NULL);
    UASSERT_EQ(stmt->kind, AST_BLOCK);
    /* var $cls = Object.clone(); $cls.protos().insertFront(Bar); $cls.x = 1; Realm.Foo = $cls */
    UASSERT_EQ(4, stmt->u.block.count);
    UAstNode *insert_call = stmt->u.block.stmts[1];
    UASSERT_EQ((int)AST_CALL, (int)insert_call->kind);
    UASSERT(member_is(insert_call->u.call.callee, "insertFront"));
    UASSERT_EQ(1, insert_call->u.call.arg_count);
    UAstNode *proto_arg = insert_call->u.call.args[0];
    UASSERT_EQ((int)AST_IDENT, (int)proto_arg->kind);
    UASSERT_EQ(3, proto_arg->u.ident.len);
    UASSERT(memcmp(proto_arg->u.ident.start, "Bar", 3) == 0);
    assert_export_stmt(stmt->u.block.stmts[3], "Foo");
    ctx_destroy(&c);
}

static void parse_class_multi_proto_declaration_order(void) {
    /* class Foo : public A, B { ... } — protos insert in REVERSE
     * declaration order so the final proto chain reads [A, B, ...]
     * (S-mro-declaration-order). */
    ParseCtx c;
    ctx_init(&c, "class Foo : public A, B { var x = 1 }");
    UAstNode *stmt = uparse_next_statement(&c.p);
    UASSERT(stmt != NULL);
    UASSERT_EQ(stmt->kind, AST_BLOCK);
    UASSERT_EQ(5, stmt->u.block.count);
    UAstNode *insert_b = stmt->u.block.stmts[1];
    UAstNode *insert_a = stmt->u.block.stmts[2];
    UASSERT(member_is(insert_b->u.call.callee, "insertFront"));
    UASSERT(member_is(insert_a->u.call.callee, "insertFront"));
    UASSERT(memcmp(insert_b->u.call.args[0]->u.ident.start, "B", 1) == 0);
    UASSERT(memcmp(insert_a->u.call.args[0]->u.ident.start, "A", 1) == 0);
    assert_export_stmt(stmt->u.block.stmts[4], "Foo");
    ctx_destroy(&c);
}

static void parse_class_with_function(void) {
    ParseCtx c;
    ctx_init(&c, "class Foo { var bar = function() { return 42 } }");
    UAstNode *stmt = uparse_next_statement(&c.p);
    UASSERT(stmt != NULL);
    UASSERT_EQ(stmt->kind, AST_BLOCK);
    UASSERT_EQ(3, stmt->u.block.count);
    /* The body's var-decl lowers to a MEMBER_SET whose value is the
     * AST_FUNCTION. */
    UAstNode *ms = stmt->u.block.stmts[1];
    UASSERT_EQ((int)AST_MEMBER_SET, (int)ms->kind);
    UASSERT_EQ((int)AST_FUNCTION, (int)ms->u.member.value->kind);
    ctx_destroy(&c);
}

static void parse_nested_class_shadow(void) {
    /* Two top-level class statements with the same name:
     *
     *   class a { var foo = 40 }
     *   class a : public a { var bar = 2 }
     *
     * Parse must not error on either statement.  The inner class's
     * proto reference `a` is parsed as a plain AST_IDENT — at parse
     * time we cannot fully verify it resolves to the OUTER `a` (that
     * depends on emit-time scope resolution, exercised end-to-end by
     * tests/chk/objects/class_decl_nested.chk), but we can verify the
     * shape here: the inner class never builds a `var a = ...`
     * anywhere — only a hidden-named local — so nothing in this BLOCK
     * shadows the outer `a` before the proto reference is read. */
    ParseCtx c;
    ctx_init(&c,
        "class a { var foo = 40 } |"
        "class a : public a { var bar = 2 }");

    UAstNode *outer = uparse_next_statement(&c.p);
    UASSERT(outer != NULL);
    UASSERT(outer->kind != AST_ERROR);
    UASSERT_EQ(outer->kind, AST_BLOCK);
    assert_export_stmt(outer->u.block.stmts[outer->u.block.count - 1], "a");

    UAstNode *inner = uparse_next_statement(&c.p);
    UASSERT(inner != NULL);
    UASSERT(inner->kind != AST_ERROR);
    UASSERT_EQ(inner->kind, AST_BLOCK);
    /* var $cls; insertFront(a); var bar; Realm.a = $cls */
    UASSERT_EQ(4, inner->u.block.count);
    UASSERT_EQ((int)AST_VAR_DECL, (int)inner->u.block.stmts[0]->kind);
    /* None of the hidden names start with 'a' — the leading byte is the
     * 0x01 sentinel, so there is no way a hidden local could alias the
     * class name being declared. */
    UASSERT_EQ('\x01', inner->u.block.stmts[0]->u.var_decl.name_start[0]);

    UAstNode *insert_call = inner->u.block.stmts[1];
    UASSERT(member_is(insert_call->u.call.callee, "insertFront"));
    UAstNode *proto = insert_call->u.call.args[0];
    UASSERT_EQ((int)AST_IDENT, (int)proto->kind);
    UASSERT(proto->u.ident.start[0] == 'a');

    assert_export_stmt(inner->u.block.stmts[3], "a");

    ctx_destroy(&c);
}

void test_class_decl_parse_suite(void) {
    utest_run("lex_class_keyword",                  lex_class_keyword);
    utest_run("lex_public_keyword",                 lex_public_keyword);
    utest_run("parse_class_no_protos",              parse_class_no_protos);
    utest_run("parse_class_empty_body",             parse_class_empty_body);
    utest_run("parse_class_single_proto",           parse_class_single_proto);
    utest_run("parse_class_multi_proto_declaration_order",
              parse_class_multi_proto_declaration_order);
    utest_run("parse_class_with_function",          parse_class_with_function);
    utest_run("parse_nested_class_shadow",          parse_nested_class_shadow);
}
