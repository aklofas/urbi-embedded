/* SPDX-License-Identifier: BSD-3-Clause */
/* Lexer tests for the `every` reactive-family keyword (v0.9.4 Task 5.1). */

#include "utest.h"
#include "lex/ulex.h"
#include <string.h>

static void lex_every_keyword(void) {
    ULexer l; ulex_init(&l, "every", 5);
    const UToken t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_KW_EVERY);
    UASSERT_EQ(t.len, 5);
}

static void lex_everyone_is_identifier(void) {
    /* Adjacent identifier must not be eagerly matched as `every` + `one`. */
    ULexer l; ulex_init(&l, "everyone", 8);
    const UToken t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_IDENT);
    UASSERT_EQ(t.len, 8);
}

static void lex_every_uppercase_is_identifier(void) {
    /* Case-sensitive: `Every` lexes as plain identifier. */
    ULexer l; ulex_init(&l, "Every", 5);
    const UToken t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_IDENT);
    UASSERT_EQ(t.len, 5);
}

static void lex_every_followed_by_paren(void) {
    /* `every (` — no whitespace coalescing issues. */
    const char *src = "every (";
    ULexer l; ulex_init(&l, src, strlen(src));
    UToken t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_KW_EVERY);
    t = ulex_next(&l);
    UASSERT_EQ(t.type, TOK_LPAREN);
}

void test_lex_every_suite(void) {
    utest_run("lex_every_keyword",              lex_every_keyword);
    utest_run("lex_everyone_is_identifier",     lex_everyone_is_identifier);
    utest_run("lex_every_uppercase_is_identifier", lex_every_uppercase_is_identifier);
    utest_run("lex_every_followed_by_paren",    lex_every_followed_by_paren);
}
