/* SPDX-License-Identifier: BSD-3-Clause */
/* test_class_decl_parse.c — class declaration parsing.
 *
 * Verifies the `class Foo [: public A, B] { body }` surface desugars to
 * `var Foo = BLOCK { clone, proto inserts, slot sets, trailing $cls
 * reference }` — see uparse_stmt.c's parse_class_declaration for why the
 * name is bound by the OUTER var-decl rather than written inside the
 * block.  Also covers the S-class-name-scope rule: `class a : public a
 * { ... }` must not bind `a` to the inner class before its proto list
 * and body are built, so the proto reference resolves to the OUTER `a`. */

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

/* The outer node is `var <Name> = <block>`; returns the inner BLOCK. */
static const UAstNode *assert_outer_var_decl(const UAstNode *stmt, const char *name) {
    UASSERT(stmt != NULL);
    UASSERT_EQ((int)AST_VAR_DECL, (int)stmt->kind);
    UASSERT_EQ((int)strlen(name), stmt->u.var_decl.name_len);
    UASSERT(memcmp(stmt->u.var_decl.name_start, name, strlen(name)) == 0);
    UASSERT(stmt->u.var_decl.init != NULL);
    UASSERT_EQ((int)AST_BLOCK, (int)stmt->u.var_decl.init->kind);
    return stmt->u.var_decl.init;
}

/* The block's last statement is always the bare hidden `$cls` reference
 * (its value is what the outer var-decl binds the class name to). */
static void assert_trailing_cls_ident(const UAstNode *block) {
    UAstNode *last = block->u.block.stmts[block->u.block.count - 1];
    UASSERT_EQ((int)AST_IDENT, (int)last->kind);
    UASSERT('\x01' == last->u.ident.start[0]);
}

static void parse_class_no_protos(void) {
    ParseCtx c;
    ctx_init(&c, "class Foo { var x = 1 }");
    UAstNode *stmt = uparse_next_statement(&c.p);
    const UAstNode *block = assert_outer_var_decl(stmt, "Foo");
    /* var $cls = Object.clone(); $cls.x = 1; $cls */
    UASSERT_EQ(3, block->u.block.count);
    UASSERT_EQ((int)AST_VAR_DECL, (int)block->u.block.stmts[0]->kind);
    UASSERT_EQ((int)AST_MEMBER_SET, (int)block->u.block.stmts[1]->kind);
    UASSERT_EQ(1, block->u.block.stmts[1]->u.member.name_len);
    UASSERT_EQ('x', block->u.block.stmts[1]->u.member.name_start[0]);
    assert_trailing_cls_ident(block);
    ctx_destroy(&c);
}

static void parse_class_empty_body(void) {
    ParseCtx c;
    ctx_init(&c, "class Foo {}");
    UAstNode *stmt = uparse_next_statement(&c.p);
    const UAstNode *block = assert_outer_var_decl(stmt, "Foo");
    /* var $cls = Object.clone(); $cls */
    UASSERT_EQ(2, block->u.block.count);
    UASSERT_EQ((int)AST_VAR_DECL, (int)block->u.block.stmts[0]->kind);
    assert_trailing_cls_ident(block);
    ctx_destroy(&c);
}

static void parse_class_single_proto(void) {
    ParseCtx c;
    ctx_init(&c, "class Foo : public Bar { var x = 1 }");
    UAstNode *stmt = uparse_next_statement(&c.p);
    const UAstNode *block = assert_outer_var_decl(stmt, "Foo");
    /* var $cls = Object.clone(); $cls.protos().insertFront(Bar); $cls.x = 1; $cls */
    UASSERT_EQ(4, block->u.block.count);
    UAstNode *insert_call = block->u.block.stmts[1];
    UASSERT_EQ((int)AST_CALL, (int)insert_call->kind);
    UASSERT(member_is(insert_call->u.call.callee, "insertFront"));
    UASSERT_EQ(1, insert_call->u.call.arg_count);
    UAstNode *proto_arg = insert_call->u.call.args[0];
    UASSERT_EQ((int)AST_IDENT, (int)proto_arg->kind);
    UASSERT_EQ(3, proto_arg->u.ident.len);
    UASSERT(memcmp(proto_arg->u.ident.start, "Bar", 3) == 0);
    assert_trailing_cls_ident(block);
    ctx_destroy(&c);
}

