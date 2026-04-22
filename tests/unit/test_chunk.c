/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include "uchunk.h"
#include "uchunk_internal.h"
#include "uarena.h"
#include "uemit.h"
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

/* --- Varint write helpers for building test blobs --- */

/* Append an LEB128 unsigned varint.  Returns new offset. */
static size_t put_varint(uint8_t *buf, size_t offset, uint64_t v) {
    while (v >= 0x80u) {
        buf[offset++] = (uint8_t)((v & 0x7Fu) | 0x80u);
        v >>= 7;
    }
    buf[offset++] = (uint8_t)v;
    return offset;
}

/* Append a signed zigzag varint. */
static size_t put_varint_zz(uint8_t *buf, size_t offset, int64_t v) {
    uint64_t u = ((uint64_t)v << 1) ^ (uint64_t)(v >> 63);
    return put_varint(buf, offset, u);
}

/* --- Body-decode tests (Task 5) --- */

UTEST(deserialize_loads_metadata_max_reg_and_source_name) {
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    /* metadata: max_reg=5, source_name="repl" */
    buf[off++] = 5;
    off = put_varint(buf, off, 4);   /* source_name_len = 4 */
    memcpy(buf + off, "repl", 4); off += 4;
    /* constants: 0 */
    off = put_varint(buf, off, 0);
    /* instructions: 0 */
    off = put_varint(buf, off, 0);
    /* synclines: 0 deltas, 0 abs_lines */
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 0);

    Chunk c = {0};
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    UASSERT_EQ((uint8_t)5, c.max_reg);
    UASSERT(c.source_name != NULL);
    UASSERT_EQ(0, strcmp(c.source_name, "repl"));
    uchunk_destroy(&c);
}

UTEST(deserialize_loads_integer_constant_pool) {
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    /* metadata: max_reg=0, no source_name */
    buf[off++] = 0;
    off = put_varint(buf, off, 0);
    /* constants: 2 entries — UVAL_INT 1, UVAL_INT -42 */
    off = put_varint(buf, off, 2);
    buf[off++] = (uint8_t)UVAL_INT;
    off = put_varint_zz(buf, off, 1);
    buf[off++] = (uint8_t)UVAL_INT;
    off = put_varint_zz(buf, off, -42);
    /* instructions: 0 */
    off = put_varint(buf, off, 0);
    /* synclines: 0, 0 */
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 0);

    Chunk c = {0};
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    UASSERT_EQ((size_t)2, c.const_count);
    UASSERT_EQ((uint8_t)UVAL_INT, c.constants[0].kind);
    UASSERT_EQ((int64_t)1,   c.constants[0].v.i);
    UASSERT_EQ((uint8_t)UVAL_INT, c.constants[1].kind);
    UASSERT_EQ((int64_t)-42, c.constants[1].v.i);
    uchunk_destroy(&c);
}

UTEST(deserialize_rejects_out_of_range_uconst_tag) {
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    buf[off++] = 0;                         /* max_reg */
    off = put_varint(buf, off, 0);          /* source_name_len */
    off = put_varint(buf, off, 1);          /* 1 constant */
    buf[off++] = 99;                        /* invalid kind */
    off = put_varint_zz(buf, off, 0);       /* payload (ignored, rejected first) */
    Chunk c = {0};
    UASSERT_EQ(ULOAD_CORRUPT_TAG, uchunk_deserialize(&c, buf, off, NULL, 0));
    uchunk_destroy(&c);
}

/* --- Instruction-stream + syncline tests (Task 5 deferred + Task 6) --- */

UTEST(deserialize_loads_instruction_stream_with_4_byte_alignment) {
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    /* metadata: max_reg=1, no source name */
    buf[off++] = 1;
    off = put_varint(buf, off, 0);
    /* constants: 0 */
    off = put_varint(buf, off, 0);
    /* instructions: 1.  varint 1 = 1 byte.  off before = 27; 27 mod 4 = 3, pad 1 byte. */
    off = put_varint(buf, off, 1);
    while ((off & 3u) != 0u) buf[off++] = 0;
    /* OP_RET R0: op=7, A=0, B=0, C=0 */
    const uint32_t instr = (uint32_t)OP_RET;
    buf[off + 0] = (uint8_t)(instr & 0xFF);
    buf[off + 1] = (uint8_t)((instr >> 8)  & 0xFF);
    buf[off + 2] = (uint8_t)((instr >> 16) & 0xFF);
    buf[off + 3] = (uint8_t)((instr >> 24) & 0xFF);
    off += 4;
    /* synclines: n_deltas=1, 1 abs_line (pc=0, line=5) */
    off = put_varint(buf, off, 1);
    buf[off++] = (uint8_t)(int8_t)-128;     /* INT8_MIN sentinel */
    off = put_varint(buf, off, 1);
    off = put_varint(buf, off, 0);          /* abs_line[0].pc = 0 */
    off = put_varint(buf, off, 5);          /* abs_line[0].line = 5 */

    Chunk c = {0};
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    UASSERT_EQ((size_t)1, c.instr_count);
    UASSERT_EQ((UOpcode)OP_RET, uinstr_op(c.instructions[0]));
    uchunk_destroy(&c);
}

