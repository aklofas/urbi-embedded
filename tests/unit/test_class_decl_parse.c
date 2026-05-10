/* SPDX-License-Identifier: BSD-3-Clause */
/* test_class_decl_parse.c — Phase 6: class declaration parsing.
 *
 * Verifies the `class Foo [: public A, B] { body }` surface parses to
 * AST_CLASS_DECL with the expected payload (name, proto array preserving
 * declaration order, body block).  Also covers the S-class-name-scope
 * rule: the class name is NOT in scope while its protos and body parse,
 * so `class a : public a { ... }` resolves the proto to the OUTER `a`. */

#include "utest.h"
#include "lex/ulex.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "value/uarena.h"
#include <string.h>

/* === Task 65: lexer recognizes class / public === */

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

/* === Task 66/67: parse class declarations to AST_CLASS_DECL === */

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

static void parse_class_no_protos(void) {
    ParseCtx c;
    ctx_init(&c, "class Foo { var x = 1 }");
    UAstNode *stmt = uparse_next_statement(&c.p);
    UASSERT(stmt != NULL);
    UASSERT_EQ(stmt->kind, AST_CLASS_DECL);
    UASSERT_EQ(stmt->u.class_decl.name_len, 3);
    UASSERT(memcmp(stmt->u.class_decl.name_start, "Foo", 3) == 0);
    UASSERT_EQ(stmt->u.class_decl.proto_count, 0);
    UASSERT(stmt->u.class_decl.protos == NULL);
    UASSERT(stmt->u.class_decl.body != NULL);
    UASSERT_EQ(stmt->u.class_decl.body->kind, AST_BLOCK);
    UASSERT_EQ(stmt->u.class_decl.body->u.block.count, 1);
    ctx_destroy(&c);
}

static void parse_class_empty_body(void) {
    ParseCtx c;
    ctx_init(&c, "class Foo {}");
    UAstNode *stmt = uparse_next_statement(&c.p);
    UASSERT(stmt != NULL);
    UASSERT_EQ(stmt->kind, AST_CLASS_DECL);
    UASSERT_EQ(stmt->u.class_decl.proto_count, 0);
    UASSERT(stmt->u.class_decl.body != NULL);
    UASSERT_EQ(stmt->u.class_decl.body->kind, AST_BLOCK);
    UASSERT_EQ(stmt->u.class_decl.body->u.block.count, 0);
    ctx_destroy(&c);
}

static void parse_class_single_proto(void) {
    ParseCtx c;
    ctx_init(&c, "class Foo : public Bar { var x = 1 }");
    UAstNode *stmt = uparse_next_statement(&c.p);
    UASSERT(stmt != NULL);
    UASSERT_EQ(stmt->kind, AST_CLASS_DECL);
    UASSERT_EQ(stmt->u.class_decl.proto_count, 1);
    UASSERT(stmt->u.class_decl.protos != NULL);
    UAstNode *p0 = stmt->u.class_decl.protos[0];
    UASSERT(p0 != NULL);
    UASSERT_EQ(p0->kind, AST_IDENT);
    UASSERT_EQ(p0->u.ident.len, 3);
    UASSERT(memcmp(p0->u.ident.start, "Bar", 3) == 0);
    ctx_destroy(&c);
}

static void parse_class_multi_proto_declaration_order(void) {
    /* class Foo : public A, B { ... } — protos preserve declaration order
     * (S-mro-declaration-order; emit will reverse during insertFront). */
    ParseCtx c;
    ctx_init(&c, "class Foo : public A, B { var x = 1 }");
    UAstNode *stmt = uparse_next_statement(&c.p);
    UASSERT(stmt != NULL);
    UASSERT_EQ(stmt->kind, AST_CLASS_DECL);
    UASSERT_EQ(stmt->u.class_decl.proto_count, 2);
    UAstNode *p0 = stmt->u.class_decl.protos[0];
    UAstNode *p1 = stmt->u.class_decl.protos[1];
    UASSERT_EQ(p0->kind, AST_IDENT);
    UASSERT_EQ(p1->kind, AST_IDENT);
    UASSERT(memcmp(p0->u.ident.start, "A", 1) == 0);
    UASSERT(memcmp(p1->u.ident.start, "B", 1) == 0);
    ctx_destroy(&c);
}

static void parse_class_with_function(void) {
    ParseCtx c;
    ctx_init(&c, "class Foo { var bar = function() { return 42 } }");
    UAstNode *stmt = uparse_next_statement(&c.p);
    UASSERT(stmt != NULL);
    UASSERT_EQ(stmt->kind, AST_CLASS_DECL);
    UASSERT(stmt->u.class_decl.body != NULL);
    UASSERT_EQ(stmt->u.class_decl.body->u.block.count, 1);
    /* The body's first statement is a var-decl whose init is AST_FUNCTION. */
    UAstNode *vd = stmt->u.class_decl.body->u.block.stmts[0];
    UASSERT_EQ(vd->kind, AST_VAR_DECL);
    UASSERT_EQ(vd->u.var_decl.init->kind, AST_FUNCTION);
    ctx_destroy(&c);
}

/* === Task 68: nested-class shadow scoping === */

static void parse_nested_class_shadow(void) {
    /* Two top-level class statements with the same name:
     *
     *   class a { var foo = 40 }
     *   class a : public a { var bar = 2 }
     *
     * Parse must not error on either statement.  The inner class's
     * proto reference `a` is parsed as an AST_IDENT — at parse time we
     * cannot fully verify it resolves to the OUTER `a` (binding happens
     * at emit time), but we can verify the shape: the parser does NOT
     * bind the class name before parsing protos+body.  Combined with
     * emit's deferred-binding rule (the local install happens after
     * body emit), this lets `class a : public a { ... }` resolve the
     * proto to the outer binding (S-class-name-scope). */
    ParseCtx c;
    ctx_init(&c,
        "class a { var foo = 40 } |"
        "class a : public a { var bar = 2 }");

    UAstNode *outer = uparse_next_statement(&c.p);
    UASSERT(outer != NULL);
    UASSERT(outer->kind != AST_ERROR);
    UASSERT_EQ(outer->kind, AST_CLASS_DECL);
    UASSERT_EQ(outer->u.class_decl.name_len, 1);
    UASSERT(outer->u.class_decl.name_start[0] == 'a');
    UASSERT_EQ(outer->u.class_decl.proto_count, 0);

    UAstNode *inner = uparse_next_statement(&c.p);
    UASSERT(inner != NULL);
    UASSERT(inner->kind != AST_ERROR);
    UASSERT_EQ(inner->kind, AST_CLASS_DECL);
    UASSERT_EQ(inner->u.class_decl.name_len, 1);
    UASSERT(inner->u.class_decl.name_start[0] == 'a');
    UASSERT_EQ(inner->u.class_decl.proto_count, 1);
    UAstNode *proto = inner->u.class_decl.protos[0];
    UASSERT_EQ(proto->kind, AST_IDENT);
    UASSERT(proto->u.ident.start[0] == 'a');

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
