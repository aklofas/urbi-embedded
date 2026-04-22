/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include "uchunk.h"
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

UTEST(chunk_error_name_ok) {
    UASSERT_EQ(0, strcmp("ULOAD_OK", uchunk_load_error_name(ULOAD_OK)));
}

UTEST(destroy_empty_chunk_is_noop) {
    Chunk c = {0};
    uchunk_destroy(&c);
    UASSERT_EQ((void *)NULL, (void *)c.instructions);
    UASSERT_EQ((void *)NULL, (void *)c.constants);
    UASSERT_EQ((size_t)0, c.instr_count);
    UASSERT_EQ((size_t)0, c.const_count);
}

UTEST(destroy_chunk_with_buffers_frees_them) {
    Chunk c = {0};
    /* Simulate allocation by directly using stdlib and letting destroy free.
       This is the same path uchunk_deserialize / uchunk_serialize use via
       the alloc_fn hook — when alloc_fn is NULL, destroy uses stdlib free. */
    c.instructions = (uint32_t *)malloc(sizeof(uint32_t) * 4);
    c.instr_cap = 4;
    c.instr_count = 2;
    c.instructions[0] = 0x11223344;
    c.instructions[1] = 0x55667788;

    c.constants = (UConst *)malloc(sizeof(UConst) * 2);
    c.const_cap = 2;
    c.const_count = 1;

    c.line_deltas = (int8_t *)malloc(sizeof(int8_t) * 4);

    c.abs_lines = (AbsLine *)malloc(sizeof(AbsLine) * 2);
    c.abs_line_cap = 2;
    c.abs_line_count = 1;

    uchunk_destroy(&c);

    UASSERT_EQ((void *)NULL, (void *)c.instructions);
    UASSERT_EQ((void *)NULL, (void *)c.constants);
    UASSERT_EQ((void *)NULL, (void *)c.line_deltas);
    UASSERT_EQ((void *)NULL, (void *)c.abs_lines);
    UASSERT_EQ((size_t)0, c.instr_count);
    UASSERT_EQ((size_t)0, c.const_count);
    UASSERT_EQ((size_t)0, c.abs_line_count);
}

void test_chunk_suite(void);

void test_chunk_suite(void) {
    utest_run("chunk error name ok",                   chunk_error_name_ok);
    utest_run("destroy empty chunk is a no-op",        destroy_empty_chunk_is_noop);
    utest_run("destroy chunk with allocated buffers frees them",
              destroy_chunk_with_buffers_frees_them);
}