UTEST(deserialize_rejects_non_zero_alignment_padding) {
    /* To force a padding byte, we need off to be non-4-aligned after the
       n_instructions varint.  Use source_name_len=1 ("x") to shift layout:
       24 (hdr) + 1 (max_reg) + 1 (src_len varint 1) + 1 ("x") +
       1 (n_constants=0) + 1 (n_instructions=1) = off=29; 29 mod 4 = 1,
       so 3 pad bytes are needed. */
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    buf[off++] = 0;                         /* max_reg */
    off = put_varint(buf, off, 1);          /* source_name_len = 1 */
    buf[off++] = 'x';                       /* source_name = "x" */
    off = put_varint(buf, off, 0);          /* n_constants */
    off = put_varint(buf, off, 1);          /* n_instructions=1 */
    /* off is now 29 (29 mod 4 == 1); 3 pad bytes needed — corrupt them */
    while ((off & 3u) != 0u) buf[off++] = 0xFF;
    /* instruction body (alignment check fires before reading this) */
    buf[off + 0] = (uint8_t)OP_RET;
    buf[off + 1] = 0; buf[off + 2] = 0; buf[off + 3] = 0;
    off += 4;
    /* minimal synclines to avoid trailing-bytes complaint */
    off = put_varint(buf, off, 1);
    buf[off++] = 0;
    off = put_varint(buf, off, 0);

    Chunk c = {0};
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "align") != NULL);
    uchunk_destroy(&c);
}

UTEST(deserialize_loads_delta_synclines_and_abs_checkpoints) {
    uint8_t buf[256];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    /* metadata */
    buf[off++] = 1;
    off = put_varint(buf, off, 0);
    /* constants: 0 */
    off = put_varint(buf, off, 0);
    /* instructions: 3 */
    off = put_varint(buf, off, 3);
    while ((off & 3u) != 0u) buf[off++] = 0;
    {
        int j;
        for (j = 0; j < 3; j++) {
            const uint32_t ins = (uint32_t)OP_RET;
            buf[off + 0] = (uint8_t)(ins & 0xFF);
            buf[off + 1] = (uint8_t)((ins >> 8)  & 0xFF);
            buf[off + 2] = (uint8_t)((ins >> 16) & 0xFF);
            buf[off + 3] = (uint8_t)((ins >> 24) & 0xFF);
            off += 4;
        }
    }
    /* synclines: 3 deltas (INT8_MIN, +2, -1), 1 abs checkpoint (pc=0, line=10) */
    off = put_varint(buf, off, 3);
    buf[off++] = (uint8_t)(int8_t)-128;     /* INT8_MIN */
    buf[off++] = (uint8_t)(int8_t)2;        /* +2 */
    buf[off++] = (uint8_t)(int8_t)-1;       /* -1 */
    off = put_varint(buf, off, 1);
    off = put_varint(buf, off, 0);          /* abs_line[0].pc = 0 */
    off = put_varint(buf, off, 10);         /* abs_line[0].line = 10 */

    Chunk c = {0};
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    UASSERT_EQ((size_t)3, c.instr_count);
    UASSERT_EQ((int8_t)-128, c.line_deltas[0]);
    UASSERT_EQ((int8_t)2,    c.line_deltas[1]);
    UASSERT_EQ((int8_t)-1,   c.line_deltas[2]);
    UASSERT_EQ((size_t)1, c.abs_line_count);
    UASSERT_EQ((uint32_t)0,  c.abs_lines[0].pc);
    UASSERT_EQ((uint32_t)10, c.abs_lines[0].line);
    uchunk_destroy(&c);
}

