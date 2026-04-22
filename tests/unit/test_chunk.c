/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include "uchunk.h"

#define UTEST(name) static void name(void)

UTEST(chunk_placeholder) {
    UASSERT_EQ(ULOAD_OK, uchunk_deserialize(NULL, NULL, 0, NULL, 0));
}

void test_chunk_suite(void);

void test_chunk_suite(void) {
    utest_run("chunk_placeholder", chunk_placeholder);
}
