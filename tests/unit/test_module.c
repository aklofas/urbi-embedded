/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include "umodule.h"
#include "uvm.h"
#include "uarena.h"
#include "uemit.h"
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

UTEST(module_error_name_ok) {
    UASSERT_EQ(0, strcmp("ULOAD_OK", umodule_load_error_name(ULOAD_OK)));
}

UTEST(destroy_empty_module_is_noop) {
    UModule c = {0};
    umodule_destroy(&c);
    UASSERT_EQ((void *)NULL, (void *)c.instructions);
    UASSERT_EQ((void *)NULL, (void *)c.constants);
    UASSERT_EQ((size_t)0, c.instr_count);
    UASSERT_EQ((size_t)0, c.const_count);
}

UTEST(destroy_module_with_buffers_frees_them) {
    UModule c = {0};
    /* Simulate allocation by directly using stdlib and letting destroy free.
       This is the same path umodule_deserialize / umodule_serialize use via
       the alloc_fn hook — when alloc_fn is NULL, destroy uses stdlib free. */
    c.instructions = (uint32_t *)malloc(sizeof(uint32_t) * 4);
    c.instr_cap = 4;
    c.instr_count = 2;
    c.instructions[0] = 0x11223344;
    c.instructions[1] = 0x55667788;

    c.constants = (UValue *)malloc(sizeof(UValue) * 2);
    c.const_cap = 2;
    c.const_count = 1;

    c.line_deltas = (int8_t *)malloc(sizeof(int8_t) * 4);

    c.abs_lines = (UAbsLine *)malloc(sizeof(UAbsLine) * 2);
    c.abs_line_cap = 2;
    c.abs_line_count = 1;

    umodule_destroy(&c);

    UASSERT_EQ((void *)NULL, (void *)c.instructions);
    UASSERT_EQ((void *)NULL, (void *)c.constants);
    UASSERT_EQ((void *)NULL, (void *)c.line_deltas);
    UASSERT_EQ((void *)NULL, (void *)c.abs_lines);
    UASSERT_EQ((size_t)0, c.instr_count);
    UASSERT_EQ((size_t)0, c.const_count);
    UASSERT_EQ((size_t)0, c.abs_line_count);
}

/* --- Header parse tests --- */

/* Build a canonical v1.0 little-endian i64+f64 header.
   Used as a base in tests that mutate one byte. */
static void build_good_header(uint8_t hdr[24]) {
    size_t i;
    for (i = 0; i < 24; i++) hdr[i] = 0;
    hdr[0] = 'U'; hdr[1] = 'R'; hdr[2] = 'B'; hdr[3] = 'I'; /* magic */
    hdr[4] = (uint8_t)URBI_BYTECODE_VERSION_BYTE;  /* current version */
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
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, offset, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    umodule_destroy(&c);
}

UTEST(deserialize_rejects_buffer_shorter_than_header) {
    uint8_t buf[10];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    UModule c = {0};
    UASSERT_EQ(ULOAD_TRUNCATED, umodule_deserialize(&c, buf, sizeof buf, NULL, 0));
    umodule_destroy(&c);
}

UTEST(deserialize_rejects_bad_magic) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[0] = 'X';  /* corrupt first magic byte */
    UModule c = {0};
    UASSERT_EQ(ULOAD_BAD_MAGIC, umodule_deserialize(&c, hdr, sizeof hdr, NULL, 0));
    umodule_destroy(&c);
}

UTEST(deserialize_rejects_corrupted_canary_simulated_ftp_ascii) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[8] = 0x20;  /* \r replaced by space — ASCII-mode munge */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UModuleLoadError rc = umodule_deserialize(&c, hdr, sizeof hdr, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_BAD_MAGIC, rc);
    UASSERT(strstr(errmsg, "canary") != NULL);
    umodule_destroy(&c);
}

UTEST(deserialize_rejects_unsupported_version) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[4] = 0x20;  /* would be v2.0 */
    UModule c = {0};
    UASSERT_EQ(ULOAD_UNSUPPORTED_VERSION, umodule_deserialize(&c, hdr, sizeof hdr, NULL, 0));
    umodule_destroy(&c);
}

UTEST(deserialize_rejects_v1_0_module) {
    /* Build a 24-byte header with version byte = 0x10 (v1.0). */
    uint8_t buf[30];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    buf[4] = 0x10;  /* version byte (v1.0 — should be rejected) */
    /* Minimal body: 6 zero-count varints (metadata + sections). */
    size_t offset = 24;
    buf[offset++] = 0;  /* max_reg */
    buf[offset++] = 0;  /* varint source_name_len = 0 */
    buf[offset++] = 0;  /* varint n_constants = 0 */
    buf[offset++] = 0;  /* varint n_instructions = 0 */
    buf[offset++] = 0;  /* varint n_deltas = 0 */
    buf[offset++] = 0;  /* varint n_abs_lines = 0 */
    UModule c = {0};
    UASSERT_EQ(ULOAD_UNSUPPORTED_VERSION, umodule_deserialize(&c, buf, offset, NULL, 0));
    umodule_destroy(&c);
}

