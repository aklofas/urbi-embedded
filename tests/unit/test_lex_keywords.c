/* SPDX-License-Identifier: BSD-3-Clause */
/* Lexer tests for M5 reactive keywords. */

#include "utest.h"
#include "ulex.h"

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

void test_lex_keywords_suite(void) {
    utest_run("lex_at_keyword",        lex_at_keyword);
    utest_run("lex_whenever_keyword",  lex_whenever_keyword);
    utest_run("lex_waituntil_keyword", lex_waituntil_keyword);
    utest_run("lex_onleave_keyword",   lex_onleave_keyword);
    utest_run("lex_sync_keyword",      lex_sync_keyword);
    utest_run("lex_async_keyword",     lex_async_keyword);
}
