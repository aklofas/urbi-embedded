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

void test_lex_string_suite(void) {
    utest_run("lex_string_basic", lex_string_basic);
}
