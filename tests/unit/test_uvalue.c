/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for src/uvalue.c — UValue formatter. */

#include "utest.h"

#include "module/umodule.h"
#include "value/uvalue.h"

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

UTEST(uvalue_str_tab_escape) {
    UValue v = make_str("a\tb");
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "\"a\\tb\"");
}

UTEST(uvalue_str_cr_escape) {
    UValue v = make_str("a\rb");
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "\"a\\rb\"");
}

UTEST(uvalue_str_hex_escape) {
    UValue v = make_str("a\x01""b");  /* 0x01 is non-printable */
    char buf[32] = {0};
    uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "\"a\\x01b\"");
}

/* --- Truncation + unknown kind --- */

UTEST(uvalue_truncation_cap_zero) {
    UValue v = { .kind = UVAL_BOOL };
    v.v.i = 1;
    char buf[4] = { 'x', 'x', 'x', 'x' };
    size_t n = uvalue_format(&v, buf, 0);
    UASSERT_EQ(0, (int)n);
    /* cap=0 means we don't touch buf[0] — no NUL-write guarantee here;
       the API contract says we NUL-terminate only when cap > 0. */
    UASSERT_EQ('x', buf[0]);
}

UTEST(uvalue_truncation_cap_one) {
    UValue v = { .kind = UVAL_BOOL };
    v.v.i = 1;
    char buf[8] = { 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x' };
    size_t n = uvalue_format(&v, buf, 1);
    /* cap=1 -> only NUL fits */
    UASSERT_EQ(0, (int)n);
    UASSERT_EQ('\0', buf[0]);
}

UTEST(uvalue_truncation_cap_three) {
    UValue v = { .kind = UVAL_BOOL };
    v.v.i = 1;
    char buf[4] = {0};
    size_t n = uvalue_format(&v, buf, 3);
    /* cap=3 means 2 chars + NUL; "true" truncated to "tr" */
    UASSERT_EQ(2, (int)n);
    UASSERT_STR_EQ(buf, "tr");
}

UTEST(uvalue_unknown_kind) {
    UValue v = { .kind = 99 };  /* outside the valid 0..4 range */
    char buf[16] = {0};
    size_t n = uvalue_format(&v, buf, sizeof buf);
    UASSERT_STR_EQ(buf, "<?>");
    UASSERT_EQ(3, (int)n);
}

/* --- uvalue_truthy --- */

UTEST(uvalue_truthy_nil_is_false) {
    UValue v = { .kind = UVAL_NIL };
    UASSERT(!uvalue_truthy(&v));
}

UTEST(uvalue_truthy_bool_false_is_false) {
    UValue v = { .kind = UVAL_BOOL }; v.v.i = 0;
    UASSERT(!uvalue_truthy(&v));
}

UTEST(uvalue_truthy_bool_true_is_true) {
    UValue v = { .kind = UVAL_BOOL }; v.v.i = 1;
    UASSERT(uvalue_truthy(&v));
}

UTEST(uvalue_truthy_void_is_false) {
    UValue v = { .kind = UVAL_VOID };
    UASSERT(!uvalue_truthy(&v));
}

UTEST(uvalue_truthy_int_zero_is_true) {
    /* Per urbiscript: int 0 is truthy — only nil/false/void are falsy. */
    UValue v = { .kind = UVAL_INT }; v.v.i = 0;
    UASSERT(uvalue_truthy(&v));
}

UTEST(uvalue_truthy_int_nonzero_is_true) {
    UValue v = { .kind = UVAL_INT }; v.v.i = 42;
    UASSERT(uvalue_truthy(&v));
}

UTEST(uvalue_truthy_float_zero_is_true) {
    /* Float 0.0 is truthy in urbiscript. */
    UValue v = { .kind = UVAL_FLOAT }; v.v.f = 0.0;
    UASSERT(uvalue_truthy(&v));
}

/* --- uvalue_equal --- */

UTEST(uvalue_equal_nil_eq_nil) {
    UValue a = { .kind = UVAL_NIL };
    UValue b = { .kind = UVAL_NIL };
    UASSERT(uvalue_equal(&a, &b));
}

UTEST(uvalue_equal_int_eq_int_same) {
    UValue a = { .kind = UVAL_INT }; a.v.i = 7;
    UValue b = { .kind = UVAL_INT }; b.v.i = 7;
    UASSERT(uvalue_equal(&a, &b));
}

UTEST(uvalue_equal_int_eq_int_diff) {
    UValue a = { .kind = UVAL_INT }; a.v.i = 7;
    UValue b = { .kind = UVAL_INT }; b.v.i = 8;
    UASSERT(!uvalue_equal(&a, &b));
}

UTEST(uvalue_equal_cross_kind_int_float_true) {
    UValue a = { .kind = UVAL_INT };   a.v.i = 1;
    UValue b = { .kind = UVAL_FLOAT }; b.v.f = 1.0;
    UASSERT(uvalue_equal(&a, &b));
}

UTEST(uvalue_equal_cross_kind_float_int_true) {
    UValue a = { .kind = UVAL_FLOAT }; a.v.f = 2.0;
    UValue b = { .kind = UVAL_INT };   b.v.i = 2;
    UASSERT(uvalue_equal(&a, &b));
}

UTEST(uvalue_equal_cross_kind_int_float_false) {
    UValue a = { .kind = UVAL_INT };   a.v.i = 1;
    UValue b = { .kind = UVAL_FLOAT }; b.v.f = 2.5;
    UASSERT(!uvalue_equal(&a, &b));
}

UTEST(uvalue_equal_void_void_is_false) {
    /* void != void per spec — void is never equal to anything. */
    UValue a = { .kind = UVAL_VOID };
    UValue b = { .kind = UVAL_VOID };
    UASSERT(!uvalue_equal(&a, &b));
}

UTEST(uvalue_equal_int_vs_nil_is_false) {
    UValue a = { .kind = UVAL_INT }; a.v.i = 0;
    UValue b = { .kind = UVAL_NIL };
    UASSERT(!uvalue_equal(&a, &b));
}

/* --- uvalue_lt --- */

UTEST(uvalue_lt_int_vs_int) {
    UValue a = { .kind = UVAL_INT }; a.v.i = 1;
    UValue b = { .kind = UVAL_INT }; b.v.i = 2;
    bool out = false;
    UASSERT_EQ((int)UVAL_CMP_OK, (int)uvalue_lt(&a, &b, &out));
    UASSERT(out);
}

UTEST(uvalue_lt_int_vs_float) {
    UValue a = { .kind = UVAL_INT };   a.v.i = 1;
    UValue b = { .kind = UVAL_FLOAT }; b.v.f = 2.5;
    bool out = false;
    UASSERT_EQ((int)UVAL_CMP_OK, (int)uvalue_lt(&a, &b, &out));
    UASSERT(out);
}

UTEST(uvalue_lt_non_numeric_returns_type_error) {
    UValue a = { .kind = UVAL_NIL };
    UValue b = { .kind = UVAL_INT }; b.v.i = 1;
    bool out = false;
    UASSERT_EQ((int)UVAL_CMP_TYPE_ERROR, (int)uvalue_lt(&a, &b, &out));
}

/* --- uvalue_le --- */

UTEST(uvalue_le_equal_ints) {
    UValue a = { .kind = UVAL_INT }; a.v.i = 3;
    UValue b = { .kind = UVAL_INT }; b.v.i = 3;
    bool out = false;
    UASSERT_EQ((int)UVAL_CMP_OK, (int)uvalue_le(&a, &b, &out));
    UASSERT(out);
}

UTEST(uvalue_le_float_vs_int) {
    UValue a = { .kind = UVAL_FLOAT }; a.v.f = 1.5;
    UValue b = { .kind = UVAL_INT };   b.v.i = 2;
    bool out = false;
    UASSERT_EQ((int)UVAL_CMP_OK, (int)uvalue_le(&a, &b, &out));
    UASSERT(out);
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
    utest_run("uvalue: str \\t escape", uvalue_str_tab_escape);
    utest_run("uvalue: str \\r escape", uvalue_str_cr_escape);
    utest_run("uvalue: str \\xNN hex escape", uvalue_str_hex_escape);
    utest_run("uvalue: truncation cap=0", uvalue_truncation_cap_zero);
    utest_run("uvalue: truncation cap=1", uvalue_truncation_cap_one);
    utest_run("uvalue: truncation cap=3", uvalue_truncation_cap_three);
    utest_run("uvalue: unknown kind -> <?>", uvalue_unknown_kind);
    /* truthy rules */
    utest_run("uvalue_truthy: nil → false",        uvalue_truthy_nil_is_false);
    utest_run("uvalue_truthy: bool false → false",  uvalue_truthy_bool_false_is_false);
    utest_run("uvalue_truthy: bool true → true",    uvalue_truthy_bool_true_is_true);
    utest_run("uvalue_truthy: void → false",        uvalue_truthy_void_is_false);
    utest_run("uvalue_truthy: int 0 → true (urbiscript semantics)", uvalue_truthy_int_zero_is_true);
    utest_run("uvalue_truthy: int 42 → true",       uvalue_truthy_int_nonzero_is_true);
    utest_run("uvalue_truthy: float 0.0 → true",    uvalue_truthy_float_zero_is_true);
    /* equal */
    utest_run("uvalue_equal: nil == nil → true",    uvalue_equal_nil_eq_nil);
    utest_run("uvalue_equal: 7 == 7 → true",        uvalue_equal_int_eq_int_same);
    utest_run("uvalue_equal: 7 == 8 → false",       uvalue_equal_int_eq_int_diff);
    utest_run("uvalue_equal: 1(INT) == 1.0(FLOAT) → true",  uvalue_equal_cross_kind_int_float_true);
    utest_run("uvalue_equal: 2.0(FLOAT) == 2(INT) → true",  uvalue_equal_cross_kind_float_int_true);
    utest_run("uvalue_equal: 1(INT) == 2.5(FLOAT) → false", uvalue_equal_cross_kind_int_float_false);
    utest_run("uvalue_equal: void == void → false (per spec)", uvalue_equal_void_void_is_false);
    utest_run("uvalue_equal: 0(INT) == nil → false",uvalue_equal_int_vs_nil_is_false);
    /* lt */
    utest_run("uvalue_lt: 1 < 2 → ok true",        uvalue_lt_int_vs_int);
    utest_run("uvalue_lt: 1(INT) < 2.5(FLOAT) → ok true", uvalue_lt_int_vs_float);
    utest_run("uvalue_lt: nil < 1 → type error",   uvalue_lt_non_numeric_returns_type_error);
    /* le */
    utest_run("uvalue_le: 3 <= 3 → ok true",        uvalue_le_equal_ints);
    utest_run("uvalue_le: 1.5(FLOAT) <= 2(INT) → ok true", uvalue_le_float_vs_int);
}