UTEST(deserialize_rejects_v1_1_module) {
    /* Version byte 0x11 (M2) must be rejected: OP_RETURN dispatch semantics
       changed between M2 and M3; loading old modules silently would produce
       wrong nested-unwind behaviour.  Hard break is the safe choice. */
    uint8_t buf[30];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    buf[4] = 0x11;  /* version byte (v1.1 — M2; should be rejected) */
    size_t offset = 24;
    buf[offset++] = 0;  /* max_reg */
    buf[offset++] = 0;  /* varint source_name_len = 0 */
    buf[offset++] = 0;  /* varint n_constants = 0 */
    buf[offset++] = 0;  /* varint n_instructions = 0 */
    buf[offset++] = 0;  /* varint n_deltas = 0 */
    buf[offset++] = 0;  /* varint n_abs_lines = 0 */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UModuleLoadError rc = umodule_deserialize(&c, buf, offset, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_UNSUPPORTED_VERSION, rc);
    /* errmsg should name the rejected version so users know what they have */
    UASSERT(strstr(errmsg, "0x11") != NULL || strstr(errmsg, "1.1") != NULL);
    umodule_destroy(&c);
}

UTEST(deserialize_rejects_v1_2_module) {
    /* Version byte 0x12 (M3) must be rejected by the v1.3 loader: M4 added
       UProto.ic_count + UProto.ic_names side table; loading v1.2 silently
       would leave IC sites uninitialized.  Hard break per encoding spec §7. */
    uint8_t buf[30];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    buf[4] = 0x12;  /* version byte (v1.2 — M3; should be rejected by v1.3 loader) */
    size_t offset = 24;
    buf[offset++] = 0;  /* max_reg */
    buf[offset++] = 0;  /* varint source_name_len = 0 */
    buf[offset++] = 0;  /* varint n_constants = 0 */
    buf[offset++] = 0;  /* varint n_instructions = 0 */
    buf[offset++] = 0;  /* varint n_deltas = 0 */
    buf[offset++] = 0;  /* varint n_abs_lines = 0 */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UModuleLoadError rc = umodule_deserialize(&c, buf, offset, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_UNSUPPORTED_VERSION, rc);
    /* errmsg should name the rejected version so users know what they have */
    UASSERT(strstr(errmsg, "0x12") != NULL || strstr(errmsg, "1.2") != NULL);
    umodule_destroy(&c);
}

UTEST(deserialize_accepts_v13_module) {
    /* A minimal well-formed v1.3 module must be accepted. */
    uint8_t buf[30];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    buf[4] = URBI_BYTECODE_VERSION_BYTE;  /* v1.3 */
    size_t offset = 24;
    buf[offset++] = 0;  /* max_reg */
    buf[offset++] = 0;  /* varint source_name_len = 0 */
    buf[offset++] = 0;  /* varint n_constants = 0 */
    buf[offset++] = 0;  /* varint n_instructions = 0 */
    buf[offset++] = 0;  /* varint n_deltas = 0 */
    buf[offset++] = 0;  /* varint n_abs_lines = 0 */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UModuleLoadError rc = umodule_deserialize(&c, buf, offset, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    umodule_destroy(&c);
}

UTEST(uproto_alloc_zero_inits_ic_count_and_ic_names) {
    /* M4 v1.3: umodule_alloc_nested_proto must zero ic_count and ic_names
       (encoding spec §5.1).  Subsequent M4 tasks rely on this so freshly
       allocated protos start with no IC sites. */
    UModule m = {0};
    UProto *p = umodule_alloc_nested_proto(&m);
    UASSERT(p != NULL);
    UASSERT_EQ((unsigned)p->ic_count, 0u);
    UASSERT_EQ((void *)p->ic_names, (void *)NULL);
    umodule_destroy(&m);
}

UTEST(uproto_destroy_frees_ic_names) {
    /* M4 v1.3: umodule_proto_destroy_buffers must free the ic_names array.
       Allocate via stdlib so destroy (alloc_fn == NULL → stdlib_alloc) frees it. */
    UModule m = {0};
    UProto *p = umodule_alloc_nested_proto(&m);
    UASSERT(p != NULL);
    /* Pretend the emitter populated ic_count + ic_names with two opaque slots. */
    p->ic_count = 2;
    p->ic_names = (USymbol **)malloc(2 * sizeof(USymbol *));
    UASSERT(p->ic_names != NULL);
    p->ic_names[0] = NULL;
    p->ic_names[1] = NULL;
    /* umodule_destroy frees nested protos via umodule_proto_destroy_buffers. */
    umodule_destroy(&m);
    /* If we reach here without leaking under ASan, ic_names was freed. */
}

UTEST(deserialize_rejects_wrong_int_width) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[12] = 4;  /* claims i32 */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UModuleLoadError rc = umodule_deserialize(&c, hdr, sizeof hdr, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_FLAVOR_MISMATCH, rc);
    UASSERT(strstr(errmsg, "int_width") != NULL);
    umodule_destroy(&c);
}

UTEST(deserialize_rejects_wrong_float_type) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[13] = (URBI_FLOAT_TYPE == 8) ? 4 : 8;  /* flip to the other flavor */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UModuleLoadError rc = umodule_deserialize(&c, hdr, sizeof hdr, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_FLAVOR_MISMATCH, rc);
    UASSERT(strstr(errmsg, "float_type") != NULL);
    umodule_destroy(&c);
}

UTEST(deserialize_rejects_wrong_instr_width) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[14] = 8;  /* claims 8-byte instructions */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UModuleLoadError rc = umodule_deserialize(&c, hdr, sizeof hdr, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_FLAVOR_MISMATCH, rc);
    UASSERT(strstr(errmsg, "instr_width") != NULL);
    umodule_destroy(&c);
}

