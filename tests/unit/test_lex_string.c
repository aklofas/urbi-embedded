/* SPDX-License-Identifier: BSD-3-Clause */
/* test_lex_string.c — LEX-035 / Phase 1: string-literal lex.
 *
 * Per REVIVAL §14.1 row L3 (preserve adjacent-string concat) + spec
 * Phase 1: "foo" lexes to TOK_STRING with start/len pointing into the
 * source buffer (lifetime aliased to the source span — see UToken docs
 * at src/lex/ulex.h).  Escape resolution happens at parse time so the
 * lexer remains zero-allocation (LEX-027). */

#include "utest.h"
#include "lex/ulex.h"
#include <string.h>

static void lex_string_basic(void) {
    const char src[] = "\"foo\"";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t = ulex_next(&lex);
    UASSERT_EQ(t.type, TOK_STRING);
    UASSERT_EQ(t.u.str.len, 3);
    UASSERT(memcmp(t.u.str.start, "foo", 3) == 0);
    UToken eof = ulex_next(&lex);
    UASSERT_EQ(eof.type, TOK_EOF);
}

static void lex_string_empty(void) {
    /* "" — interior len is 0; outer t.len covers both quotes (2). */
    const char src[] = "\"\"";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t = ulex_next(&lex);
    UASSERT_EQ(t.type, TOK_STRING);
    UASSERT_EQ(t.u.str.len, 0);
    UASSERT_EQ(t.len, 2);
    UToken eof = ulex_next(&lex);
    UASSERT_EQ(eof.type, TOK_EOF);
}

static void lex_string_escape_newline(void) {
    /* Source: "a\nb" — interior bytes literally are: a, \, n, b (len=4).
     * Escape resolution happens at parse time, NOT lex time. */
    const char src[] = "\"a\\nb\"";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t = ulex_next(&lex);
    UASSERT_EQ(t.type, TOK_STRING);
    UASSERT_EQ(t.u.str.len, 4);
    UASSERT(memcmp(t.u.str.start, "a\\nb", 4) == 0);
}

static void lex_string_escape_quote(void) {
    /* "a\"b" — interior bytes a, \, ", b (len=4). */
    const char src[] = "\"a\\\"b\"";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t = ulex_next(&lex);
    UASSERT_EQ(t.type, TOK_STRING);
    UASSERT_EQ(t.u.str.len, 4);
    UASSERT(memcmp(t.u.str.start, "a\\\"b", 4) == 0);
}

static void lex_string_unterminated(void) {
    /* No closing quote → LEX_UNTERMINATED_STRING; cursor at EOF. */
    const char src[] = "\"foo";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t = ulex_next(&lex);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_UNTERMINATED_STRING);
    /* Cursor advanced to EOF for clean recovery. */
    UToken next = ulex_next(&lex);
    UASSERT_EQ(next.type, TOK_EOF);
}

static void lex_string_invalid_escape(void) {
    /* "a\x" — \x not in Phase 1 escape set → LEX_INVALID_ESCAPE. */
    const char src[] = "\"a\\x\"";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t = ulex_next(&lex);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_INVALID_ESCAPE);
}

void test_lex_string_suite(void) {
    utest_run("lex_string_basic", lex_string_basic);
    utest_run("lex_string_empty", lex_string_empty);
    utest_run("lex_string_escape_newline", lex_string_escape_newline);
    utest_run("lex_string_escape_quote", lex_string_escape_quote);
    utest_run("lex_string_unterminated", lex_string_unterminated);
    utest_run("lex_string_invalid_escape", lex_string_invalid_escape);
}
