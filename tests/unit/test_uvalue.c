/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for src/uvalue.c — UValue formatter. */

#include "utest.h"

#include "umodule.h"
#include "uvalue.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* --- UVAL_NIL --- */

UTEST(uvalue_nil_formats_as_nil) {
    UValue v = { .kind = UVAL_NIL };
    char buf[16] = {0};
    size_t n = uvalue_format(&v, buf, sizeof buf);
    UASSERT_EQ(3, (int)n);
    UASSERT_STR_EQ(buf, "nil");
}

/* --- UVAL_BOOL --- */

UTEST(uvalue_bool_true) {
    UValue v = { .kind = UVAL_BOOL };
    v.v.i = 1;
    char buf[16] = {0};
    size_t n = uvalue_format(&v, buf, sizeof buf);
    UASSERT_EQ(4, (int)n);
    UASSERT_STR_EQ(buf, "true");
}

UTEST(uvalue_bool_false) {
    UValue v = { .kind = UVAL_BOOL };
    v.v.i = 0;
    char buf[16] = {0};
    size_t n = uvalue_format(&v, buf, sizeof buf);
    UASSERT_EQ(5, (int)n);
    UASSERT_STR_EQ(buf, "false");
}

/* --- UVAL_INT --- */

UTEST(uvalue_int_zero) {
    UValue v = { .kind = UVAL_INT };
    v.v.i = 0;
    char buf[32] = {0};
    size_t n = uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "0");
    UASSERT_EQ(1, (int)n);
}

UTEST(uvalue_int_positive) {
    UValue v = { .kind = UVAL_INT };
    v.v.i = 42;
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "42");
}

UTEST(uvalue_int_negative) {
    UValue v = { .kind = UVAL_INT };
    v.v.i = -42;
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "-42");
}

UTEST(uvalue_int_max) {
    UValue v = { .kind = UVAL_INT };
    v.v.i = INT64_MAX;
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "9223372036854775807");
}

UTEST(uvalue_int_min) {
    UValue v = { .kind = UVAL_INT };
    v.v.i = INT64_MIN;
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "-9223372036854775808");
}

/* --- UVAL_FLOAT ---
 * Lua-5.4-style: %.14g (f64) or %.7g (f32), with trailing ".0" appended when
 * the formatted output contains none of '.', 'e', 'E', 'n', 'i'.  This makes
 * whole-number floats visually distinct from integers. */

UTEST(uvalue_float_two_point_five) {
    UValue v = { .kind = UVAL_FLOAT };
    v.v.f = 2.5;
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "2.5");
}

UTEST(uvalue_float_whole_gets_dot_zero) {
    UValue v = { .kind = UVAL_FLOAT };
    v.v.f = 3.0;
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "3.0");
}

UTEST(uvalue_float_negative_zero) {
    UValue v = { .kind = UVAL_FLOAT };
    v.v.f = -0.0;
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "-0.0");
}

UTEST(uvalue_float_scientific_no_append) {
    UValue v = { .kind = UVAL_FLOAT };
    v.v.f = 1.5e+20;
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    /* result contains 'e' so .0 is NOT appended */
    UASSERT(strchr(buf, 'e') != NULL);
    UASSERT(strstr(buf, ".0") == NULL || strstr(buf, ".0") < strchr(buf, 'e'));
}

UTEST(uvalue_float_nan) {
    UValue v = { .kind = UVAL_FLOAT };
    v.v.f = (double)NAN;
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    /* snprintf produces "nan" on glibc; scan rule sees 'n' and skips append */
    UASSERT(strstr(buf, "nan") != NULL);
    UASSERT(strstr(buf, "nan.0") == NULL);
}

UTEST(uvalue_float_pos_inf) {
    UValue v = { .kind = UVAL_FLOAT };
    v.v.f = (double)INFINITY;
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT(strstr(buf, "inf") != NULL);
    UASSERT(strstr(buf, "inf.0") == NULL);
}

UTEST(uvalue_float_neg_inf) {
    UValue v = { .kind = UVAL_FLOAT };
    v.v.f = -(double)INFINITY;
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT(strstr(buf, "-inf") != NULL);
}

/* --- UVAL_STR ---
 * M1 placeholder: UValue carries the string pointer in the same slot as
 * v.i, reinterpreted.  Replace with a proper union member at M2 when
 * string literals become reachable. */

static UValue make_str(const char *s) {
    UValue v = { .kind = UVAL_STR };
    v.v.i = (int64_t)(intptr_t)s;
    return v;
}

UTEST(uvalue_str_empty) {
    UValue v = make_str("");
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "\"\"");
}

UTEST(uvalue_str_printable) {
    UValue v = make_str("hello");
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "\"hello\"");
}

UTEST(uvalue_str_newline_escape) {
    UValue v = make_str("a\nb");
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "\"a\\nb\"");
}

UTEST(uvalue_str_backslash_escape) {
    UValue v = make_str("a\\b");
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "\"a\\\\b\"");
}

UTEST(uvalue_str_quote_escape) {
    UValue v = make_str("a\"b");
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "\"a\\\"b\"");
}

void test_uvalue_suite(void) {
    utest_run("uvalue: nil -> \"nil\"", uvalue_nil_formats_as_nil);
    utest_run("uvalue: bool true", uvalue_bool_true);
    utest_run("uvalue: bool false", uvalue_bool_false);
    utest_run("uvalue: int 0", uvalue_int_zero);
    utest_run("uvalue: int 42", uvalue_int_positive);
    utest_run("uvalue: int -42", uvalue_int_negative);
    utest_run("uvalue: int INT64_MAX", uvalue_int_max);
    utest_run("uvalue: int INT64_MIN", uvalue_int_min);
    utest_run("uvalue: float 2.5", uvalue_float_two_point_five);
    utest_run("uvalue: float 3.0 -> 3.0", uvalue_float_whole_gets_dot_zero);
    utest_run("uvalue: float -0.0 preserves sign", uvalue_float_negative_zero);
    utest_run("uvalue: float 1.5e+20 no .0 append", uvalue_float_scientific_no_append);
    utest_run("uvalue: float NaN", uvalue_float_nan);
    utest_run("uvalue: float +Inf", uvalue_float_pos_inf);
    utest_run("uvalue: float -Inf", uvalue_float_neg_inf);
    utest_run("uvalue: str empty", uvalue_str_empty);
    utest_run("uvalue: str printable", uvalue_str_printable);
    utest_run("uvalue: str \\n escape", uvalue_str_newline_escape);
    utest_run("uvalue: str \\\\ escape", uvalue_str_backslash_escape);
    utest_run("uvalue: str \\\" escape", uvalue_str_quote_escape);
}