UTEST(deserialize_rejects_wrong_endianness) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[15] = 1;  /* big-endian on LE host */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UModuleLoadError rc = umodule_deserialize(&c, hdr, sizeof hdr, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_FLAVOR_MISMATCH, rc);
    UASSERT(strstr(errmsg, "endianness") != NULL);
    umodule_destroy(&c);
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

    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    UASSERT_EQ((uint8_t)5, c.max_reg);
    UASSERT(c.source_name != NULL);
    UASSERT_EQ(0, strcmp(c.source_name, "repl"));
    umodule_destroy(&c);
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

    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    UASSERT_EQ((size_t)2, c.const_count);
    UASSERT_EQ((uint8_t)UVAL_INT, c.constants[0].kind);
    UASSERT_EQ((int64_t)1,   c.constants[0].v.i);
    UASSERT_EQ((uint8_t)UVAL_INT, c.constants[1].kind);
    UASSERT_EQ((int64_t)-42, c.constants[1].v.i);
    umodule_destroy(&c);
}

UTEST(deserialize_rejects_out_of_range_uvalue_tag) {
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
    UModule c = {0};
    UASSERT_EQ(ULOAD_CORRUPT_TAG, umodule_deserialize(&c, buf, off, NULL, 0));
    umodule_destroy(&c);
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

    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    UASSERT_EQ((size_t)1, c.instr_count);
    UASSERT_EQ((UOpcode)OP_RET, uinstr_op(c.instructions[0]));
    umodule_destroy(&c);
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

    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "align") != NULL);
    umodule_destroy(&c);
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

    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    UASSERT_EQ((size_t)3, c.instr_count);
    UASSERT_EQ((int8_t)-128, c.line_deltas[0]);
    UASSERT_EQ((int8_t)2,    c.line_deltas[1]);
    UASSERT_EQ((int8_t)-1,   c.line_deltas[2]);
    UASSERT_EQ((size_t)1, c.abs_line_count);
    UASSERT_EQ((uint32_t)0,  c.abs_lines[0].pc);
    UASSERT_EQ((uint32_t)10, c.abs_lines[0].line);
    umodule_destroy(&c);
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

    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "n_deltas") != NULL);
    umodule_destroy(&c);
}

/* --- build_module_bytes: constructs a well-formed module byte blob ---
   Header + metadata + constants (UVAL_INT) + aligned instructions + synclines.
   Returns total bytes written.  buf must be at least 256 bytes. */
static size_t build_module_bytes(uint8_t *buf,
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

UTEST(verifier_accepts_minimal_ret_only_module) {
    uint8_t buf[256];
    const uint32_t instrs[] = { uinstr_enc_abc(OP_RET, 0, 0, 0) };
    size_t total = build_module_bytes(buf, 0, NULL, 0, instrs, 1);
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    umodule_destroy(&c);
}

UTEST(verifier_rejects_opcode_ge_op_max) {
    uint8_t buf[256];
    const uint32_t instrs[] = {
        200u,                                   /* op=200, invalid */
        uinstr_enc_abc(OP_RET, 0, 0, 0)
    };
    size_t total = build_module_bytes(buf, 0, NULL, 0, instrs, 2);
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "opcode") != NULL);
    umodule_destroy(&c);
}

UTEST(verifier_rejects_register_gt_max_reg) {
    uint8_t buf[256];
    /* max_reg=0 but instruction references A=5 */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_MOVE, 5, 0, 0),
        uinstr_enc_abc(OP_RET, 0, 0, 0)
    };
    size_t total = build_module_bytes(buf, 0, NULL, 0, instrs, 2);
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "register") != NULL);
    umodule_destroy(&c);
}

UTEST(verifier_rejects_loadk_bx_out_of_constant_range) {
    uint8_t buf[256];
    const int64_t consts[] = { 1 };             /* only K[0] exists */
    const uint32_t instrs[] = {
        uinstr_enc_abx(OP_LOADK, 0, 5u),        /* Bx=5, out of range */
        uinstr_enc_abc(OP_RET, 0, 0, 0)
    };
    size_t total = build_module_bytes(buf, 0, consts, 1, instrs, 2);
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "LOADK") != NULL || strstr(errmsg, "Bx") != NULL);
    umodule_destroy(&c);
}

UTEST(verifier_rejects_last_instruction_not_ret) {
    uint8_t buf[256];
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_ADD, 0, 0, 0)         /* no terminating RET */
    };
    size_t total = build_module_bytes(buf, 0, NULL, 0, instrs, 1);
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "RET") != NULL);
    umodule_destroy(&c);
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
    size_t total = build_module_bytes(buf, 0, consts, 1, instrs, 3);
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);                   /* MUST accept */
    umodule_destroy(&c);
}

UTEST(verifier_accepts_ret_with_arbitrary_b_and_c) {
    uint8_t buf[128];
    const int64_t consts[] = { 5 };
    const uint32_t instrs[] = {
        uinstr_enc_abx(OP_LOADK, 0, 0),
        /* RET R0 with B=99, C=88 (both unused, must be accepted) */
        uinstr_enc_abc(OP_RET, 0, 99, 88)
    };
    size_t total = build_module_bytes(buf, 0, consts, 1, instrs, 2);
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    umodule_destroy(&c);
}

UTEST(verifier_accepts_hand_crafted_op_move_module) {
    uint8_t buf[256];
    const int64_t consts[] = { 42 };
    const uint32_t instrs[] = {
        uinstr_enc_abx(OP_LOADK, 0, 0),         /* R0 = 42 */
        uinstr_enc_abc(OP_MOVE, 1, 0, 0),       /* R1 = R0 */
        uinstr_enc_abc(OP_RET, 1, 0, 0)         /* return R1 */
    };
    size_t total = build_module_bytes(buf, 1, consts, 1, instrs, 3);
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    UASSERT_EQ((uint8_t)OP_MOVE, (uint8_t)uinstr_op(c.instructions[1]));
    umodule_destroy(&c);
}

/* --- Round-trip integration tests (Task 15) --- */

