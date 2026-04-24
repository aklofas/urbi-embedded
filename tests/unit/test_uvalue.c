/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for src/uvalue.c — UValue formatter. */

#include "utest.h"

#include "umodule.h"
#include "uvalue.h"

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

void test_uvalue_suite(void) {
    utest_run("uvalue: nil -> \"nil\"", uvalue_nil_formats_as_nil);
}
