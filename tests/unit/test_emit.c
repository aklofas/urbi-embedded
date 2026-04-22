/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include "uemit.h"

#define UTEST(name) static void name(void)

UTEST(emit_placeholder) {
    UASSERT_EQ(EMIT_OK, uemit_finish(NULL));
}

void test_emit_suite(void);

void test_emit_suite(void) {
    utest_run("emit_placeholder", emit_placeholder);
}
