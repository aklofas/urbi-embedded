/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for src/uvalue.c — UValue formatter. */

#include "utest.h"

#include "umodule.h"
#include "uvalue.h"

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

void test_uvalue_suite(void) {
    utest_run("uvalue: nil -> \"nil\"", uvalue_nil_formats_as_nil);
    utest_run("uvalue: bool true", uvalue_bool_true);
    utest_run("uvalue: bool false", uvalue_bool_false);
    utest_run("uvalue: int 0", uvalue_int_zero);
    utest_run("uvalue: int 42", uvalue_int_positive);
    utest_run("uvalue: int -42", uvalue_int_negative);
    utest_run("uvalue: int INT64_MAX", uvalue_int_max);
    utest_run("uvalue: int INT64_MIN", uvalue_int_min);
}