UTEST(deserialize_rejects_n_deltas_not_equal_n_instructions) {
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    buf[off++] = 0;
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 1);          /* 1 instruction */
    while ((off & 3u) != 0u) buf[off++] = 0;
    {
        const uint32_t ins = (uint32_t)OP_RET;
        buf[off + 0] = (uint8_t)(ins & 0xFF);
        buf[off + 1] = (uint8_t)((ins >> 8)  & 0xFF);
        buf[off + 2] = (uint8_t)((ins >> 16) & 0xFF);
        buf[off + 3] = (uint8_t)((ins >> 24) & 0xFF);
        off += 4;
    }
    /* claim 2 deltas but only 1 instruction */
    off = put_varint(buf, off, 2);
    buf[off++] = 0;
    buf[off++] = 0;
    off = put_varint(buf, off, 0);

    Chunk c = {0};
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "n_deltas") != NULL);
    uchunk_destroy(&c);
}

/* --- build_chunk_bytes: constructs a well-formed chunk byte blob ---
   Header + metadata + constants (UVAL_INT) + aligned instructions + synclines.
   Returns total bytes written.  buf must be at least 256 bytes. */
static size_t build_chunk_bytes(uint8_t *buf,
                                uint8_t max_reg,
                                const int64_t *const_vals, size_t n_const,
                                const uint32_t *instrs, size_t n_instr) {
    build_good_header(buf);
    size_t off = 24;
    buf[off++] = max_reg;
    off = put_varint(buf, off, 0);              /* empty source_name */
    off = put_varint(buf, off, (uint64_t)n_const);
    size_t ci;
    for (ci = 0; ci < n_const; ci++) {
        buf[off++] = (uint8_t)UVAL_INT;
        off = put_varint_zz(buf, off, const_vals[ci]);
    }
    off = put_varint(buf, off, (uint64_t)n_instr);
    while ((off & 3u) != 0u) buf[off++] = 0;
    size_t ii;
    for (ii = 0; ii < n_instr; ii++) {
        buf[off++] = (uint8_t)(instrs[ii] & 0xFF);
        buf[off++] = (uint8_t)((instrs[ii] >> 8)  & 0xFF);
        buf[off++] = (uint8_t)((instrs[ii] >> 16) & 0xFF);
        buf[off++] = (uint8_t)((instrs[ii] >> 24) & 0xFF);
    }
    /* synclines: n_deltas = n_instr (all zero), 0 abs_lines */
    off = put_varint(buf, off, (uint64_t)n_instr);
    size_t di;
    for (di = 0; di < n_instr; di++) buf[off++] = 0;
    off = put_varint(buf, off, 0);
    return off;
}

/* --- Task 7: Loader verifier tests --- */

UTEST(verifier_accepts_minimal_ret_only_chunk) {
    uint8_t buf[256];
    const uint32_t instrs[] = { uinstr_enc_abc(OP_RET, 0, 0, 0) };
    size_t total = build_chunk_bytes(buf, 0, NULL, 0, instrs, 1);
    Chunk c = {0};
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    uchunk_destroy(&c);
}

UTEST(verifier_rejects_opcode_ge_op_max) {
    uint8_t buf[256];
    const uint32_t instrs[] = {
        200u,                                   /* op=200, invalid */
        uinstr_enc_abc(OP_RET, 0, 0, 0)
    };
    size_t total = build_chunk_bytes(buf, 0, NULL, 0, instrs, 2);
    Chunk c = {0};
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "opcode") != NULL);
    uchunk_destroy(&c);
}

UTEST(verifier_rejects_register_gt_max_reg) {
    uint8_t buf[256];
    /* max_reg=0 but instruction references A=5 */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_MOVE, 5, 0, 0),
        uinstr_enc_abc(OP_RET, 0, 0, 0)
    };
    size_t total = build_chunk_bytes(buf, 0, NULL, 0, instrs, 2);
    Chunk c = {0};
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "register") != NULL);
    uchunk_destroy(&c);
}

UTEST(verifier_rejects_loadk_bx_out_of_constant_range) {
    uint8_t buf[256];
    const int64_t consts[] = { 1 };             /* only K[0] exists */
    const uint32_t instrs[] = {
        uinstr_enc_abx(OP_LOADK, 0, 5u),        /* Bx=5, out of range */
        uinstr_enc_abc(OP_RET, 0, 0, 0)
    };
    size_t total = build_chunk_bytes(buf, 0, consts, 1, instrs, 2);
    Chunk c = {0};
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "LOADK") != NULL || strstr(errmsg, "Bx") != NULL);
    uchunk_destroy(&c);
}