/* Returns true if two modules are semantically equivalent. */
static bool modules_equivalent(const UModule *a, const UModule *b) {
    size_t i;
    if (a->instr_count     != b->instr_count)     return false;
    if (a->const_count     != b->const_count)     return false;
    if (a->abs_line_count  != b->abs_line_count)  return false;
    if (a->max_reg         != b->max_reg)         return false;
    for (i = 0; i < a->instr_count; i++) {
        if (a->instructions[i] != b->instructions[i]) return false;
        if (a->line_deltas[i]  != b->line_deltas[i])  return false;
    }
    for (i = 0; i < a->const_count; i++) {
        if (a->constants[i].kind != b->constants[i].kind) return false;
        if (a->constants[i].kind == UVAL_INT
         && a->constants[i].v.i  != b->constants[i].v.i)  return false;
    }
    for (i = 0; i < a->abs_line_count; i++) {
        if (a->abs_lines[i].pc   != b->abs_lines[i].pc)   return false;
        if (a->abs_lines[i].line != b->abs_lines[i].line) return false;
    }
    /* source_name: both NULL, or strcmp == 0 */
    if ((a->source_name == NULL) != (b->source_name == NULL)) return false;
    if (a->source_name != NULL && strcmp(a->source_name, b->source_name) != 0) return false;
    return true;
}

/* Emit ast, serialize to a heap buffer, deserialize into a second module,
   assert round-trip equivalence, then clean up both modules and the arena. */
static void roundtrip_ast(UAstNode *ast, const char *source_name) {
    UModule src = {0};
    UArena arena;
    UEmitter e;
    char errmsg[256];
    uint8_t *buf;
    UModule dst = {0};
    ptrdiff_t need;
    ptrdiff_t wrote;
    UModuleLoadError rc;

    uarena_init(&arena, 0);
    uemit_init(&e, &src, &arena, NULL, source_name);
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, ast));
    UASSERT_EQ(EMIT_OK, uemit_finish(&e));

    need = umodule_serialize(&src, NULL, 0);
    UASSERT((ptrdiff_t)0 < need);

    buf = (uint8_t *)malloc((size_t)need);
    wrote = umodule_serialize(&src, buf, (size_t)need);
    UASSERT_EQ(need, wrote);

    errmsg[0] = '\0';
    rc = umodule_deserialize(&dst, buf, (size_t)need, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    UASSERT(modules_equivalent(&src, &dst));

    free(buf);
    uarena_destroy(&arena);
    umodule_destroy(&src);
    umodule_destroy(&dst);
}

UTEST(roundtrip_ast_int_literal) {
    UAstNode n = {0};
    n.kind = AST_INT; n.u.i = 42; n.line = 1;
    roundtrip_ast(&n, "test");
}

UTEST(roundtrip_ast_binary_1_plus_2) {
    UAstNode lhs = {0}; lhs.kind = AST_INT; lhs.u.i = 1; lhs.line = 1;
    UAstNode rhs = {0}; rhs.kind = AST_INT; rhs.u.i = 2; rhs.line = 1;
    UAstNode bin = {0};
    bin.kind = AST_BINARY; bin.u.binary.op = BOP_ADD;
    bin.u.binary.lhs = &lhs; bin.u.binary.rhs = &rhs;
    bin.line = 1;
    roundtrip_ast(&bin, NULL);
}

UTEST(roundtrip_ast_unary_neg_5) {
    UAstNode operand = {0}; operand.kind = AST_INT; operand.u.i = 5; operand.line = 1;
    UAstNode neg = {0};
    neg.kind = AST_UNARY; neg.u.unary.op = UOP_NEG; neg.u.unary.operand = &operand;
    neg.line = 1;
    roundtrip_ast(&neg, "a/b/c.u");
}

/* --- Serializer tests (Task 14) --- */

