/* SPDX-License-Identifier: BSD-3-Clause */
/* test_lex_float_literals.c — Gap #5: float-literal lexing.
 *
 * Closes LEX-035 partial (string literals landed in v0.6.0; float literals
 * land here).  Decimal float only — hex floats are out of scope per the
 * v0.6.2 locked sub-decisions.
 *
 * Disambiguation rule: `0.foo` is INT(0) DOT IDENT(foo); only INT followed
 * by '.' then a digit promotes to TOK_FLOAT.  Leading-dot form `.5` is
 * dispatched from the top-level lex loop when the first char is '.'. */

#include "utest.h"
#include "lex/ulex.h"
#include <math.h>

/* ------------------------------------------------------------------ */
/* Happy-path tests                                                    */
/* ------------------------------------------------------------------ */

static void lex_float_basic_1_5(void) {
    /* 1.5 → TOK_FLOAT with value 1.5 */
    const char src[] = "1.5";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t = ulex_next(&lex);
    UASSERT_EQ(t.type, TOK_FLOAT);
    UASSERT(t.u.f == 1.5);
    UASSERT_EQ(ulex_next(&lex).type, TOK_EOF);
}

static void lex_float_leading_dot(void) {
    /* .5 → TOK_FLOAT with value 0.5 */
    const char src[] = ".5";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t = ulex_next(&lex);
    UASSERT_EQ(t.type, TOK_FLOAT);
    UASSERT(t.u.f == 0.5);
    UASSERT_EQ(ulex_next(&lex).type, TOK_EOF);
}

static void lex_float_zero_point_five(void) {
    /* 0.5 — leading zero with fraction digit (not ambiguous leading-zero) */
    const char src[] = "0.5";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t = ulex_next(&lex);
    UASSERT_EQ(t.type, TOK_FLOAT);
    UASSERT(t.u.f == 0.5);
    UASSERT_EQ(ulex_next(&lex).type, TOK_EOF);
}

static void lex_float_exponent_upper(void) {
    /* 1.5E3 → 1500.0 */
    const char src[] = "1.5E3";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t = ulex_next(&lex);
    UASSERT_EQ(t.type, TOK_FLOAT);
    UASSERT(t.u.f == 1500.0);
    UASSERT_EQ(ulex_next(&lex).type, TOK_EOF);
}

static void lex_float_exponent_lower(void) {
    /* 1.5e3 → 1500.0 */
    const char src[] = "1.5e3";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t = ulex_next(&lex);
    UASSERT_EQ(t.type, TOK_FLOAT);
    UASSERT(t.u.f == 1500.0);
    UASSERT_EQ(ulex_next(&lex).type, TOK_EOF);
}

static void lex_float_exponent_negative(void) {
    /* 1.5e-3 → 0.0015 */
    const char src[] = "1.5e-3";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t = ulex_next(&lex);
    UASSERT_EQ(t.type, TOK_FLOAT);
    UASSERT(fabs(t.u.f - 0.0015) < 1e-15);
    UASSERT_EQ(ulex_next(&lex).type, TOK_EOF);
}

static void lex_float_integer_with_exponent(void) {
    /* 1e3 → 1000.0 — no decimal point; exponent alone triggers TOK_FLOAT */
    const char src[] = "1e3";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t = ulex_next(&lex);
    UASSERT_EQ(t.type, TOK_FLOAT);
    UASSERT(t.u.f == 1000.0);
    UASSERT_EQ(ulex_next(&lex).type, TOK_EOF);
}

/* ------------------------------------------------------------------ */
/* Disambiguation tests                                                */
/* ------------------------------------------------------------------ */

static void lex_float_disambig_zero_dot_ident(void) {
    /* 0.foo must NOT become a float literal.
     * Disambiguation rule: INT followed by '.' then a non-digit keeps
     * INT(0) DOT IDENT(foo). */
    const char src[] = "0.foo";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t1 = ulex_next(&lex);
    UASSERT_EQ(t1.type, TOK_INT);
    UASSERT_EQ(t1.u.i, 0);
    UToken t2 = ulex_next(&lex);
    UASSERT_EQ(t2.type, TOK_DOT);
    UToken t3 = ulex_next(&lex);
    UASSERT_EQ(t3.type, TOK_IDENT);
    UASSERT_EQ(ulex_next(&lex).type, TOK_EOF);
}

static void lex_float_plain_int_unchanged(void) {
    /* 42 stays TOK_INT — no regression */
    const char src[] = "42";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t = ulex_next(&lex);
    UASSERT_EQ(t.type, TOK_INT);
    UASSERT_EQ(t.u.i, 42);
    UASSERT_EQ(ulex_next(&lex).type, TOK_EOF);
}

/* ------------------------------------------------------------------ */
/* Error-path tests                                                    */
/* ------------------------------------------------------------------ */

static void lex_float_error_trailing_dot(void) {
    /* 1. — no fraction digits after dot → LEX_FLOAT_TRAILING_DOT */
    const char src[] = "1.";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t = ulex_next(&lex);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_FLOAT_TRAILING_DOT);
    /* Recovery: cursor advanced past the literal */
    UASSERT_EQ(ulex_next(&lex).type, TOK_EOF);
}

static void lex_float_error_exponent_no_digits(void) {
    /* 1.5e+ — exponent marker present but no digits follow */
    const char src[] = "1.5e+";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t = ulex_next(&lex);
    UASSERT_EQ(t.type, TOK_ERROR);
    UASSERT_EQ(t.u.err.code, LEX_FLOAT_EXPONENT_NO_DIGITS);
    UASSERT_EQ(ulex_next(&lex).type, TOK_EOF);
}

static void lex_float_error_leading_dot_no_digits(void) {
    /* .e3 — leading dot not followed by a digit is NOT a float; the dot
     * becomes TOK_DOT, and e3 is an identifier.  Only a leading dot
     * followed by a decimal digit triggers TOK_FLOAT. */
    const char src[] = ".e3";
    ULexer lex;
    ulex_init(&lex, src, sizeof(src) - 1);
    UToken t1 = ulex_next(&lex);
    UASSERT_EQ(t1.type, TOK_DOT);
    UToken t2 = ulex_next(&lex);
    UASSERT_EQ(t2.type, TOK_IDENT);
    UASSERT_EQ(ulex_next(&lex).type, TOK_EOF);
}

void test_lex_float_literals_suite(void) {
    utest_run("lex_float_basic_1_5",               lex_float_basic_1_5);
    utest_run("lex_float_leading_dot",              lex_float_leading_dot);
    utest_run("lex_float_zero_point_five",          lex_float_zero_point_five);
    utest_run("lex_float_exponent_upper",           lex_float_exponent_upper);
    utest_run("lex_float_exponent_lower",           lex_float_exponent_lower);
    utest_run("lex_float_exponent_negative",        lex_float_exponent_negative);
    utest_run("lex_float_integer_with_exponent",    lex_float_integer_with_exponent);
    utest_run("lex_float_disambig_zero_dot_ident",  lex_float_disambig_zero_dot_ident);
    utest_run("lex_float_plain_int_unchanged",      lex_float_plain_int_unchanged);
    utest_run("lex_float_error_trailing_dot",       lex_float_error_trailing_dot);
    utest_run("lex_float_error_exponent_no_digits", lex_float_error_exponent_no_digits);
    utest_run("lex_float_error_leading_dot_no_digits", lex_float_error_leading_dot_no_digits);
}