UTEST(verifier_rejects_last_instruction_not_ret) {
    uint8_t buf[256];
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_ADD, 0, 0, 0)         /* no terminating RET */
    };
    size_t total = build_chunk_bytes(buf, 0, NULL, 0, instrs, 1);
    Chunk c = {0};
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "RET") != NULL);
    uchunk_destroy(&c);
}

UTEST(verifier_accepts_unused_operand_arbitrary_bytes) {
    /* OP_NEG with C=99 (unused); verifier must NOT reject. */
    uint8_t buf[256];
    const int64_t consts[] = { 5 };
    const uint32_t instrs[] = {
        uinstr_enc_abx(OP_LOADK, 0, 0),
        uinstr_enc_abc(OP_NEG, 0, 0, 99),       /* C=99, officially unused */
        uinstr_enc_abc(OP_RET, 0, 0, 0)
    };
    size_t total = build_chunk_bytes(buf, 0, consts, 1, instrs, 3);
    Chunk c = {0};
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);                   /* MUST accept */
    uchunk_destroy(&c);
}

UTEST(verifier_accepts_ret_with_arbitrary_b_and_c) {
    uint8_t buf[128];
    const int64_t consts[] = { 5 };
    const uint32_t instrs[] = {
        uinstr_enc_abx(OP_LOADK, 0, 0),
        /* RET R0 with B=99, C=88 (both unused, must be accepted) */
        uinstr_enc_abc(OP_RET, 0, 99, 88)
    };
    size_t total = build_chunk_bytes(buf, 0, consts, 1, instrs, 2);
    Chunk c = {0};
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    uchunk_destroy(&c);
}

UTEST(verifier_accepts_hand_crafted_op_move_chunk) {
    uint8_t buf[256];
    const int64_t consts[] = { 42 };
    const uint32_t instrs[] = {
        uinstr_enc_abx(OP_LOADK, 0, 0),         /* R0 = 42 */
        uinstr_enc_abc(OP_MOVE, 1, 0, 0),       /* R1 = R0 */
        uinstr_enc_abc(OP_RET, 1, 0, 0)         /* return R1 */
    };
    size_t total = build_chunk_bytes(buf, 1, consts, 1, instrs, 3);
    Chunk c = {0};
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    UASSERT_EQ((uint8_t)OP_MOVE, (uint8_t)uinstr_op(c.instructions[1]));
    uchunk_destroy(&c);
}

/* --- Serializer tests (Task 14) --- */

UTEST(serialize_empty_chunk_produces_24_byte_header_plus_zero_sized_sections) {
    /* Empty chunk (no statements): 24-byte header + 6 body bytes.
       Body = max_reg(1) + src_len varint 0(1) + n_const varint 0(1)
            + n_instr varint 0(1) + 0 alignment pad + n_deltas varint 0(1)
            + n_abs varint 0(1) = 6 bytes.  Total = 30. */
    Chunk chunk = {0};
    Arena arena;
    Emitter e;
    uarena_init(&arena, 0);
    uemit_init(&e, &chunk, &arena, NULL);
    (void)uemit_finish(&e);

    /* Size query (buf == NULL) */
    ptrdiff_t n = uchunk_serialize(&chunk, NULL, 0);
    UASSERT_EQ((ptrdiff_t)30, n);

    /* Write pass */
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0xAA;  /* poison */
    ptrdiff_t written = uchunk_serialize(&chunk, buf, sizeof buf);
    UASSERT_EQ((ptrdiff_t)30, written);

    /* Header field checks */
    UASSERT_EQ((uint8_t)'U', buf[0]);
    UASSERT_EQ((uint8_t)'R', buf[1]);
    UASSERT_EQ((uint8_t)'B', buf[2]);
    UASSERT_EQ((uint8_t)'I', buf[3]);
    UASSERT_EQ((uint8_t)0x10, buf[4]);               /* version */
    UASSERT_EQ((uint8_t)0x00, buf[5]);               /* flags */
    UASSERT_EQ((uint8_t)0x19, buf[6]);               /* canary[0] */
    UASSERT_EQ((uint8_t)0x93, buf[7]);               /* canary[1] */
    UASSERT_EQ((uint8_t)'\r', buf[8]);               /* canary[2] */
    UASSERT_EQ((uint8_t)'\n', buf[9]);               /* canary[3] */
    UASSERT_EQ((uint8_t)0x1A, buf[10]);              /* canary[4] */
    UASSERT_EQ((uint8_t)'\n', buf[11]);              /* canary[5] */
    UASSERT_EQ((uint8_t)URBI_INT_WIDTH,   buf[12]);
    UASSERT_EQ((uint8_t)URBI_FLOAT_TYPE,  buf[13]);
    UASSERT_EQ((uint8_t)URBI_INSTR_WIDTH, buf[14]);
    UASSERT_EQ((uint8_t)URBI_ENDIANNESS,  buf[15]);

    /* Verify the output round-trips cleanly */
    Chunk c2 = {0};
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&c2, buf, (size_t)written, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    uchunk_destroy(&c2);

    uarena_destroy(&arena);
    uchunk_destroy(&chunk);
}