UTEST(serialize_empty_module_produces_24_byte_header_plus_zero_sized_sections) {
    /* Empty module (no statements): 24-byte header + 6 body bytes.
       Body = max_reg(1) + src_len varint 0(1) + n_const varint 0(1)
            + n_instr varint 0(1) + 0 alignment pad + n_deltas varint 0(1)
            + n_abs varint 0(1) = 6 bytes.  Total = 30. */
    UModule module = {0};
    UArena arena;
    UEmitter e;
    uarena_init(&arena, 0);
    uemit_init(&e, &module, &arena, NULL, NULL);
    (void)uemit_finish(&e);

    /* Size query (buf == NULL) */
    ptrdiff_t n = umodule_serialize(&module, NULL, 0);
    UASSERT_EQ((ptrdiff_t)30, n);

    /* Write pass */
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0xAA;  /* poison */
    ptrdiff_t written = umodule_serialize(&module, buf, sizeof buf);
    UASSERT_EQ((ptrdiff_t)30, written);

    /* Header field checks */
    UASSERT_EQ((uint8_t)'U', buf[0]);
    UASSERT_EQ((uint8_t)'R', buf[1]);
    UASSERT_EQ((uint8_t)'B', buf[2]);
    UASSERT_EQ((uint8_t)'I', buf[3]);
    UASSERT_EQ((uint8_t)URBI_BYTECODE_VERSION_BYTE, buf[4]); /* version */
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
    UModule c2 = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c2, buf, (size_t)written, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    umodule_destroy(&c2);

    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(serialize_cap_0_returns_required_size_without_writing) {
    UModule module = {0};
    UArena arena;
    UEmitter e;
    uarena_init(&arena, 0);
    uemit_init(&e, &module, &arena, NULL, NULL);
    (void)uemit_finish(&e);

    ptrdiff_t needed = umodule_serialize(&module, NULL, 0);
    UASSERT((ptrdiff_t)0 < needed);

    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(serialize_cap_too_small_returns_ULOAD_TRUNCATED_negative) {
    UModule module = {0};
    UArena arena;
    UEmitter e;
    uarena_init(&arena, 0);
    uemit_init(&e, &module, &arena, NULL, NULL);
    (void)uemit_finish(&e);

    uint8_t buf[10];
    ptrdiff_t rc = umodule_serialize(&module, buf, sizeof buf);
    UASSERT_EQ(-(ptrdiff_t)ULOAD_TRUNCATED, rc);

    uarena_destroy(&arena);
    umodule_destroy(&module);
}

/* Custom allocator that fails after `fails_after` successful calls.
   Mirrors the LimitAlloc type in test_emit.c. */
typedef struct { size_t ok_calls; size_t fails_after; } UModuleLimitAlloc;

static void *module_limit_alloc(void *ptr, size_t nbytes, void *ud) {
    UModuleLimitAlloc *la = (UModuleLimitAlloc *)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    if (la->ok_calls >= la->fails_after) return NULL;
    la->ok_calls++;
    return realloc(ptr, nbytes);
}

/* --- Additional coverage tests --- */

UTEST(destroy_null_module_is_noop) {
    /* umodule_destroy(NULL) must not crash — guards the null branch. */
    umodule_destroy(NULL);
    UASSERT(true);
}

UTEST(module_load_error_name_all_codes) {
    /* Exercise every UModuleLoadError case in umodule_load_error_name. */
    UASSERT_EQ(0, strcmp("ULOAD_OK",                  umodule_load_error_name(ULOAD_OK)));
    UASSERT_EQ(0, strcmp("ULOAD_BAD_MAGIC",           umodule_load_error_name(ULOAD_BAD_MAGIC)));
    UASSERT_EQ(0, strcmp("ULOAD_UNSUPPORTED_VERSION", umodule_load_error_name(ULOAD_UNSUPPORTED_VERSION)));
    UASSERT_EQ(0, strcmp("ULOAD_FLAVOR_MISMATCH",     umodule_load_error_name(ULOAD_FLAVOR_MISMATCH)));
    UASSERT_EQ(0, strcmp("ULOAD_TRUNCATED",           umodule_load_error_name(ULOAD_TRUNCATED)));
    UASSERT_EQ(0, strcmp("ULOAD_CORRUPT_VARINT",      umodule_load_error_name(ULOAD_CORRUPT_VARINT)));
    UASSERT_EQ(0, strcmp("ULOAD_CORRUPT_TAG",         umodule_load_error_name(ULOAD_CORRUPT_TAG)));
    UASSERT_EQ(0, strcmp("ULOAD_CORRUPT",             umodule_load_error_name(ULOAD_CORRUPT)));
    UASSERT_EQ(0, strcmp("ULOAD_OOM",                 umodule_load_error_name(ULOAD_OOM)));
    /* Out-of-range code falls through to ULOAD_UNKNOWN sentinel. */
    UASSERT(umodule_load_error_name((UModuleLoadError)99) != NULL);
}

UTEST(deserialize_null_module_returns_truncated) {
    /* umodule_deserialize(NULL, ...) must not crash — covers the null guard. */
    uint8_t buf[24];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    UModuleLoadError rc = umodule_deserialize(NULL, buf, sizeof buf, NULL, 0);
    UASSERT_EQ(ULOAD_TRUNCATED, rc);
}

UTEST(deserialize_oom_on_constants_allocation) {
    /* Cause ULOAD_OOM during the constants module_grow by failing after the
       initial source_name allocation succeeds (first alloc for source_name)
       but failing on the next call (constants buffer). */
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    buf[off++] = 0;
    off = put_varint(buf, off, 0);              /* no source_name */
    off = put_varint(buf, off, 2);              /* 2 constants -> triggers grow */
    buf[off++] = (uint8_t)UVAL_INT;
    off = put_varint_zz(buf, off, 1);
    buf[off++] = (uint8_t)UVAL_INT;
    off = put_varint_zz(buf, off, 2);
    off = put_varint(buf, off, 0);              /* 0 instructions */
    off = put_varint(buf, off, 0);              /* 0 deltas */
    off = put_varint(buf, off, 0);              /* 0 abs_lines */

    UModuleLimitAlloc la;
    la.ok_calls = 0;
    la.fails_after = 0;                         /* fail on first alloc (constants grow) */
    UModule c = {0};
    c.alloc_fn = module_limit_alloc;
    c.alloc_ud = &la;
    UModuleLoadError rc = umodule_deserialize(&c, buf, off, NULL, 0);
    UASSERT_EQ(ULOAD_OOM, rc);
    umodule_destroy(&c);
}

UTEST(deserialize_oom_on_instructions_allocation) {
    /* Cause ULOAD_OOM during the instructions module_grow by allowing the
       constants allocation to succeed (1 call) but failing the next. */
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    buf[off++] = 0;
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 1);              /* 1 constant — triggers first grow */
    buf[off++] = (uint8_t)UVAL_INT;
    off = put_varint_zz(buf, off, 5);
    off = put_varint(buf, off, 1);              /* 1 instruction — triggers second grow */
    while ((off & 3u) != 0u) buf[off++] = 0;
    {
        const uint32_t ins = (uint32_t)OP_RET;
        buf[off + 0] = (uint8_t)(ins & 0xFF);
        buf[off + 1] = (uint8_t)((ins >> 8)  & 0xFF);
        buf[off + 2] = (uint8_t)((ins >> 16) & 0xFF);
        buf[off + 3] = (uint8_t)((ins >> 24) & 0xFF);
        off += 4;
    }
    off = put_varint(buf, off, 1);
    buf[off++] = (uint8_t)(int8_t)-128;
    off = put_varint(buf, off, 1);
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 1);

    /* Allow only the first allocation (constants), fail the second (instructions). */
    UModuleLimitAlloc la;
    la.ok_calls = 0;
    la.fails_after = 1;
    UModule c = {0};
    c.alloc_fn = module_limit_alloc;
    c.alloc_ud = &la;
    UModuleLoadError rc = umodule_deserialize(&c, buf, off, NULL, 0);
    UASSERT_EQ(ULOAD_OOM, rc);
    umodule_destroy(&c);
}

