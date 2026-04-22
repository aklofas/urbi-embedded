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

/* --- Header parse tests --- */

/* Build a canonical v1.0 little-endian i64+f64 header.
   Used as a base in tests that mutate one byte. */
static void build_good_header(uint8_t hdr[24]) {
    size_t i;
    for (i = 0; i < 24; i++) hdr[i] = 0;
    hdr[0] = 'U'; hdr[1] = 'R'; hdr[2] = 'B'; hdr[3] = 'I'; /* magic */
    hdr[4] = 0x10;                         /* version v1.0 */
    hdr[5] = 0x00;                         /* flags */
    hdr[6]  = 0x19; hdr[7]  = 0x93;        /* canary bytes 0-1 */
    hdr[8]  = '\r'; hdr[9]  = '\n';        /* canary bytes 2-3 */
    hdr[10] = 0x1A; hdr[11] = '\n';        /* canary bytes 4-5 */
    hdr[12] = 8;                           /* int_width = i64 */
    hdr[13] = 8;                           /* float_type = f64 */
    hdr[14] = 4;                           /* instr_width = uint32 */
    hdr[15] = 0;                           /* endianness = little */
    /* hdr[16..23] reserved, zero */
}

UTEST(deserialize_accepts_good_header_with_empty_body_sections) {
    uint8_t buf[64];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    /* Minimal body: 6 zero-count varints (metadata + sections). */
    size_t offset = 24;
    buf[offset++] = 0;  /* max_reg */
    buf[offset++] = 0;  /* varint source_name_len = 0 */
    buf[offset++] = 0;  /* varint n_constants = 0 */
    buf[offset++] = 0;  /* varint n_instructions = 0 */
    buf[offset++] = 0;  /* varint n_deltas = 0 */
    buf[offset++] = 0;  /* varint n_abs_lines = 0 */
    Chunk c = {0};
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&c, buf, offset, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    uchunk_destroy(&c);
}

UTEST(deserialize_rejects_buffer_shorter_than_header) {
    uint8_t buf[10];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    Chunk c = {0};
    UASSERT_EQ(ULOAD_TRUNCATED, uchunk_deserialize(&c, buf, sizeof buf, NULL, 0));
    uchunk_destroy(&c);
}

UTEST(deserialize_rejects_bad_magic) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[0] = 'X';  /* corrupt first magic byte */
    Chunk c = {0};
    UASSERT_EQ(ULOAD_BAD_MAGIC, uchunk_deserialize(&c, hdr, sizeof hdr, NULL, 0));
    uchunk_destroy(&c);
}

UTEST(deserialize_rejects_corrupted_canary_simulated_ftp_ascii) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[8] = 0x20;  /* \r replaced by space — ASCII-mode munge */
    Chunk c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = uchunk_deserialize(&c, hdr, sizeof hdr, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_BAD_MAGIC, rc);
    UASSERT(strstr(errmsg, "canary") != NULL);
    uchunk_destroy(&c);
}

UTEST(deserialize_rejects_unsupported_version) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[4] = 0x20;  /* would be v2.0 */
    Chunk c = {0};
    UASSERT_EQ(ULOAD_UNSUPPORTED_VERSION, uchunk_deserialize(&c, hdr, sizeof hdr, NULL, 0));
    uchunk_destroy(&c);
}

UTEST(deserialize_rejects_wrong_int_width) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[12] = 4;  /* claims i32 */
    Chunk c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = uchunk_deserialize(&c, hdr, sizeof hdr, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_FLAVOR_MISMATCH, rc);
    UASSERT(strstr(errmsg, "int_width") != NULL);
    uchunk_destroy(&c);
}

UTEST(deserialize_rejects_wrong_float_type) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[13] = (URBI_FLOAT_TYPE == 8) ? 4 : 8;  /* flip to the other flavor */
    Chunk c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = uchunk_deserialize(&c, hdr, sizeof hdr, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_FLAVOR_MISMATCH, rc);
    UASSERT(strstr(errmsg, "float_type") != NULL);
    uchunk_destroy(&c);
}

UTEST(deserialize_rejects_wrong_instr_width) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[14] = 8;  /* claims 8-byte instructions */
    Chunk c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = uchunk_deserialize(&c, hdr, sizeof hdr, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_FLAVOR_MISMATCH, rc);
    UASSERT(strstr(errmsg, "instr_width") != NULL);
    uchunk_destroy(&c);
}

UTEST(deserialize_rejects_wrong_endianness) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[15] = 1;  /* big-endian on LE host */
    Chunk c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = uchunk_deserialize(&c, hdr, sizeof hdr, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_FLAVOR_MISMATCH, rc);
    UASSERT(strstr(errmsg, "endianness") != NULL);
    uchunk_destroy(&c);
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
    utest_run("deserialize accepts good header with empty body sections",
              deserialize_accepts_good_header_with_empty_body_sections);
    utest_run("deserialize rejects buffer shorter than header",
              deserialize_rejects_buffer_shorter_than_header);
    utest_run("deserialize rejects bad magic",
              deserialize_rejects_bad_magic);
    utest_run("deserialize rejects corrupted canary simulated FTP-ASCII",
              deserialize_rejects_corrupted_canary_simulated_ftp_ascii);
    utest_run("deserialize rejects unsupported version",
              deserialize_rejects_unsupported_version);
    utest_run("deserialize rejects wrong int_width",
              deserialize_rejects_wrong_int_width);
    utest_run("deserialize rejects wrong float_type",
              deserialize_rejects_wrong_float_type);
    utest_run("deserialize rejects wrong instr_width",
              deserialize_rejects_wrong_instr_width);
    utest_run("deserialize rejects wrong endianness",
              deserialize_rejects_wrong_endianness);
}