UTEST(serialize_cap_0_returns_required_size_without_writing) {
    Chunk chunk = {0};
    Arena arena;
    Emitter e;
    uarena_init(&arena, 0);
    uemit_init(&e, &chunk, &arena, NULL);
    (void)uemit_finish(&e);

    ptrdiff_t needed = uchunk_serialize(&chunk, NULL, 0);
    UASSERT((ptrdiff_t)0 < needed);

    uarena_destroy(&arena);
    uchunk_destroy(&chunk);
}

UTEST(serialize_cap_too_small_returns_ULOAD_TRUNCATED_negative) {
    Chunk chunk = {0};
    Arena arena;
    Emitter e;
    uarena_init(&arena, 0);
    uemit_init(&e, &chunk, &arena, NULL);
    (void)uemit_finish(&e);

    uint8_t buf[10];
    ptrdiff_t rc = uchunk_serialize(&chunk, buf, sizeof buf);
    UASSERT_EQ(-(ptrdiff_t)ULOAD_TRUNCATED, rc);

    uarena_destroy(&arena);
    uchunk_destroy(&chunk);
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
    utest_run("deserialize loads metadata max_reg and source_name",
              deserialize_loads_metadata_max_reg_and_source_name);
    utest_run("deserialize loads integer constant pool",
              deserialize_loads_integer_constant_pool);
    utest_run("deserialize rejects out-of-range UConst tag",
              deserialize_rejects_out_of_range_uconst_tag);
    utest_run("deserialize loads instruction stream with 4-byte alignment",
              deserialize_loads_instruction_stream_with_4_byte_alignment);
    utest_run("deserialize rejects non-zero alignment padding",
              deserialize_rejects_non_zero_alignment_padding);
    utest_run("deserialize loads delta synclines and abs checkpoints",
              deserialize_loads_delta_synclines_and_abs_checkpoints);
    utest_run("deserialize rejects n_deltas not equal n_instructions",
              deserialize_rejects_n_deltas_not_equal_n_instructions);
    utest_run("verifier accepts minimal RET-only chunk",
              verifier_accepts_minimal_ret_only_chunk);
    utest_run("verifier rejects opcode >= OP_MAX",
              verifier_rejects_opcode_ge_op_max);
    utest_run("verifier rejects register > max_reg",
              verifier_rejects_register_gt_max_reg);
    utest_run("verifier rejects LOADK Bx out of constant range",
              verifier_rejects_loadk_bx_out_of_constant_range);
    utest_run("verifier rejects last instruction not RET",
              verifier_rejects_last_instruction_not_ret);
    utest_run("verifier accepts unused-operand arbitrary bytes",
              verifier_accepts_unused_operand_arbitrary_bytes);
    utest_run("verifier accepts RET with arbitrary B and C",
              verifier_accepts_ret_with_arbitrary_b_and_c);
    utest_run("verifier accepts hand-crafted OP_MOVE chunk",
              verifier_accepts_hand_crafted_op_move_chunk);
    utest_run("serialize empty chunk produces 24-byte header plus zero-sized sections",
              serialize_empty_chunk_produces_24_byte_header_plus_zero_sized_sections);
    utest_run("serialize cap=0 returns required size without writing",
              serialize_cap_0_returns_required_size_without_writing);
    utest_run("serialize cap too small returns ULOAD_TRUNCATED negative",
              serialize_cap_too_small_returns_ULOAD_TRUNCATED_negative);
}