UTEST(deserialize_loads_float_constant) {
    /* Exercise the UVAL_FLOAT branch in the constants decode path. */
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    buf[off++] = 0;
    off = put_varint(buf, off, 0);              /* no source_name */
    off = put_varint(buf, off, 1);              /* 1 constant: UVAL_FLOAT */
    buf[off++] = (uint8_t)UVAL_FLOAT;
    /* Write a float value (3.14) as raw bytes matching URBI_FLOAT_TYPE. */
    if (URBI_FLOAT_TYPE == 8) {
        double fval = 3.14;
        unsigned char fbytes[8];
        memcpy(fbytes, &fval, 8);
        for (i = 0; i < 8; i++) buf[off++] = fbytes[i];
    } else {
        float fval = 3.14f;
        unsigned char fbytes[4];
        memcpy(fbytes, &fval, 4);
        for (i = 0; i < 4; i++) buf[off++] = fbytes[i];
    }
    /* n_instructions = 0; then write 4-byte alignment padding if needed. */
    off = put_varint(buf, off, 0);
    while ((off & 3u) != 0u) buf[off++] = 0;
    off = put_varint(buf, off, 0);              /* n_deltas = 0 */
    off = put_varint(buf, off, 0);              /* n_abs_lines = 0 */
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    UASSERT_EQ((size_t)1, c.const_count);
    UASSERT_EQ((uint8_t)UVAL_FLOAT, c.constants[0].kind);
    umodule_destroy(&c);
}

UTEST(deserialize_rejects_nil_bool_str_constant_tag) {
    /* UVAL_NIL/UVAL_BOOL/UVAL_STR constants have no payload at M1.
       The deserializer rejects them with ULOAD_CORRUPT_TAG via the else branch
       (they pass the > UVAL_STR range check but are neither INT nor FLOAT). */
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    buf[off++] = 0;
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 1);              /* 1 constant */
    buf[off++] = (uint8_t)UVAL_NIL;            /* kind 0 — no INT or FLOAT, hits else */
    off = put_varint(buf, off, 0);              /* padding */
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 0);
    UModule c = {0};
    UModuleLoadError rc = umodule_deserialize(&c, buf, off, NULL, 0);
    UASSERT_EQ(ULOAD_CORRUPT_TAG, rc);
    umodule_destroy(&c);
}

UTEST(deserialize_truncated_at_line_deltas) {
    /* Build a valid module with 1 instruction but truncate the buffer before
       the line_deltas data — should return ULOAD_TRUNCATED. */
    uint8_t buf[256];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    buf[off++] = 0;                             /* max_reg */
    off = put_varint(buf, off, 0);              /* no source_name */
    off = put_varint(buf, off, 0);              /* 0 constants */
    off = put_varint(buf, off, 1);              /* 1 instruction */
    while ((off & 3u) != 0u) buf[off++] = 0;
    {
        const uint32_t ins = (uint32_t)OP_RET;
        buf[off + 0] = (uint8_t)(ins & 0xFF);
        buf[off + 1] = (uint8_t)((ins >> 8)  & 0xFF);
        buf[off + 2] = (uint8_t)((ins >> 16) & 0xFF);
        buf[off + 3] = (uint8_t)((ins >> 24) & 0xFF);
        off += 4;
    }
    /* Write n_deltas=1 but truncate before writing the actual delta byte. */
    off = put_varint(buf, off, 1);              /* n_deltas = 1 */
    /* Do NOT write the delta byte; pass off as size so buffer ends here. */
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_TRUNCATED, rc);
    umodule_destroy(&c);
}

UTEST(deserialize_oom_on_abs_lines_allocation) {
    /* Cause ULOAD_OOM during the abs_lines module_grow by allowing constants and
       instructions and line_deltas allocations to succeed but failing abs_lines. */
    uint8_t buf[256];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    buf[off++] = 0;
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 0);              /* 0 constants */
    off = put_varint(buf, off, 1);              /* 1 instruction */
    while ((off & 3u) != 0u) buf[off++] = 0;
    {
        const uint32_t ins = (uint32_t)OP_RET;
        buf[off + 0] = (uint8_t)(ins & 0xFF);
        buf[off + 1] = 0; buf[off + 2] = 0; buf[off + 3] = 0;
        off += 4;
    }
    /* 1 delta (sentinel), 1 abs_line */
    off = put_varint(buf, off, 1);
    buf[off++] = (uint8_t)(int8_t)-128;
    off = put_varint(buf, off, 1);              /* n_abs = 1 */
    off = put_varint(buf, off, 0);              /* abs_line[0].pc = 0 */
    off = put_varint(buf, off, 1);              /* abs_line[0].line = 1 */

    /* Allocation order: (1) instructions grow, (2) line_deltas fresh alloc,
       (3) abs_lines module_grow — fail the 3rd. */
    UModuleLimitAlloc la;
    la.ok_calls = 0;
    la.fails_after = 2;
    UModule c = {0};
    c.alloc_fn = module_limit_alloc;
    c.alloc_ud = &la;
    UModuleLoadError rc = umodule_deserialize(&c, buf, off, NULL, 0);
    UASSERT_EQ(ULOAD_OOM, rc);
    umodule_destroy(&c);
}

