/* SPDX-License-Identifier: BSD-3-Clause */
/* test_lex_operators.c — v1.0-rc stdlib-completeness lexer tests for the
 * %, &&, || operators.
 *
 * Guards:
 *   1. `&` / `|` still lex as the single-char statement separators
 *      (TOK_AMP / TOK_PIPE) after being moved out of the punct fast-path.
 *   2. `&&` / `||` lex as the new TOK_AMPAMP / TOK_PIPEPIPE.
 *   3. `%` lexes as the new TOK_PERCENT.
 *
 * Short-circuit evaluation is a runtime question and lives in the .chk
 * corpus, not here.
 */

#include "utest.h"
#include "lex/ulex.h"
#include "util/umacros.h"   /* urbi_zero/urbi_strlen only */
#include "urbi/urbi.h"
#include "urbi/urbi.h"

#define UTEST(name) static void name(void)

/* Return the n-th token type (0-based) of src. */
static UTokenType nth_tok(const char *src, int n) {
    ULexer lx;
    ulex_init(&lx, src, (size_t)urbi_strlen(src));
    UToken t;
    int i = 0;
    do { t = ulex_next(&lx); } while (i++ < n);
    return t.type;
}

UTEST(lex_amp_still_separator)   { UASSERT_EQ((int)nth_tok("a & b", 1),  (int)TOK_AMP); }
UTEST(lex_pipe_still_separator)  { UASSERT_EQ((int)nth_tok("a | b", 1),  (int)TOK_PIPE); }
UTEST(lex_ampamp)                { UASSERT_EQ((int)nth_tok("a && b", 1), (int)TOK_AMPAMP); }
UTEST(lex_pipepipe)              { UASSERT_EQ((int)nth_tok("a || b", 1), (int)TOK_PIPEPIPE); }
UTEST(lex_percent)               { UASSERT_EQ((int)nth_tok("a % b", 1),  (int)TOK_PERCENT); }

/* && / || are two chars; the trailing single separators must be unaffected:
 * `a & b & c` -> idents and AMP separators, never AMPAMP. */
UTEST(lex_single_amp_run_no_ampamp) {
    UASSERT_EQ((int)nth_tok("a & b & c", 1), (int)TOK_AMP);
    UASSERT_EQ((int)nth_tok("a & b & c", 3), (int)TOK_AMP);
}

void test_lex_operators_suite(void);
void test_lex_operators_suite(void) {
    utest_run("lex_amp_still_separator",        lex_amp_still_separator);
    utest_run("lex_pipe_still_separator",       lex_pipe_still_separator);
    utest_run("lex_ampamp",                     lex_ampamp);
    utest_run("lex_pipepipe",                   lex_pipepipe);
    utest_run("lex_percent",                    lex_percent);
    utest_run("lex_single_amp_run_no_ampamp",   lex_single_amp_run_no_ampamp);
}
