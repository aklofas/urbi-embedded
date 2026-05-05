/* SPDX-License-Identifier: BSD-3-Clause */
/* Lexer tests for M5 reactive keywords. */

#include "utest.h"
#include "uarena.h"
#include "uast.h"
#include "ulex.h"
#include "uparse.h"
#include <string.h>

static void lex_at_keyword(void) {
    ULexer l; ulex_init(&l, "at", 2);
    const UToken t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_KW_AT);
    UASSERT_EQ(t.len, 2);
}

static void lex_whenever_keyword(void) {
    ULexer l; ulex_init(&l, "whenever", 8);
    const UToken t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_KW_WHENEVER);
    UASSERT_EQ(t.len, 8);
}

static void lex_waituntil_keyword(void) {
    ULexer l; ulex_init(&l, "waituntil", 9);
    const UToken t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_KW_WAITUNTIL);
    UASSERT_EQ(t.len, 9);
}

static void lex_onleave_keyword(void) {
    ULexer l; ulex_init(&l, "onleave", 7);
    const UToken t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_KW_ONLEAVE);
    UASSERT_EQ(t.len, 7);
}

static void lex_sync_keyword(void) {
    ULexer l; ulex_init(&l, "sync", 4);
    const UToken t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_KW_SYNC);
    UASSERT_EQ(t.len, 4);
}

static void lex_async_keyword(void) {
    ULexer l; ulex_init(&l, "async", 5);
    const UToken t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_KW_ASYNC);
    UASSERT_EQ(t.len, 5);
}

/* --- T4: reserved-keyword-as-variable-name diagnostics --- */

/* Helper: parse one statement from src, return the AST_ERROR parse code.
 * Returns PARSE_OK if the statement parses without error. */
static UParseError parse_var_error(const char *src) {
    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    uarena_init(&arena, 1024);
    UParser p;
    uparse_init(&p, &lex, &arena);
    UAstNode *n = uparse_next_statement(&p);
    UParseError code = PARSE_OK;
    if (n != NULL && n->kind == AST_ERROR) {
        code = (UParseError)n->u.err.code;
    }
    uarena_destroy(&arena);
    return code;
}

/* Helper: returns the error message from the first AST_ERROR, or NULL. */
static const char *parse_var_errmsg(const char *src) {
    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    uarena_init(&arena, 1024);
    UParser p;
    uparse_init(&p, &lex, &arena);
    UAstNode *n = uparse_next_statement(&p);
    const char *msg = NULL;
    if (n != NULL && n->kind == AST_ERROR) {
        msg = n->u.err.message;
    }
    uarena_destroy(&arena);
    return msg;
}

static void var_at_as_ident_fails(void) {
    /* `var at = 1` must be rejected with PARSE_RESERVED_KEYWORD_AS_IDENT. */
    UASSERT_EQ((int)PARSE_RESERVED_KEYWORD_AS_IDENT,
               (int)parse_var_error("var at = 1"));
}

static void var_whenever_as_ident_fails(void) {
    UASSERT_EQ((int)PARSE_RESERVED_KEYWORD_AS_IDENT,
               (int)parse_var_error("var whenever = 1"));
}

static void var_waituntil_as_ident_fails(void) {
    UASSERT_EQ((int)PARSE_RESERVED_KEYWORD_AS_IDENT,
               (int)parse_var_error("var waituntil = 1"));
}

static void var_onleave_as_ident_fails(void) {
    UASSERT_EQ((int)PARSE_RESERVED_KEYWORD_AS_IDENT,
               (int)parse_var_error("var onleave = 1"));
}

static void var_sync_as_ident_fails(void) {
    UASSERT_EQ((int)PARSE_RESERVED_KEYWORD_AS_IDENT,
               (int)parse_var_error("var sync = 1"));
}

static void var_async_as_ident_ok(void) {
    /* `async` is a soft keyword — allowed as identifier at v1.0. */
    UASSERT_EQ((int)PARSE_OK,
               (int)parse_var_error("var async = 1"));
}

static void var_reserved_keyword_has_diagnostic(void) {
    /* The error message must be non-NULL and mention the keyword. */
    const char *msg = parse_var_errmsg("var at = 1");
    UASSERT(msg != NULL);
}

void test_lex_keywords_suite(void) {
    utest_run("lex_at_keyword",        lex_at_keyword);
    utest_run("lex_whenever_keyword",  lex_whenever_keyword);
    utest_run("lex_waituntil_keyword", lex_waituntil_keyword);
    utest_run("lex_onleave_keyword",   lex_onleave_keyword);
    utest_run("lex_sync_keyword",      lex_sync_keyword);
    utest_run("lex_async_keyword",     lex_async_keyword);

    /* T4: reserved-keyword-as-variable-name diagnostics */
    utest_run("var at as ident: PARSE_RESERVED_KEYWORD_AS_IDENT",      var_at_as_ident_fails);
    utest_run("var whenever as ident: PARSE_RESERVED_KEYWORD_AS_IDENT", var_whenever_as_ident_fails);
    utest_run("var waituntil as ident: PARSE_RESERVED_KEYWORD_AS_IDENT", var_waituntil_as_ident_fails);
    utest_run("var onleave as ident: PARSE_RESERVED_KEYWORD_AS_IDENT",  var_onleave_as_ident_fails);
    utest_run("var sync as ident: PARSE_RESERVED_KEYWORD_AS_IDENT",     var_sync_as_ident_fails);
    utest_run("var async as ident: soft keyword, PARSE_OK",             var_async_as_ident_ok);
    utest_run("var reserved keyword: non-NULL diagnostic message",      var_reserved_keyword_has_diagnostic);
}