UTEST(deserialize_rejects_abs_line_pc_out_of_range) {
    /* abs_line pc >= instr_count should return ULOAD_CORRUPT. */
    uint8_t buf[256];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    buf[off++] = 0;
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 1);              /* 1 instruction */
    while ((off & 3u) != 0u) buf[off++] = 0;
    {
        const uint32_t ins = (uint32_t)OP_RET;
        buf[off + 0] = (uint8_t)(ins & 0xFF);
        buf[off + 1] = 0; buf[off + 2] = 0; buf[off + 3] = 0;
        off += 4;
    }
    off = put_varint(buf, off, 1);
    buf[off++] = (uint8_t)(int8_t)-128;
    off = put_varint(buf, off, 1);              /* 1 abs_line */
    off = put_varint(buf, off, 99);             /* pc=99, out of range (instr_count=1) */
    off = put_varint(buf, off, 5);
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "out of range") != NULL || strstr(errmsg, "pc") != NULL);
    umodule_destroy(&c);
}

UTEST(deserialize_truncated_at_metadata_max_reg) {
    /* Buffer exactly 24 bytes (valid header but no body) — triggers the
       "truncated at metadata" guard at the max_reg read. */
    uint8_t buf[24];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UModuleLoadError rc = umodule_deserialize(&c, buf, sizeof buf, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_TRUNCATED, rc);
    UASSERT(strstr(errmsg, "truncated") != NULL || strstr(errmsg, "metadata") != NULL);
    umodule_destroy(&c);
}

UTEST(deserialize_module_grow_reuses_existing_cap) {
    /* Deserialize two modules from the same buffer back-to-back, reusing
       the existing constants buffer — triggers the module_grow "cap >= new_cap"
       early-return branch (when cap is already large enough). */
    uint8_t buf[256];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    /* Build a module with 2 constants. */
    int64_t cv[] = {1, 2};
    const uint32_t instrs[] = { uinstr_enc_abc(OP_RET, 0, 0, 0) };
    size_t total = build_module_bytes(buf, 0, cv, 2, instrs, 1);

    /* First deserialize. */
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    UASSERT_EQ((size_t)2, c.const_count);
    /* c now has const_cap >= 8 (first grow starts at 8). */

    /* Reset counts but keep the buffers allocated. */
    c.const_count = 0;
    c.instr_count = 0;
    c.abs_line_count = 0;
    if (c.line_deltas != NULL) {
        UModuleAllocFn alloc = c.alloc_fn != NULL ? c.alloc_fn
                            : (UModuleAllocFn)NULL; /* stdlib handled by destroy later */
        (void)alloc; /* just keep the pointer, don't free now */
        free(c.line_deltas); c.line_deltas = NULL;
    }

    /* Second deserialize into the same module — module_grow for constants will
       see const_cap >= 2, triggering the "already large enough" branch. */
    rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    UASSERT_EQ((size_t)2, c.const_count);

    umodule_destroy(&c);
}

UTEST(deserialize_rejects_trailing_bytes) {
    /* A valid module followed by extra bytes should return ULOAD_CORRUPT. */
    uint8_t buf[256];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    const uint32_t instrs[] = { uinstr_enc_abc(OP_RET, 0, 0, 0) };
    size_t total = build_module_bytes(buf, 0, NULL, 0, instrs, 1);
    /* Append extra garbage byte. */
    buf[total] = 0xAB;
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, total + 1, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "trailing") != NULL);
    umodule_destroy(&c);
}

UTEST(verifier_rejects_register_b_gt_max_reg) {
    /* OP_ADD with B > max_reg should return ULOAD_CORRUPT. */
    uint8_t buf[256];
    /* max_reg=0, ADD with B=5 > max_reg */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_ADD, 0, 5, 0),       /* B=5 > max_reg=0 */
        uinstr_enc_abc(OP_RET, 0, 0, 0)
    };
    size_t total = build_module_bytes(buf, 0, NULL, 0, instrs, 2);
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "register") != NULL || strstr(errmsg, "B=") != NULL);
    umodule_destroy(&c);
}

UTEST(verifier_rejects_register_c_gt_max_reg) {
    /* OP_ADD with C > max_reg should return ULOAD_CORRUPT. */
    uint8_t buf[256];
    /* max_reg=0, ADD with B=0 ok, C=5 > max_reg */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_ADD, 0, 0, 5),       /* C=5 > max_reg=0 */
        uinstr_enc_abc(OP_RET, 0, 0, 0)
    };
    size_t total = build_module_bytes(buf, 0, NULL, 0, instrs, 2);
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "register") != NULL || strstr(errmsg, "C=") != NULL);
    umodule_destroy(&c);
}

