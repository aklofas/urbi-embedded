/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include "uchunk.h"
#include "uchunk_internal.h"
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

/* --- Varint tests (helpers exposed via uchunk_internal.h) --- */

UTEST(varint_decode_u_single_byte) {
    const uint8_t buf[] = {0x00};
    uint64_t v = 0;
    size_t consumed = 0;
    UASSERT_EQ(ULOAD_OK, varint_decode_u(buf, sizeof buf, &v, &consumed));
    UASSERT_EQ((uint64_t)0, v);
    UASSERT_EQ((size_t)1, consumed);

    const uint8_t buf2[] = {0x7F};          /* 127 — max single-byte */
    UASSERT_EQ(ULOAD_OK, varint_decode_u(buf2, sizeof buf2, &v, &consumed));
    UASSERT_EQ((uint64_t)127, v);
    UASSERT_EQ((size_t)1, consumed);
}

UTEST(varint_decode_u_multi_byte) {
    /* 128 = 0x80 0x01  — low 7 bits (0) with continuation, then high bits (1) */
    const uint8_t buf[] = {0x80, 0x01};
    uint64_t v = 0;
    size_t consumed = 0;
    UASSERT_EQ(ULOAD_OK, varint_decode_u(buf, sizeof buf, &v, &consumed));
    UASSERT_EQ((uint64_t)128, v);
    UASSERT_EQ((size_t)2, consumed);

    /* 300 = 0xAC 0x02 */
    const uint8_t buf2[] = {0xAC, 0x02};
    UASSERT_EQ(ULOAD_OK, varint_decode_u(buf2, sizeof buf2, &v, &consumed));
    UASSERT_EQ((uint64_t)300, v);
    UASSERT_EQ((size_t)2, consumed);
}

UTEST(varint_decode_u_truncation) {
    const uint8_t buf[] = {0x80};           /* continuation set, no next byte */
    uint64_t v = 0;
    size_t consumed = 0;
    UASSERT_EQ(ULOAD_TRUNCATED, varint_decode_u(buf, sizeof buf, &v, &consumed));
}

UTEST(varint_decode_u_oversize) {
    /* 11 continuation bytes — exceeds 10-byte max for uint64 */
    const uint8_t buf[] = {0x80, 0x80, 0x80, 0x80, 0x80,
                           0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
    uint64_t v = 0;
    size_t consumed = 0;
    UASSERT_EQ(ULOAD_CORRUPT_VARINT, varint_decode_u(buf, sizeof buf, &v, &consumed));
}

UTEST(varint_decode_zz_round_trip) {
    /* zigzag: 0->0, -1->1, 1->2, -2->3, 2->4, ... */
    struct { int64_t decoded; uint8_t enc[10]; size_t enc_len; } cases[] = {
        { 0,   {0x00},       1 },
        { -1,  {0x01},       1 },
        { 1,   {0x02},       1 },
        { -2,  {0x03},       1 },
        { 2,   {0x04},       1 },
        { 63,  {0x7E},       1 },   /* 63*2 = 126 */
        { -64, {0x7F},       1 },   /* zigzag(-64) = 127 */
        { 64,  {0x80, 0x01}, 2 },   /* 64*2 = 128 */
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int64_t v = 0;
        size_t consumed = 0;
        UChunkLoadError rc = varint_decode_zz(cases[i].enc, cases[i].enc_len, &v, &consumed);
        UASSERT_EQ(ULOAD_OK, rc);
        UASSERT_EQ(cases[i].decoded, v);
        UASSERT_EQ(cases[i].enc_len, consumed);
    }
}

void test_chunk_suite(void);

void test_chunk_suite(void) {
    utest_run("chunk error name ok",                   chunk_error_name_ok);
    utest_run("destroy empty chunk is a no-op",        destroy_empty_chunk_is_noop);
    utest_run("destroy chunk with allocated buffers frees them",
              destroy_chunk_with_buffers_frees_them);
    utest_run("varint decode u single byte",            varint_decode_u_single_byte);
    utest_run("varint decode u multi byte",             varint_decode_u_multi_byte);
    utest_run("varint decode u truncation",             varint_decode_u_truncation);
    utest_run("varint decode u oversize",               varint_decode_u_oversize);
    utest_run("varint decode zz round trip",            varint_decode_zz_round_trip);
}