static void parse_class_multi_proto_declaration_order(void) {
    /* class Foo : public A, B { ... } — protos insert in REVERSE
     * declaration order so the final proto chain reads [A, B, ...]
     * (S-mro-declaration-order). */
    ParseCtx c;
    ctx_init(&c, "class Foo : public A, B { var x = 1 }");
    UAstNode *stmt = uparse_next_statement(&c.p);
    const UAstNode *block = assert_outer_var_decl(stmt, "Foo");
    UASSERT_EQ(5, block->u.block.count);
    UAstNode *insert_b = block->u.block.stmts[1];
    UAstNode *insert_a = block->u.block.stmts[2];
    UASSERT(member_is(insert_b->u.call.callee, "insertFront"));
    UASSERT(member_is(insert_a->u.call.callee, "insertFront"));
    UASSERT(memcmp(insert_b->u.call.args[0]->u.ident.start, "B", 1) == 0);
    UASSERT(memcmp(insert_a->u.call.args[0]->u.ident.start, "A", 1) == 0);
    assert_trailing_cls_ident(block);
    ctx_destroy(&c);
}

static void parse_class_with_function(void) {
    ParseCtx c;
    ctx_init(&c, "class Foo { var bar = function() { return 42 } }");
    UAstNode *stmt = uparse_next_statement(&c.p);
    const UAstNode *block = assert_outer_var_decl(stmt, "Foo");
    UASSERT_EQ(3, block->u.block.count);
    /* The body's var-decl lowers to a MEMBER_SET whose value is the
     * AST_FUNCTION. */
    UAstNode *ms = block->u.block.stmts[1];
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
     * anywhere inside its block — only a hidden-named local — and the
     * OUTER var-decl that finally binds `a` wraps the whole block as
     * its initializer, so nothing shadows the outer `a` before the
     * proto reference is read. */
    ParseCtx c;
    ctx_init(&c,
        "class a { var foo = 40 } |"
        "class a : public a { var bar = 2 }");

    UAstNode *outer = uparse_next_statement(&c.p);
    UASSERT(outer != NULL);
    UASSERT(outer->kind != AST_ERROR);
    const UAstNode *outer_block = assert_outer_var_decl(outer, "a");
    assert_trailing_cls_ident(outer_block);

    UAstNode *inner = uparse_next_statement(&c.p);
    UASSERT(inner != NULL);
    UASSERT(inner->kind != AST_ERROR);
    const UAstNode *inner_block = assert_outer_var_decl(inner, "a");
    /* var $cls; insertFront(a); var bar; $cls */
    UASSERT_EQ(4, inner_block->u.block.count);
    UASSERT_EQ((int)AST_VAR_DECL, (int)inner_block->u.block.stmts[0]->kind);
    /* None of the hidden names start with 'a' — the leading byte is the
     * 0x01 sentinel, so there is no way a hidden local could alias the
     * class name being declared. */
    UASSERT_EQ('\x01', inner_block->u.block.stmts[0]->u.var_decl.name_start[0]);

    UAstNode *insert_call = inner_block->u.block.stmts[1];
    UASSERT(member_is(insert_call->u.call.callee, "insertFront"));
    UAstNode *proto = insert_call->u.call.args[0];
    UASSERT_EQ((int)AST_IDENT, (int)proto->kind);
    UASSERT(proto->u.ident.start[0] == 'a');

    assert_trailing_cls_ident(inner_block);

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