UTEST(deserialize_rejects_non_monotonic_abs_lines) {
    /* Build a module with two abs_line checkpoints where the second has a
       lower pc than the first — should return ULOAD_CORRUPT. */
    uint8_t buf[256];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    buf[off++] = 1;                             /* max_reg=1 */
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 0);              /* 0 constants */
    /* 2 instructions */
    off = put_varint(buf, off, 2);
    while ((off & 3u) != 0u) buf[off++] = 0;
    {
        int j;
        for (j = 0; j < 2; j++) {
            const uint32_t ins = (uint32_t)OP_RET;
            buf[off + 0] = (uint8_t)(ins & 0xFF);
            buf[off + 1] = (uint8_t)((ins >> 8)  & 0xFF);
            buf[off + 2] = (uint8_t)((ins >> 16) & 0xFF);
            buf[off + 3] = (uint8_t)((ins >> 24) & 0xFF);
            off += 4;
        }
    }
    /* 2 deltas */
    off = put_varint(buf, off, 2);
    buf[off++] = (uint8_t)(int8_t)-128;
    buf[off++] = (uint8_t)(int8_t)-128;
    /* 2 abs_line checkpoints — second pc (0) <= first pc (1): non-monotonic */
    off = put_varint(buf, off, 2);
    off = put_varint(buf, off, 1);              /* abs_line[0].pc = 1 */
    off = put_varint(buf, off, 10);
    off = put_varint(buf, off, 0);              /* abs_line[1].pc = 0 <= prev=1: corrupt */
    off = put_varint(buf, off, 20);
    UModule c = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "monotonic") != NULL);
    umodule_destroy(&c);
}

void test_module_suite(void);

void test_module_suite(void) {
    utest_run("module error name ok",                   module_error_name_ok);
    utest_run("destroy empty module is a no-op",        destroy_empty_module_is_noop);
    utest_run("destroy module with allocated buffers frees them",
              destroy_module_with_buffers_frees_them);
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
    utest_run("deserialize rejects v1.0 module",
              deserialize_rejects_v1_0_module);
    utest_run("deserialize rejects v1.1 module (M2 hard break)",
              deserialize_rejects_v1_1_module);
    utest_run("deserialize rejects v1.2 module (M4 hard break)",
              deserialize_rejects_v1_2_module);
    utest_run("deserialize accepts v1.3 module",
              deserialize_accepts_v13_module);
    utest_run("uproto alloc zero-inits ic_count and ic_names",
              uproto_alloc_zero_inits_ic_count_and_ic_names);
    utest_run("uproto destroy frees ic_names",
              uproto_destroy_frees_ic_names);
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
    utest_run("deserialize rejects out-of-range UValue tag",
              deserialize_rejects_out_of_range_uvalue_tag);
    utest_run("deserialize loads instruction stream with 4-byte alignment",
              deserialize_loads_instruction_stream_with_4_byte_alignment);
    utest_run("deserialize rejects non-zero alignment padding",
              deserialize_rejects_non_zero_alignment_padding);
    utest_run("deserialize loads delta synclines and abs checkpoints",
              deserialize_loads_delta_synclines_and_abs_checkpoints);
    utest_run("deserialize rejects n_deltas not equal n_instructions",
              deserialize_rejects_n_deltas_not_equal_n_instructions);
    utest_run("verifier accepts minimal RET-only module",
              verifier_accepts_minimal_ret_only_module);
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
    utest_run("verifier accepts hand-crafted OP_MOVE module",
              verifier_accepts_hand_crafted_op_move_module);
    utest_run("serialize empty module produces 24-byte header plus zero-sized sections",
              serialize_empty_module_produces_24_byte_header_plus_zero_sized_sections);
    utest_run("serialize cap=0 returns required size without writing",
              serialize_cap_0_returns_required_size_without_writing);
    utest_run("serialize cap too small returns ULOAD_TRUNCATED negative",
              serialize_cap_too_small_returns_ULOAD_TRUNCATED_negative);
    utest_run("roundtrip AST_INT literal emit-serialize-deserialize",
              roundtrip_ast_int_literal);
    utest_run("roundtrip AST_BINARY 1+2 emit-serialize-deserialize",
              roundtrip_ast_binary_1_plus_2);
    utest_run("roundtrip AST_UNARY -5 emit-serialize-deserialize",
              roundtrip_ast_unary_neg_5);
    utest_run("destroy NULL module is a no-op",
              destroy_null_module_is_noop);
    utest_run("module load error name covers all codes",
              module_load_error_name_all_codes);
    utest_run("deserialize NULL module returns ULOAD_TRUNCATED",
              deserialize_null_module_returns_truncated);
    utest_run("deserialize OOM on constants allocation returns ULOAD_OOM",
              deserialize_oom_on_constants_allocation);
    utest_run("deserialize OOM on instructions allocation returns ULOAD_OOM",
              deserialize_oom_on_instructions_allocation);
    utest_run("deserialize loads UVAL_FLOAT constant",
              deserialize_loads_float_constant);
    utest_run("deserialize rejects non-monotonic abs_line checkpoints",
              deserialize_rejects_non_monotonic_abs_lines);
    utest_run("deserialize rejects NIL/BOOL/STR constant tag as corrupt",
              deserialize_rejects_nil_bool_str_constant_tag);
    utest_run("deserialize truncated at line_deltas returns ULOAD_TRUNCATED",
              deserialize_truncated_at_line_deltas);
    utest_run("deserialize OOM on abs_lines allocation returns ULOAD_OOM",
              deserialize_oom_on_abs_lines_allocation);
    utest_run("deserialize rejects abs_line pc out of range",
              deserialize_rejects_abs_line_pc_out_of_range);
    utest_run("deserialize rejects trailing bytes after syncline section",
              deserialize_rejects_trailing_bytes);
    utest_run("verifier rejects register B > max_reg",
              verifier_rejects_register_b_gt_max_reg);
    utest_run("verifier rejects register C > max_reg",
              verifier_rejects_register_c_gt_max_reg);
    utest_run("deserialize truncated at metadata max_reg returns ULOAD_TRUNCATED",
              deserialize_truncated_at_metadata_max_reg);
    utest_run("deserialize module grow reuses existing buffer cap",
              deserialize_module_grow_reuses_existing_cap);
}
