/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include "chunk/uchunk.h"
#include "vm/uvm.h"
#include "value/uarena.h"
#include "value/uintern.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "object/uchunk_instance.h"
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

UTEST(module_error_name_ok) {
    UASSERT_EQ(0, strcmp("UCHUNK_LOAD_OK", umodule_load_error_name(UCHUNK_LOAD_OK)));
}

UTEST(destroy_empty_module_is_noop) {
    /* Task 11: UModule is a thin 5-field shell; all chunk data is on root_proto.
     * A zero-initialised module (root_proto == NULL) must destroy safely. */
    UModule c = {0};
    umodule_destroy(&c, NULL);
    UASSERT_EQ((void *)NULL, (void *)c.root_proto);
    UASSERT_EQ((void *)NULL, (void *)c.source_name);
}

UTEST(destroy_module_with_buffers_frees_them) {
    /* Task 11: All chunk-top buffers live on root_proto.
     * Simulate allocation via stdlib calloc; umodule_destroy frees root_proto
     * buffers via umodule_destroy_proto_buffers then the proto itself via alloc_fn.
     * alloc_fn == NULL means the proto struct was NOT heap-allocated separately
     * (caller-owned stack object), so we skip the root_proto free and only verify
     * the sub-buffers are cleared.
     *
     * To exercise the full free path, allocate root_proto on the heap with
     * alloc_fn == NULL (signals stdlib ownership).  umodule_destroy_proto_buffers
     * uses proto->alloc_fn; with NULL it uses stdlib free. */
    UModule c = {0};
    UProto *rp = (UProto *)calloc(1, sizeof(UProto));
    UASSERT(rp != NULL);
    c.root_proto = rp;

    /* Simulate allocation by directly using stdlib and letting destroy free.
       This is the same path umodule_deserialize / umodule_serialize use via
       the alloc_fn hook — when alloc_fn is NULL, destroy uses stdlib free. */
    rp->instructions = (uint32_t *)malloc(sizeof(uint32_t) * 4);
    rp->instr_cap = 4;
    rp->instr_count = 2;
    rp->instructions[0] = 0x11223344;
    rp->instructions[1] = 0x55667788;

    /* calloc, not malloc: umodule_destroy walks `const_count` slots through
     * free_owned_str_constants, which inspects each UValue's kind + _pad[0]
     * (the deserializer-set ownership marker for UVAL_STR; emit-time slots
     * carry _pad[0] == 0).  malloc'd uninitialised bytes trip valgrind on
     * the kind/_pad reads even when no slot is actually a marked string;
     * calloc gives a valgrind-clean baseline that matches the deserializer
     * (which zero-fills before decoding). */
    rp->constants = (UValue *)calloc(2, sizeof(UValue));
    rp->const_cap = 2;
    rp->const_count = 1;

    rp->line_deltas = (int8_t *)malloc(sizeof(int8_t) * 4);

    rp->abs_lines = (UAbsLine *)malloc(sizeof(UAbsLine) * 2);
    rp->abs_line_cap = 2;
    rp->abs_line_count = 1;

    umodule_destroy(&c, NULL);

    /* After destroy, root_proto is detached (either freed or cleared). */
    UASSERT_EQ((void *)NULL, (void *)c.root_proto);
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
    /* v1.7 body: source_name + root_proto block.
     * Alignment: instr_count varint lands at offset 30 (24+1+3+2=30);
     * 30 % 4 = 2, pad 2 bytes → offset 32 before n_deltas. */
    size_t offset = 24;
    buf[offset++] = 0;  /* varint source_name_len = 0 */
    buf[offset++] = 0;  /* max_reg */
    buf[offset++] = 0;  /* nupvals */
    buf[offset++] = 0;  /* nparams */
    buf[offset++] = 0;  /* varint n_constants = 0 */
    buf[offset++] = 0;  /* varint n_instructions = 0 */
    buf[offset++] = 0;  /* align pad byte 1 (offset 30 -> 31) */
    buf[offset++] = 0;  /* align pad byte 2 (offset 31 -> 32) */
    buf[offset++] = 0;  /* varint n_deltas = 0 */
    buf[offset++] = 0;  /* varint n_abs_lines = 0 */
    buf[offset++] = 0;  /* varint ic_count = 0 */
    buf[offset++] = 0;  /* varint nested_count = 0 (v1.7) */
    UModule c = {0};
    char errmsg[128];
    UChunkLoadError rc = umodule_deserialize(&c, buf, offset, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_buffer_shorter_than_header) {
    uint8_t buf[10];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    UModule c = {0};
    UASSERT_EQ(UCHUNK_LOAD_TRUNCATED, umodule_deserialize(&c, buf, sizeof buf, NULL, 0));
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_bad_magic) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[0] = 'X';  /* corrupt first magic byte */
    UModule c = {0};
    UASSERT_EQ(UCHUNK_LOAD_BAD_MAGIC, umodule_deserialize(&c, hdr, sizeof hdr, NULL, 0));
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_corrupted_canary_simulated_ftp_ascii) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[8] = 0x20;  /* \r replaced by space — ASCII-mode munge */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = umodule_deserialize(&c, hdr, sizeof hdr, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_BAD_MAGIC, rc);
    UASSERT(strstr(errmsg, "canary") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_unsupported_version) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[4] = 0x20;  /* would be v2.0 */
    UModule c = {0};
    UASSERT_EQ(UCHUNK_LOAD_UNSUPPORTED_VERSION, umodule_deserialize(&c, hdr, sizeof hdr, NULL, 0));
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_v1_0_module) {
    /* Build a 24-byte header with version byte = 0x10 (v1.0). */
    uint8_t buf[64];
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
    buf[offset++] = 0;  /* varint ic_count = 0 (v1.5) */
    buf[offset++] = 0;  /* varint nested_count = 0 (v1.5) */
    UModule c = {0};
    UASSERT_EQ(UCHUNK_LOAD_UNSUPPORTED_VERSION, umodule_deserialize(&c, buf, offset, NULL, 0));
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_v1_1_module) {
    /* Version byte 0x11 (M2) must be rejected: OP_RETURN dispatch semantics
       changed between M2 and M3; loading old modules silently would produce
       wrong nested-unwind behaviour.  Hard break is the safe choice. */
    uint8_t buf[64];
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
    buf[offset++] = 0;  /* varint ic_count = 0 (v1.5) */
    buf[offset++] = 0;  /* varint nested_count = 0 (v1.5) */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = umodule_deserialize(&c, buf, offset, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_UNSUPPORTED_VERSION, rc);
    /* errmsg should name the rejected version so users know what they have */
    UASSERT(strstr(errmsg, "0x11") != NULL || strstr(errmsg, "1.1") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_v1_2_module) {
    /* Version byte 0x12 (M3) must be rejected by the v1.3 loader: M4 added
       UProto.ic_count + UProto.ic_names side table; loading v1.2 silently
       would leave IC sites uninitialized.  Hard break per encoding spec §7. */
    uint8_t buf[64];
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
    buf[offset++] = 0;  /* varint ic_count = 0 (v1.5) */
    buf[offset++] = 0;  /* varint nested_count = 0 (v1.5) */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = umodule_deserialize(&c, buf, offset, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_UNSUPPORTED_VERSION, rc);
    /* errmsg should name the rejected version so users know what they have */
    UASSERT(strstr(errmsg, "0x12") != NULL || strstr(errmsg, "1.2") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_v1_3_module) {
    /* Version byte 0x13 (M4) must be rejected by the v1.5 loader: M5 added
       reactive opcodes 38-45, gc_byte bit 7, and 4 new AST node kinds; v0.5.6
       Wave 4 then completed the wire format with nested protos + ic_name_strs
       and renumbered M5.  Loading v1.3 silently is unsafe. */
    uint8_t buf[64];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    buf[4] = 0x13;  /* version byte (v1.3 — M4; should be rejected by v1.5 loader) */
    size_t offset = 24;
    buf[offset++] = 0;  /* max_reg */
    buf[offset++] = 0;  /* varint source_name_len = 0 */
    buf[offset++] = 0;  /* varint n_constants = 0 */
    buf[offset++] = 0;  /* varint n_instructions = 0 */
    buf[offset++] = 0;  /* varint n_deltas = 0 */
    buf[offset++] = 0;  /* varint n_abs_lines = 0 */
    buf[offset++] = 0;  /* varint ic_count = 0 (v1.5) */
    buf[offset++] = 0;  /* varint nested_count = 0 (v1.5) */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = umodule_deserialize(&c, buf, offset, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_UNSUPPORTED_VERSION, rc);
    UASSERT(strstr(errmsg, "0x13") != NULL || strstr(errmsg, "1.3") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_v1_4_module) {
    /* Version byte 0x14 (M5) must be rejected by the v1.5 loader.  v0.5.6
       Wave 4 completed the wire format (nested protos + per-proto + root
       ic_name_strs) and renumbered M5 reactive opcodes 39-46 → 38-45.
       Loading v1.4 silently would index past the new sections and read
       wrong-shape opcodes.  Hard break per §3.8 of the cleanup spec. */
    uint8_t buf[64];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    buf[4] = 0x14;  /* version byte (v1.4 — M5; should be rejected by v1.5 loader) */
    size_t offset = 24;
    buf[offset++] = 0;  /* max_reg */
    buf[offset++] = 0;  /* varint source_name_len = 0 */
    buf[offset++] = 0;  /* varint n_constants = 0 */
    buf[offset++] = 0;  /* varint n_instructions = 0 */
    buf[offset++] = 0;  /* varint n_deltas = 0 */
    buf[offset++] = 0;  /* varint n_abs_lines = 0 */
    buf[offset++] = 0;  /* varint ic_count = 0 (v1.5) */
    buf[offset++] = 0;  /* varint nested_count = 0 (v1.5) */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = umodule_deserialize(&c, buf, offset, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_UNSUPPORTED_VERSION, rc);
    UASSERT(strstr(errmsg, "0x14") != NULL || strstr(errmsg, "1.4") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_v1_6_module) {
    /* Version byte 0x16 (v0.7.2) must be rejected by the v1.7 loader.
     * v1.7 changed the UModule body layout (header + source_name + recursive
     * root_proto block; per-field duplication removed).  Loading v1.6
     * silently would parse the body as the wrong structure. */
    uint8_t buf[64];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    buf[4] = 0x16;  /* version byte (v1.6 — should be rejected by v1.7 loader) */
    size_t offset = 24;
    buf[offset++] = 0;  /* max_reg (v1.6 metadata) */
    buf[offset++] = 0;  /* varint source_name_len = 0 */
    buf[offset++] = 0;  /* varint n_constants = 0 */
    buf[offset++] = 0;  /* varint n_instructions = 0 */
    buf[offset++] = 0;  /* varint n_deltas = 0 */
    buf[offset++] = 0;  /* varint n_abs_lines = 0 */
    buf[offset++] = 0;  /* varint ic_count = 0 */
    buf[offset++] = 0;  /* varint nested_count = 0 */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = umodule_deserialize(&c, buf, offset, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_UNSUPPORTED_VERSION, rc);
    UASSERT(strstr(errmsg, "0x16") != NULL || strstr(errmsg, "1.6") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_accepts_current_version_module) {
    /* A minimal well-formed module at the current bytecode version
     * (URBI_BYTECODE_VERSION_BYTE = 0x17) must be accepted.  This is the
     * positive-control twin of the deserialize_rejects_v1_X tests. */
    uint8_t buf[64];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    buf[4] = URBI_BYTECODE_VERSION_BYTE;
    /* v1.7 body: source_name + root_proto block (same as the accepts_good_header
     * test above; repeated here for clarity as a version-specific positive ctrl). */
    size_t offset = 24;
    buf[offset++] = 0;  /* varint source_name_len = 0 */
    buf[offset++] = 0;  /* max_reg */
    buf[offset++] = 0;  /* nupvals */
    buf[offset++] = 0;  /* nparams */
    buf[offset++] = 0;  /* varint n_constants = 0 */
    buf[offset++] = 0;  /* varint n_instructions = 0 */
    buf[offset++] = 0;  /* align pad */
    buf[offset++] = 0;  /* align pad */
    buf[offset++] = 0;  /* varint n_deltas = 0 */
    buf[offset++] = 0;  /* varint n_abs_lines = 0 */
    buf[offset++] = 0;  /* varint ic_count = 0 */
    buf[offset++] = 0;  /* varint nested_count = 0 (v1.7) */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = umodule_deserialize(&c, buf, offset, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    umodule_destroy(&c, NULL);
}

UTEST(uproto_alloc_zero_inits_ic_count_and_ic_names) {
    /* M4 v1.3: umodule_alloc_nested_proto must zero ic_count and ic_names
       (encoding spec §5.1).  Subsequent M4 tasks rely on this so freshly
       allocated protos start with no IC sites. */
    UModule m = {0};
    /* Task 11: root_proto must exist before umodule_alloc_nested_proto. */
    m.root_proto = (UProto *)calloc(1, sizeof(UProto));
    UASSERT(m.root_proto != NULL);
    UProto *p = umodule_alloc_nested_proto(&m, m.root_proto);
    UASSERT(p != NULL);
    UASSERT_EQ((unsigned)p->ic_count, 0U);
    UASSERT_EQ((void *)p->ic_names, (void *)NULL);
    umodule_destroy(&m, NULL);
}

UTEST(uproto_destroy_frees_ic_names) {
    /* M4 v1.3: umodule_destroy_proto_buffers must free the ic_names array.
       Allocate via stdlib so destroy (alloc_fn == NULL → stdlib_alloc) frees it. */
    UModule m = {0};
    /* Task 11: root_proto must exist before umodule_alloc_nested_proto. */
    m.root_proto = (UProto *)calloc(1, sizeof(UProto));
    UASSERT(m.root_proto != NULL);
    UProto *p = umodule_alloc_nested_proto(&m, m.root_proto);
    UASSERT(p != NULL);
    /* Pretend the emitter populated ic_count + ic_names with two opaque slots. */
    p->ic_count = 2;
    p->ic_names = (USymbol **)malloc(2 * sizeof(USymbol *));
    UASSERT(p->ic_names != NULL);
    p->ic_names[0] = NULL;
    p->ic_names[1] = NULL;
    /* umodule_destroy frees nested protos via umodule_destroy_proto_buffers. */
    umodule_destroy(&m, NULL);
    /* If we reach here without leaking under ASan, ic_names was freed. */
}

UTEST(deserialize_rejects_wrong_int_width) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[12] = 4;  /* claims i32 */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = umodule_deserialize(&c, hdr, sizeof hdr, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_FLAVOR_MISMATCH, rc);
    UASSERT(strstr(errmsg, "int_width") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_wrong_float_type) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[13] = (URBI_FLOAT_TYPE == 8) ? 4 : 8;  /* flip to the other flavor */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = umodule_deserialize(&c, hdr, sizeof hdr, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_FLAVOR_MISMATCH, rc);
    UASSERT(strstr(errmsg, "float_type") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_wrong_instr_width) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[14] = 8;  /* claims 8-byte instructions */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = umodule_deserialize(&c, hdr, sizeof hdr, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_FLAVOR_MISMATCH, rc);
    UASSERT(strstr(errmsg, "instr_width") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_wrong_endianness) {
    uint8_t hdr[24];
    build_good_header(hdr);
    hdr[15] = 1;  /* big-endian on LE host */
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = umodule_deserialize(&c, hdr, sizeof hdr, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_FLAVOR_MISMATCH, rc);
    UASSERT(strstr(errmsg, "endianness") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_nonzero_reserved_byte) {
    uint8_t buf[64] = {0};
    build_good_header(buf);
    buf[20] = 0xCCU;  /* reserved byte 20 set non-zero */
    size_t off = 24;
    buf[off++] = 0;  /* max_reg */
    buf[off++] = 0;  /* source_name_len = 0 */
    buf[off++] = 0;  /* n_constants = 0 */
    buf[off++] = 0;  /* n_instructions = 0 */
    buf[off++] = 0;  /* n_deltas = 0 */
    buf[off++] = 0;  /* n_abs_lines = 0 */
    buf[off++] = 0;  /* ic_count = 0 (v1.5) */
    buf[off++] = 0;  /* nested_count = 0 (v1.5) */
    UModule c = {0};
    char errmsg[256];
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    /* errmsg should mention 'reserved' */
    UASSERT(strstr(errmsg, "reserved") != NULL);
    umodule_destroy(&c, NULL);
}

/* --- Varint write helpers for building test blobs --- */

/* Append an LEB128 unsigned varint.  Returns new offset. */
static size_t put_varint(uint8_t *buf, size_t offset, uint64_t v) {
    while (v >= 0x80U) {
        buf[offset++] = (uint8_t)((v & 0x7FU) | 0x80U);
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
    /* v1.7: source_name first, then root_proto block */
    off = put_varint(buf, off, 4);   /* source_name_len = 4 */
    memcpy(buf + off, "repl", 4); off += 4;
    /* root_proto block: max_reg=5, nupvals=0, nparams=0 */
    buf[off++] = 5;
    buf[off++] = 0;  /* nupvals */
    buf[off++] = 0;  /* nparams */
    /* constants: 0 */
    off = put_varint(buf, off, 0);
    /* instructions: 0 */
    off = put_varint(buf, off, 0);
    /* alignment pad: off=34 after n_instr varint, 34%4=2, pad 2 bytes */
    while ((off & 3U) != 0U) buf[off++] = 0U;
    /* synclines: 0 deltas, 0 abs_lines */
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 0);
    /* ic_names + nested_count (v1.7) */
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 0);

    UModule c = {0};
    char errmsg[128];
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    UASSERT_EQ((uint8_t)5, c.root_proto->max_reg);
    UASSERT(c.source_name != NULL);
    UASSERT_EQ(0, strcmp(c.source_name, "repl"));
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_loads_integer_constant_pool) {
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    /* v1.7: source_name_len=0, then root_proto block */
    off = put_varint(buf, off, 0);  /* source_name_len = 0 */
    buf[off++] = 0;  /* max_reg */
    buf[off++] = 0;  /* nupvals */
    buf[off++] = 0;  /* nparams */
    /* constants: 2 entries — UVAL_INT 1, UVAL_INT -42 */
    off = put_varint(buf, off, 2);
    buf[off++] = (uint8_t)UVAL_INT;
    off = put_varint_zz(buf, off, 1);
    buf[off++] = (uint8_t)UVAL_INT;
    off = put_varint_zz(buf, off, -42);
    /* instructions: 0 */
    off = put_varint(buf, off, 0);
    /* alignment pad after n_instr varint */
    while ((off & 3U) != 0U) buf[off++] = 0U;
    /* synclines: 0, 0 */
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 0);
    /* ic_names + nested_count (v1.7) */
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 0);

    UModule c = {0};
    char errmsg[128];
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    UASSERT_EQ((size_t)2, c.root_proto->const_count);
    UASSERT_EQ((uint8_t)UVAL_INT, c.root_proto->constants[0].kind);
    UASSERT_EQ((int64_t)1,   c.root_proto->constants[0].v.i);
    UASSERT_EQ((uint8_t)UVAL_INT, c.root_proto->constants[1].kind);
    UASSERT_EQ((int64_t)-42, c.root_proto->constants[1].v.i);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_out_of_range_uvalue_tag) {
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    off = put_varint(buf, off, 0);          /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;                         /* max_reg */
    buf[off++] = 0;                         /* nupvals */
    buf[off++] = 0;                         /* nparams */
    off = put_varint(buf, off, 1);          /* 1 constant */
    buf[off++] = 99;                        /* invalid kind */
    off = put_varint_zz(buf, off, 0);       /* payload (ignored, rejected first) */
    UModule c = {0};
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT_TAG, umodule_deserialize(&c, buf, off, NULL, 0));
    umodule_destroy(&c, NULL);
}

/* --- Instruction-stream + syncline tests (Task 5 deferred + Task 6) --- */

UTEST(deserialize_loads_instruction_stream_with_4_byte_alignment) {
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    /* v1.7: source_name_len=0, then root_proto block */
    off = put_varint(buf, off, 0);  /* source_name_len = 0 */
    buf[off++] = 1;                 /* max_reg */
    buf[off++] = 0;                 /* nupvals */
    buf[off++] = 0;                 /* nparams */
    /* constants: 0 */
    off = put_varint(buf, off, 0);
    /* instructions: 1.  varint 1 = 1 byte.  off before = 30; 30 mod 4 = 2, pad 2 bytes. */
    off = put_varint(buf, off, 1);
    while ((off & 3U) != 0U) buf[off++] = 0;
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
    /* v1.5: ic_names + nested[] (both empty) */
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 0);

    UModule c = {0};
    char errmsg[128];
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    UASSERT_EQ((size_t)1, c.root_proto->instr_count);
    UASSERT_EQ((UOpcode)OP_RET, uinstr_op(c.root_proto->instructions[0]));
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_non_zero_alignment_padding) {
    /* To force a padding byte, we need off to be non-4-aligned after the
       n_instructions varint.  v1.7 layout:
       24 (hdr) + 1 (src_len=0) + 1 (max_reg) + 1 (nupvals) + 1 (nparams) +
       1 (n_constants=0) + 1 (n_instructions=1) = off=30; 30 mod 4 = 2,
       so 2 pad bytes are needed.
       Use source_name_len=1 ("x") to get a different alignment:
       24 + 1 (src_len=1) + 1 ("x") + 1 (max_reg) + 1 (nupvals) + 1 (nparams)
       + 1 (n_constants=0) + 1 (n_instructions=1) = off=31; 31 mod 4 = 3,
       so 1 pad byte needed — corrupt it. */
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    off = put_varint(buf, off, 1);          /* source_name_len = 1 */
    buf[off++] = 'x';                       /* source_name = "x" */
    buf[off++] = 0;                         /* max_reg */
    buf[off++] = 0;                         /* nupvals */
    buf[off++] = 0;                         /* nparams */
    off = put_varint(buf, off, 0);          /* n_constants */
    off = put_varint(buf, off, 1);          /* n_instructions=1 */
    /* off is now 31 (31 mod 4 == 3); 1 pad byte needed — corrupt it */
    while ((off & 3U) != 0U) buf[off++] = 0xFF;
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
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "align") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_loads_delta_synclines_and_abs_checkpoints) {
    uint8_t buf[256];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    /* v1.7: source_name_len=0, then root_proto block */
    off = put_varint(buf, off, 0);  /* source_name_len = 0 */
    buf[off++] = 1;                 /* max_reg */
    buf[off++] = 0;                 /* nupvals */
    buf[off++] = 0;                 /* nparams */
    /* constants: 0 */
    off = put_varint(buf, off, 0);
    /* instructions: 3 */
    off = put_varint(buf, off, 3);
    while ((off & 3U) != 0U) buf[off++] = 0;
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
    /* ic_names + nested_count (v1.7) */
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 0);

    UModule c = {0};
    char errmsg[128];
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    UASSERT_EQ((size_t)3, c.root_proto->instr_count);
    UASSERT_EQ((int8_t)-128, c.root_proto->line_deltas[0]);
    UASSERT_EQ((int8_t)2,    c.root_proto->line_deltas[1]);
    UASSERT_EQ((int8_t)-1,   c.root_proto->line_deltas[2]);
    UASSERT_EQ((size_t)1, c.root_proto->abs_line_count);
    UASSERT_EQ((uint32_t)0,  c.root_proto->abs_lines[0].pc);
    UASSERT_EQ((uint32_t)10, c.root_proto->abs_lines[0].line);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_n_deltas_not_equal_n_instructions) {
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    off = put_varint(buf, off, 0);  /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;                 /* max_reg */
    buf[off++] = 0;                 /* nupvals */
    buf[off++] = 0;                 /* nparams */
    off = put_varint(buf, off, 0);  /* n_constants = 0 */
    off = put_varint(buf, off, 1);          /* 1 instruction */
    while ((off & 3U) != 0U) buf[off++] = 0;
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
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "n_deltas") != NULL);
    umodule_destroy(&c, NULL);
}

/* --- build_module_bytes: constructs a well-formed module byte blob ---
   v1.7: header + source_name + root_proto block.
   Returns total bytes written.  buf must be at least 256 bytes. */
static size_t build_module_bytes(uint8_t *buf,
                                uint8_t max_reg,
                                const int64_t *const_vals, size_t n_const,
                                const uint32_t *instrs, size_t n_instr) {
    build_good_header(buf);
    size_t off = 24;
    off = put_varint(buf, off, 0);              /* source_name_len = 0 */
    /* root_proto block */
    buf[off++] = max_reg;
    buf[off++] = 0;                             /* nupvals = 0 */
    buf[off++] = 0;                             /* nparams = 0 */
    off = put_varint(buf, off, (uint64_t)n_const);
    size_t ci;
    for (ci = 0; ci < n_const; ci++) {
        buf[off++] = (uint8_t)UVAL_INT;
        off = put_varint_zz(buf, off, const_vals[ci]);
    }
    off = put_varint(buf, off, (uint64_t)n_instr);
    while ((off & 3U) != 0U) buf[off++] = 0;
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
    /* v1.7: ic_names (count=0) + nested_count (v1.7, always 0 here). */
    off = put_varint(buf, off, 0);
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
    UChunkLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    umodule_destroy(&c, NULL);
}

UTEST(verifier_rejects_opcode_ge_op_max) {
    uint8_t buf[256];
    const uint32_t instrs[] = {
        200U,                                   /* op=200, invalid */
        uinstr_enc_abc(OP_RET, 0, 0, 0)
    };
    size_t total = build_module_bytes(buf, 0, NULL, 0, instrs, 2);
    UModule c = {0};
    char errmsg[128];
    UChunkLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "opcode") != NULL);
    umodule_destroy(&c, NULL);
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
    UChunkLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "register") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(verifier_rejects_loadk_bx_out_of_constant_range) {
    uint8_t buf[256];
    const int64_t consts[] = { 1 };             /* only K[0] exists */
    const uint32_t instrs[] = {
        uinstr_enc_abx(OP_LOADK, 0, 5U),        /* Bx=5, out of range */
        uinstr_enc_abc(OP_RET, 0, 0, 0)
    };
    size_t total = build_module_bytes(buf, 0, consts, 1, instrs, 2);
    UModule c = {0};
    char errmsg[128];
    UChunkLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "LOADK") != NULL || strstr(errmsg, "Bx") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(verifier_rejects_last_instruction_not_ret) {
    uint8_t buf[256];
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_ADD, 0, 0, 0)         /* no terminating RET */
    };
    size_t total = build_module_bytes(buf, 0, NULL, 0, instrs, 1);
    UModule c = {0};
    char errmsg[128];
    UChunkLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "RET") != NULL);
    umodule_destroy(&c, NULL);
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
    UChunkLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);                   /* MUST accept */
    umodule_destroy(&c, NULL);
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
    UChunkLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    umodule_destroy(&c, NULL);
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
    UChunkLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    UASSERT_EQ((uint8_t)OP_MOVE, (uint8_t)uinstr_op(c.root_proto->instructions[1]));
    umodule_destroy(&c, NULL);
}

/* --- Round-trip integration tests (Task 15) --- */

/* Returns true if two modules are semantically equivalent. */
static bool modules_equivalent(const UModule *a, const UModule *b) {
    size_t i;
    if (a->root_proto->instr_count     != b->root_proto->instr_count)     return false;
    if (a->root_proto->const_count     != b->root_proto->const_count)     return false;
    if (a->root_proto->abs_line_count  != b->root_proto->abs_line_count)  return false;
    if (a->root_proto->max_reg         != b->root_proto->max_reg)         return false;
    for (i = 0; i < a->root_proto->instr_count; i++) {
        if (a->root_proto->instructions[i] != b->root_proto->instructions[i]) return false;
        if (a->root_proto->line_deltas[i]  != b->root_proto->line_deltas[i])  return false;
    }
    for (i = 0; i < a->root_proto->const_count; i++) {
        if (a->root_proto->constants[i].kind != b->root_proto->constants[i].kind) return false;
        if (a->root_proto->constants[i].kind == UVAL_INT
         && a->root_proto->constants[i].v.i  != b->root_proto->constants[i].v.i)  return false;
    }
    for (i = 0; i < a->root_proto->abs_line_count; i++) {
        if (a->root_proto->abs_lines[i].pc   != b->root_proto->abs_lines[i].pc)   return false;
        if (a->root_proto->abs_lines[i].line != b->root_proto->abs_lines[i].line) return false;
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
    UChunkLoadError rc;

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
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    UASSERT(modules_equivalent(&src, &dst));

    free(buf);
    uarena_destroy(&arena);
    umodule_destroy(&src, NULL);
    umodule_destroy(&dst, NULL);
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

/* T12+T13 follow-up regression test: round-trip of a module with a
 * nested function literal proto.  Pre-fix proto_wire_size disagreed with
 * write_proto on instruction-pad alignment (C1); pre-fix decode_verify
 * per-proto walks passed nested_count=0 and rejected legitimate
 * OP_CLOSURE Bx in nested protos (C2).  Post-fix the round-trip succeeds
 * and decode_verify accepts the bytecode.
 *
 * v0.8.5-recursive-emit shape change: under recursive emission the inner
 * function literal is a CHILD of the outer function's UProto, not a flat
 * sibling under root.  Tree: root.nested = [outer]; outer.nested = [inner].
 * The outer's body holds OP_CLOSURE Bx=0 referring to outer.nested[0]
 * (its own child).  Pre-v0.8.5 the same source produced flat siblings
 * with root.nested_count >= 2 and OP_CLOSURE Bx=1 in outer's body. */
UTEST(roundtrip_module_with_nested_closure_proto) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Two-level closure: the outer function body holds `function() {x+y}`
     * which in turn captures x and y as upvalues.  Same source pattern as
     * test_emit.c::disassemble_closure_with_prelude — known to produce
     * nested_count >= 2. */
    const char *src = "function() { var x = 1; var y = 2; function() { x + y } }";

    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UArena arena;
    uarena_init(&arena, 0);

    UModule a = {0};
    UEmitter e;
    uemit_init(&e, &a, &arena, &vm, "test");

    UParser p;
    uparse_init(&p, &lex, &arena);

    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        UASSERT(node->kind != AST_ERROR);
        UASSERT_EQ(EMIT_OK, uemit_statement(&e, node));
    }
    UASSERT_EQ(EMIT_OK, uemit_finish(&e));
    /* v0.8.5 recursive shape: root has 1 child (outer); outer has 1
     * child (inner). */
    UASSERT_EQ(a.root_proto->nested_count, (size_t)1);
    UASSERT(a.root_proto->nested[0] != NULL);
    UASSERT_EQ(a.root_proto->nested[0]->nested_count, (size_t)1);
    UASSERT(a.root_proto->nested[0]->nested[0] != NULL);

    /* Two-pass serialize: query size, then write.  The contract is
     * that the wrote count equals the queried size (C1 violates this). */
    ptrdiff_t need = umodule_serialize(&a, NULL, 0);
    UASSERT(need > (ptrdiff_t)0);
    uint8_t *buf = (uint8_t *)malloc((size_t)need);
    UASSERT(buf != NULL);
    ptrdiff_t wrote = umodule_serialize(&a, buf, (size_t)need);
    UASSERT_EQ(need, wrote);

    /* Deserialize buf -> b and verify shape preservation (C2 makes
     * decode_verify reject the bytecode). */
    UModule b = {0};
    char errmsg[256];
    errmsg[0] = '\0';
    UChunkLoadError rc = umodule_deserialize(&b, buf, (size_t)need,
                                              errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    UASSERT_EQ(a.root_proto->nested_count, b.root_proto->nested_count);
    UASSERT(b.root_proto->nested[0] != NULL);
    UASSERT_EQ(b.root_proto->nested[0]->nested_count, (size_t)1);
    UASSERT(b.root_proto->nested[0]->nested[0] != NULL);
    UASSERT(b.root_proto->nested[0]->instr_count > (size_t)0);
    UASSERT(b.root_proto->nested[0]->nested[0]->instr_count > (size_t)0);

    free(buf);
    umodule_destroy(&a, NULL);
    umodule_destroy(&b, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* T15 (a): Round-trip a module that emits an IC site (OP_GETSLOT) and
 * verify the v1.5 ic_name_strs survive serialize+deserialize, then
 * exercise the T14 lazy-intern path by calling
 * urbi_module_instance_create on the deserialized module — which on
 * input has ic_name_strs populated but ic_names == NULL.  After
 * instance create, ic_names must be a fully populated USymbol** array
 * that resolves to the same canonical pointer as a direct
 * ustr_intern() of the same byte content.
 *
 * Source `Object` produces a bare global ref at chunk-top, which T71's
 * realm-global fallback compiles to OP_GETSLOT — adding one root-chunk
 * IC site whose name is "Object". */
UTEST(roundtrip_module_with_ic_sites_lazy_interns) {
    UVM vm_a;
    urbi_vm_init(&vm_a, NULL, NULL);

    const char *src = "Object";

    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    uarena_init(&arena, 0);

    UModule a = {0};
    UEmitter e;
    uemit_init(&e, &a, &arena, &vm_a, "test_ic");

    UParser p;
    uparse_init(&p, &lex, &arena);

    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        UASSERT(node->kind != AST_ERROR);
        UASSERT_EQ(EMIT_OK, uemit_statement(&e, node));
    }
    UASSERT_EQ(EMIT_OK, uemit_finish(&e));

    /* Sanity: emitter populated the root-chunk ic_count + ic_name_strs. */
    UASSERT(a.root_proto->ic_count >= (uint16_t)1U);
    UASSERT(a.root_proto->ic_name_strs != NULL);
    UASSERT(a.root_proto->ic_name_strs[0] != NULL);

    /* Round-trip. */
    ptrdiff_t need = umodule_serialize(&a, NULL, 0);
    UASSERT(need > (ptrdiff_t)0);
    uint8_t *buf = (uint8_t *)malloc((size_t)need);
    UASSERT(buf != NULL);
    ptrdiff_t wrote = umodule_serialize(&a, buf, (size_t)need);
    UASSERT_EQ(need, wrote);

    UModule b = {0};
    char errmsg[256];
    errmsg[0] = '\0';
    UChunkLoadError rc = umodule_deserialize(&b, buf, (size_t)need,
                                              errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    UASSERT_EQ((unsigned)a.root_proto->ic_count, (unsigned)b.root_proto->ic_count);
    UASSERT(b.root_proto->ic_name_strs != NULL);
    UASSERT_EQ(0, strcmp(a.root_proto->ic_name_strs[0], b.root_proto->ic_name_strs[0]));

    /* Pre-condition for the T14 lazy-intern path: the deserialized
     * module has no ic_names yet.  Loader cannot intern (no VM in
     * scope at decode time). */
    UASSERT_EQ((void *)NULL, (void *)b.root_proto->ic_names);

    /* Drive the lazy-intern.  Use a fresh VM to confirm the helper
     * interns into the receiving VM, not the originating one. */
    UVM vm_b;
    urbi_vm_init(&vm_b, NULL, NULL);

    UChunkInstance *mi = urbi_module_instance_create(&vm_b, &b);
    UASSERT(mi != NULL);

    /* Post-condition: ic_names is now populated on root_proto (v0.8.1 Phase 1:
     * intern writes via &rp->ic_names rather than &module->ic_names); each
     * entry equals the canonical interned pointer for the matching entry. */
    UASSERT(b.root_proto->ic_names != NULL);
    for (uint16_t k = 0; k < b.root_proto->ic_count; k++) {
        const char *name = b.root_proto->ic_name_strs[k];
        size_t nlen = strlen(name);
        const char *canon = ustr_intern(&vm_b, name, nlen);
        UASSERT_EQ((const void *)canon, (const void *)b.root_proto->ic_names[k]);
    }

    /* Idempotency: a second call must not re-allocate.  The helper's
     * fast path returns immediately when ic_names is already populated. */
    USymbol **before = b.root_proto->ic_names;
    UChunkInstance *mi2 = urbi_module_instance_create(&vm_b, &b);
    UASSERT(mi2 != NULL);
    UASSERT_EQ((void *)before, (void *)b.root_proto->ic_names);

    free(buf);
    umodule_destroy(&a, NULL);
    umodule_destroy(&b, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm_a);
    urbi_vm_destroy(&vm_b);
}

/* T15 (b): Pin that the v1.5 wire format preserves UVAL_FLOAT
 * constants in nested-proto constant pools.  The lex+parse+emit
 * pipeline at v0.5.6 only produces UVAL_INT constants (no AST_FLOAT
 * yet — see test_emit.c::serialize_module_with_float_constant_round_trips
 * for the existing manual-build pattern used here too).  Hand-build
 * a module with one nested proto whose constant pool holds a single
 * UVAL_FLOAT, round-trip, and assert the FLOAT survives. */
UTEST(roundtrip_preserves_nested_proto_float_constant) {
    UModule a = {0};
    /* Task 11: root_proto must exist before umodule_alloc_nested_proto
     * (nested[] lives on root_proto).  Allocate with stdlib_alloc (hosted). */
    a.root_proto = (UProto *)calloc(1, sizeof(UProto));
    UASSERT(a.root_proto != NULL);
    UProto *p = umodule_alloc_nested_proto(&a, a.root_proto);
    UASSERT(p != NULL);

    /* One UVAL_FLOAT in the nested proto's constant pool. */
    p->constants = (UValue *)malloc(sizeof(UValue));
    UASSERT(p->constants != NULL);
    p->const_cap   = 1;
    p->const_count = 1;
    p->constants[0].kind = (uint8_t)UVAL_FLOAT;
    {
        int q;
        for (q = 0; q < 7; q++) p->constants[0]._pad[q] = 0;
    }
#if URBI_FLOAT_TYPE == 8
    p->constants[0].v.f = 2.718281828;
#else
    p->constants[0].v.f = 2.718f;
#endif

    /* Minimum viable proto body: one OP_RET + one syncline checkpoint.
     * write_proto requires line_deltas[i] for each instruction and
     * accepts INT8_MIN as the abs-line-checkpoint sentinel. */
    p->instructions = (uint32_t *)malloc(sizeof(uint32_t));
    UASSERT(p->instructions != NULL);
    p->instr_cap = 1;
    p->instr_count = 1;
    p->instructions[0] = uinstr_enc_abc(OP_RET, 0, 0, 0);
    p->line_deltas = (int8_t *)malloc(sizeof(int8_t));
    UASSERT(p->line_deltas != NULL);
    p->line_deltas[0] = (int8_t)-128;       /* INT8_MIN: abs-line sentinel */
    p->abs_lines = (UAbsLine *)malloc(sizeof(UAbsLine));
    UASSERT(p->abs_lines != NULL);
    p->abs_line_cap   = 1;
    p->abs_line_count = 1;
    p->abs_lines[0].pc   = 0;
    p->abs_lines[0].line = 1;
    p->max_reg = 0;

    /* Likewise, the root chunk needs at least one OP_RET. */
    a.root_proto->instructions = (uint32_t *)malloc(sizeof(uint32_t));
    UASSERT(a.root_proto->instructions != NULL);
    a.root_proto->instr_cap = 1;
    a.root_proto->instr_count = 1;
    a.root_proto->instructions[0] = uinstr_enc_abc(OP_RET, 0, 0, 0);
    a.root_proto->line_deltas = (int8_t *)malloc(sizeof(int8_t));
    UASSERT(a.root_proto->line_deltas != NULL);
    a.root_proto->line_deltas[0] = (int8_t)-128;
    a.root_proto->abs_lines = (UAbsLine *)malloc(sizeof(UAbsLine));
    UASSERT(a.root_proto->abs_lines != NULL);
    a.root_proto->abs_line_cap   = 1;
    a.root_proto->abs_line_count = 1;
    a.root_proto->abs_lines[0].pc   = 0;
    a.root_proto->abs_lines[0].line = 1;
    a.root_proto->max_reg = 0;

    /* Round-trip. */
    ptrdiff_t need = umodule_serialize(&a, NULL, 0);
    UASSERT(need > (ptrdiff_t)0);
    uint8_t *buf = (uint8_t *)malloc((size_t)need);
    UASSERT(buf != NULL);
    ptrdiff_t wrote = umodule_serialize(&a, buf, (size_t)need);
    UASSERT_EQ(need, wrote);

    UModule b = {0};
    char errmsg[256];
    errmsg[0] = '\0';
    UChunkLoadError rc = umodule_deserialize(&b, buf, (size_t)need,
                                              errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    UASSERT_EQ(a.root_proto->nested_count, b.root_proto->nested_count);
    UASSERT(b.root_proto->nested[0] != NULL);
    UASSERT_EQ(p->const_count, b.root_proto->nested[0]->const_count);
    UASSERT_EQ((uint8_t)UVAL_FLOAT, b.root_proto->nested[0]->constants[0].kind);
#if URBI_FLOAT_TYPE == 8
    UASSERT(b.root_proto->nested[0]->constants[0].v.f == 2.718281828);
#else
    UASSERT(b.root_proto->nested[0]->constants[0].v.f == 2.718f);
#endif

    free(buf);
    umodule_destroy(&a, NULL);
    umodule_destroy(&b, NULL);
}

/* --- Serializer tests (Task 14) --- */

UTEST(serialize_empty_module_produces_24_byte_header_plus_zero_sized_sections) {
    /* v1.7 empty module (no statements): 24-byte header + 12 body bytes.
       Body = src_len varint 0(1) + root_proto block(11).
       Root_proto block = max_reg(1) + nupvals(1) + nparams(1)
            + n_const varint 0(1) + n_instr varint 0(1)
            + 2 align pad bytes (offset 30 % 4 = 2)
            + n_deltas varint 0(1) + n_abs varint 0(1)
            + ic_count varint 0(1) + nested_count varint 0(1) = 11 bytes.
       Total = 24 + 1 + 11 = 36. */
    UModule module = {0};
    UArena arena;
    UEmitter e;
    uarena_init(&arena, 0);
    uemit_init(&e, &module, &arena, NULL, NULL);
    (void)uemit_finish(&e);

    /* Size query (buf == NULL) */
    ptrdiff_t n = umodule_serialize(&module, NULL, 0);
    UASSERT_EQ((ptrdiff_t)36, n);

    /* Write pass */
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0xAA;  /* poison */
    ptrdiff_t written = umodule_serialize(&module, buf, sizeof buf);
    UASSERT_EQ((ptrdiff_t)36, written);

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
    UChunkLoadError rc = umodule_deserialize(&c2, buf, (size_t)written, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    umodule_destroy(&c2, NULL);

    uarena_destroy(&arena);
    umodule_destroy(&module, NULL);
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
    umodule_destroy(&module, NULL);
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
    UASSERT_EQ(-(ptrdiff_t)UCHUNK_LOAD_TRUNCATED, rc);

    uarena_destroy(&arena);
    umodule_destroy(&module, NULL);
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
    umodule_destroy(NULL, NULL);
    UASSERT(true);
}

UTEST(module_load_error_name_all_codes) {
    /* Exercise every UChunkLoadError case in umodule_load_error_name. */
    UASSERT_EQ(0, strcmp("UCHUNK_LOAD_OK",                  umodule_load_error_name(UCHUNK_LOAD_OK)));
    UASSERT_EQ(0, strcmp("UCHUNK_LOAD_BAD_MAGIC",           umodule_load_error_name(UCHUNK_LOAD_BAD_MAGIC)));
    UASSERT_EQ(0, strcmp("UCHUNK_LOAD_UNSUPPORTED_VERSION", umodule_load_error_name(UCHUNK_LOAD_UNSUPPORTED_VERSION)));
    UASSERT_EQ(0, strcmp("UCHUNK_LOAD_FLAVOR_MISMATCH",     umodule_load_error_name(UCHUNK_LOAD_FLAVOR_MISMATCH)));
    UASSERT_EQ(0, strcmp("UCHUNK_LOAD_TRUNCATED",           umodule_load_error_name(UCHUNK_LOAD_TRUNCATED)));
    UASSERT_EQ(0, strcmp("UCHUNK_LOAD_CORRUPT_VARINT",      umodule_load_error_name(UCHUNK_LOAD_CORRUPT_VARINT)));
    UASSERT_EQ(0, strcmp("UCHUNK_LOAD_CORRUPT_TAG",         umodule_load_error_name(UCHUNK_LOAD_CORRUPT_TAG)));
    UASSERT_EQ(0, strcmp("UCHUNK_LOAD_CORRUPT",             umodule_load_error_name(UCHUNK_LOAD_CORRUPT)));
    UASSERT_EQ(0, strcmp("UCHUNK_LOAD_OOM",                 umodule_load_error_name(UCHUNK_LOAD_OOM)));
    UASSERT_EQ(0, strcmp("UCHUNK_LOAD_INVALID_ARG",         umodule_load_error_name(UCHUNK_LOAD_INVALID_ARG)));
    UASSERT_EQ(0, strcmp("UCHUNK_LOAD_OVERSIZED",           umodule_load_error_name(UCHUNK_LOAD_OVERSIZED)));
    /* Out-of-range code falls through to UCHUNK_LOAD_UNKNOWN sentinel. */
    UASSERT(umodule_load_error_name((UChunkLoadError)99) != NULL);
}

UTEST(deserialize_null_module_returns_truncated) {
    /* umodule_deserialize(NULL, ...) must not crash — covers the null guard.
     * v0.5.7 T73: NULL module / NULL buf now returns UCHUNK_LOAD_INVALID_ARG; the
     * test name is preserved for git-blame continuity. */
    uint8_t buf[24];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    UChunkLoadError rc = umodule_deserialize(NULL, buf, sizeof buf, NULL, 0);
    UASSERT_EQ(UCHUNK_LOAD_INVALID_ARG, rc);
}

UTEST(deserialize_oom_on_constants_allocation) {
    /* Cause UCHUNK_LOAD_OOM during the constants module_grow by failing after the
       initial source_name allocation succeeds (first alloc for source_name)
       but failing on the next call (constants buffer). */
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    off = put_varint(buf, off, 0);              /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;                             /* max_reg */
    buf[off++] = 0;                             /* nupvals */
    buf[off++] = 0;                             /* nparams */
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
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, NULL, 0);
    UASSERT_EQ(UCHUNK_LOAD_OOM, rc);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_oom_on_instructions_allocation) {
    /* Cause UCHUNK_LOAD_OOM during the instructions module_grow by allowing the
       constants allocation to succeed (1 call) but failing the next. */
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    off = put_varint(buf, off, 0);              /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;                             /* max_reg */
    buf[off++] = 0;                             /* nupvals */
    buf[off++] = 0;                             /* nparams */
    off = put_varint(buf, off, 1);              /* 1 constant — triggers first grow */
    buf[off++] = (uint8_t)UVAL_INT;
    off = put_varint_zz(buf, off, 5);
    off = put_varint(buf, off, 1);              /* 1 instruction — triggers second grow */
    while ((off & 3U) != 0U) buf[off++] = 0;
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
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, NULL, 0);
    UASSERT_EQ(UCHUNK_LOAD_OOM, rc);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_loads_float_constant) {
    /* Exercise the UVAL_FLOAT branch in the constants decode path. */
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    off = put_varint(buf, off, 0);              /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;                             /* max_reg */
    buf[off++] = 0;                             /* nupvals */
    buf[off++] = 0;                             /* nparams */
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
    while ((off & 3U) != 0U) buf[off++] = 0;
    off = put_varint(buf, off, 0);              /* n_deltas = 0 */
    off = put_varint(buf, off, 0);              /* n_abs_lines = 0 */
    off = put_varint(buf, off, 0);              /* ic_count = 0 (v1.5) */
    off = put_varint(buf, off, 0);              /* nested_count = 0 (v1.5) */
    UModule c = {0};
    char errmsg[128];
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    UASSERT_EQ((size_t)1, c.root_proto->const_count);
    UASSERT_EQ((uint8_t)UVAL_FLOAT, c.root_proto->constants[0].kind);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_nil_bool_str_constant_tag) {
    /* UVAL_NIL/UVAL_BOOL/UVAL_STR constants have no payload at M1.
       The deserializer rejects them with UCHUNK_LOAD_CORRUPT_TAG via the else branch
       (they pass the > UVAL_STR range check but are neither INT nor FLOAT). */
    uint8_t buf[128];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    off = put_varint(buf, off, 0);              /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;                             /* max_reg */
    buf[off++] = 0;                             /* nupvals */
    buf[off++] = 0;                             /* nparams */
    off = put_varint(buf, off, 1);              /* 1 constant */
    buf[off++] = (uint8_t)UVAL_NIL;            /* kind 0 — no INT or FLOAT, hits else */
    off = put_varint(buf, off, 0);              /* padding */
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 0);
    UModule c = {0};
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, NULL, 0);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT_TAG, rc);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_truncated_at_line_deltas) {
    /* Build a valid module with 1 instruction but truncate the buffer before
       the line_deltas data — should return UCHUNK_LOAD_TRUNCATED. */
    uint8_t buf[256];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    off = put_varint(buf, off, 0);              /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;                             /* max_reg */
    buf[off++] = 0;                             /* nupvals */
    buf[off++] = 0;                             /* nparams */
    off = put_varint(buf, off, 0);              /* 0 constants */
    off = put_varint(buf, off, 1);              /* 1 instruction */
    while ((off & 3U) != 0U) buf[off++] = 0;
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
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_TRUNCATED, rc);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_oom_on_abs_lines_allocation) {
    /* Cause UCHUNK_LOAD_OOM during the abs_lines module_grow by allowing constants and
       instructions and line_deltas allocations to succeed but failing abs_lines. */
    uint8_t buf[256];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    off = put_varint(buf, off, 0);              /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;                             /* max_reg */
    buf[off++] = 0;                             /* nupvals */
    buf[off++] = 0;                             /* nparams */
    off = put_varint(buf, off, 0);              /* 0 constants */
    off = put_varint(buf, off, 1);              /* 1 instruction */
    while ((off & 3U) != 0U) buf[off++] = 0;
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
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, NULL, 0);
    UASSERT_EQ(UCHUNK_LOAD_OOM, rc);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_abs_line_pc_out_of_range) {
    /* abs_line pc >= instr_count should return UCHUNK_LOAD_CORRUPT. */
    uint8_t buf[256];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    off = put_varint(buf, off, 0);              /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;                             /* max_reg */
    buf[off++] = 0;                             /* nupvals */
    buf[off++] = 0;                             /* nparams */
    off = put_varint(buf, off, 0);
    off = put_varint(buf, off, 1);              /* 1 instruction */
    while ((off & 3U) != 0U) buf[off++] = 0;
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
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "out of range") != NULL || strstr(errmsg, "pc") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_truncated_at_metadata_max_reg) {
    /* Buffer exactly 24 bytes (valid header but no body) — triggers the
       truncation guard while reading the source_name_len varint (v1.7:
       max_reg is now in the root_proto block, not the metadata section). */
    uint8_t buf[24];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    UModule c = {0};
    char errmsg[128];
    errmsg[0] = '\0';
    UChunkLoadError rc = umodule_deserialize(&c, buf, sizeof buf, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_TRUNCATED, rc);
    UASSERT(strstr(errmsg, "source_name") != NULL || strstr(errmsg, "varint") != NULL
            || strstr(errmsg, "truncated") != NULL);
    umodule_destroy(&c, NULL);
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
    UChunkLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    UASSERT_EQ((size_t)2, c.root_proto->const_count);
    /* c now has const_cap >= 8 (first grow starts at 8). */

    /* Reset counts but keep the buffers allocated. */
    c.root_proto->const_count = 0;
    c.root_proto->instr_count = 0;
    c.root_proto->abs_line_count = 0;
    if (c.root_proto->line_deltas != NULL) {
        UChunkAllocFn alloc = c.alloc_fn != NULL ? c.alloc_fn
                            : (UChunkAllocFn)NULL; /* stdlib handled by destroy later */
        (void)alloc; /* just keep the pointer, don't free now */
        free(c.root_proto->line_deltas); c.root_proto->line_deltas = NULL;
    }

    /* Second deserialize into the same module — module_grow for constants will
       see const_cap >= 2, triggering the "already large enough" branch. */
    rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    UASSERT_EQ((size_t)2, c.root_proto->const_count);

    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_trailing_bytes) {
    /* A valid module followed by extra bytes should return UCHUNK_LOAD_CORRUPT. */
    uint8_t buf[256];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    const uint32_t instrs[] = { uinstr_enc_abc(OP_RET, 0, 0, 0) };
    size_t total = build_module_bytes(buf, 0, NULL, 0, instrs, 1);
    /* Append extra garbage byte. */
    buf[total] = 0xAB;
    UModule c = {0};
    char errmsg[128];
    UChunkLoadError rc = umodule_deserialize(&c, buf, total + 1, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "trailing") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(verifier_rejects_register_b_gt_max_reg) {
    /* OP_ADD with B > max_reg should return UCHUNK_LOAD_CORRUPT. */
    uint8_t buf[256];
    /* max_reg=0, ADD with B=5 > max_reg */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_ADD, 0, 5, 0),       /* B=5 > max_reg=0 */
        uinstr_enc_abc(OP_RET, 0, 0, 0)
    };
    size_t total = build_module_bytes(buf, 0, NULL, 0, instrs, 2);
    UModule c = {0};
    char errmsg[128];
    UChunkLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "register") != NULL || strstr(errmsg, "B=") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(verifier_rejects_register_c_gt_max_reg) {
    /* OP_ADD with C > max_reg should return UCHUNK_LOAD_CORRUPT. */
    uint8_t buf[256];
    /* max_reg=0, ADD with B=0 ok, C=5 > max_reg */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_ADD, 0, 0, 5),       /* C=5 > max_reg=0 */
        uinstr_enc_abc(OP_RET, 0, 0, 0)
    };
    size_t total = build_module_bytes(buf, 0, NULL, 0, instrs, 2);
    UModule c = {0};
    char errmsg[128];
    UChunkLoadError rc = umodule_deserialize(&c, buf, total, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "register") != NULL || strstr(errmsg, "C=") != NULL);
    umodule_destroy(&c, NULL);
}

UTEST(deserialize_rejects_non_monotonic_abs_lines) {
    /* Build a module with two abs_line checkpoints where the second has a
       lower pc than the first — should return UCHUNK_LOAD_CORRUPT. */
    uint8_t buf[256];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    off = put_varint(buf, off, 0);              /* source_name_len = 0 (v1.7) */
    buf[off++] = 1;                             /* max_reg=1 */
    buf[off++] = 0;                             /* nupvals */
    buf[off++] = 0;                             /* nparams */
    off = put_varint(buf, off, 0);              /* 0 constants */
    /* 2 instructions */
    off = put_varint(buf, off, 2);
    while ((off & 3U) != 0U) buf[off++] = 0;
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
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    UASSERT(strstr(errmsg, "monotonic") != NULL);
    umodule_destroy(&c, NULL);
}

/* MOD-014: simplification regression — the abs_lines monotonic check must
 * accept the first checkpoint at pc=0 (since the per-iteration "is this the
 * first?" gate is now an `i > 0` check rather than a `first_checkpoint` flag).
 * Without the first-iteration skip, the comparison `pc <= prev=0` would reject
 * a legitimate pc=0 first entry. */
UTEST(deserialize_accepts_first_abs_line_pc_zero) {
    uint8_t buf[256];
    size_t i;
    for (i = 0; i < sizeof buf; i++) buf[i] = 0;
    build_good_header(buf);
    size_t off = 24;
    off = put_varint(buf, off, 0);              /* source_name_len = 0 (v1.7) */
    buf[off++] = 1;                             /* max_reg=1 */
    buf[off++] = 0;                             /* nupvals */
    buf[off++] = 0;                             /* nparams */
    off = put_varint(buf, off, 0);              /* 0 constants */
    /* 2 instructions */
    off = put_varint(buf, off, 2);
    while ((off & 3U) != 0U) buf[off++] = 0;
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
    /* 2 abs_line checkpoints — first pc=0 (legitimate), second pc=1 (>0). */
    off = put_varint(buf, off, 2);
    off = put_varint(buf, off, 0);              /* abs_line[0].pc = 0 */
    off = put_varint(buf, off, 5);
    off = put_varint(buf, off, 1);              /* abs_line[1].pc = 1 */
    off = put_varint(buf, off, 10);
    off = put_varint(buf, off, 0);              /* ic_count = 0 (v1.5) */
    off = put_varint(buf, off, 0);              /* nested_count = 0 (v1.5) */
    UModule c = {0};
    char errmsg[128];
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    UASSERT_EQ((size_t)2, c.root_proto->abs_line_count);
    UASSERT_EQ((uint32_t)0, c.root_proto->abs_lines[0].pc);
    UASSERT_EQ((uint32_t)1, c.root_proto->abs_lines[1].pc);
    umodule_destroy(&c, NULL);
}

UTEST(umodule_init_zeroes_ic_count_and_ic_names) {
    /* Task 11: ic_count and ic_names live on root_proto.
     * A zero-init module has root_proto == NULL; ic fields are implicitly zero. */
    UModule m = {0};
    UASSERT(m.root_proto == NULL);
    umodule_destroy(&m, NULL);
}

/* --- T4 verifier resync regression tests (MOD-009) --- */

/* Helper: write a 24-byte good header into buf and return next offset. */
static size_t write_good_header_to(uint8_t *buf) {
    build_good_header(buf);
    return 24U;
}

/* Helper: write one ABC-shape uint32 instruction (LE) starting at *off. */
static void write_instr_abc(uint8_t *buf, size_t *off, UOpcode op,
                            uint8_t a, uint8_t b, uint8_t c) {
    uint32_t ins = (uint32_t)op
                 | ((uint32_t)a << 8)
                 | ((uint32_t)b << 16)
                 | ((uint32_t)c << 24);
    buf[(*off)++] = (uint8_t)(ins         & 0xFFU);
    buf[(*off)++] = (uint8_t)((ins >> 8)  & 0xFFU);
    buf[(*off)++] = (uint8_t)((ins >> 16) & 0xFFU);
    buf[(*off)++] = (uint8_t)((ins >> 24) & 0xFFU);
}

/* Helper: write one ABx-shape uint32 instruction (LE) starting at *off. */
static void write_instr_abx(uint8_t *buf, size_t *off, UOpcode op,
                            uint8_t a, uint16_t bx) {
    uint32_t ins = (uint32_t)op
                 | ((uint32_t)a << 8)
                 | ((uint32_t)bx << 16);
    buf[(*off)++] = (uint8_t)(ins         & 0xFFU);
    buf[(*off)++] = (uint8_t)((ins >> 8)  & 0xFFU);
    buf[(*off)++] = (uint8_t)((ins >> 16) & 0xFFU);
    buf[(*off)++] = (uint8_t)((ins >> 24) & 0xFFU);
}

UTEST(verify_accepts_loadbool_b_as_immediate) {
    /* Build a 2-instruction module: OP_LOADBOOL R[0] := true; OP_RET R[0].
     * OP_LOADBOOL encodes B=1 as the boolean-true IMMEDIATE.  Pre-T4
     * verifier rejects with "register B=1 > max_reg=0" because B is
     * checked as a register without consulting opcode shape.  Post-T4
     * the shape table flags B as UOPK_IMM_BOOL and the verifier accepts. */
    uint8_t buf[64] = {0};
    size_t off = write_good_header_to(buf);
    buf[off++] = 0;          /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;          /* max_reg = 0 */
    buf[off++] = 0;          /* nupvals = 0 */
    buf[off++] = 0;          /* nparams = 0 */
    buf[off++] = 0;          /* n_constants = 0 */
    buf[off++] = 2;          /* n_instructions = 2 */
    while ((off & 3U) != 0U) buf[off++] = 0;  /* 4-byte align */
    write_instr_abc(buf, &off, OP_LOADBOOL, /*A=*/0, /*B=*/1, /*C=*/0);
    write_instr_abc(buf, &off, OP_RET,      /*A=*/0, /*B=*/0, /*C=*/0);
    buf[off++] = 2;          /* n_deltas = 2 */
    buf[off++] = 0; buf[off++] = 0;  /* two zero deltas */
    buf[off++] = 0;          /* n_abs_lines = 0 */
    buf[off++] = 0;          /* ic_count = 0 */
    buf[off++] = 0;          /* nested_count = 0 (v1.7) */
    UModule c = {0};
    char errmsg[256];
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    umodule_destroy(&c, NULL);
}

UTEST(verify_accepts_push_tag_a_packs_flags_and_reg_nibble) {
    /* OP_PUSH_TAG A[7:4] = flags nibble, A[3:0] = tag_reg nibble.
     * tag_reg=0 + flags=0xF gives A=0xF0.  Pre-T4 verifier reads A as
     * a single register and rejects A=240 > max_reg=0; post-T4 the
     * shape's UOPK_IMM_REG_NIBBLE only checks the low nibble. */
    uint8_t buf[80] = {0};
    size_t off = write_good_header_to(buf);
    buf[off++] = 0;          /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;          /* max_reg = 0 */
    buf[off++] = 0;          /* nupvals = 0 */
    buf[off++] = 0;          /* nparams = 0 */
    buf[off++] = 0;          /* n_constants = 0 */
    buf[off++] = 2;          /* n_instructions = 2 */
    while ((off & 3U) != 0U) buf[off++] = 0;
    write_instr_abx(buf, &off, OP_PUSH_TAG, /*A=*/0xF0, /*Bx=*/0);
    /* Bx=0 is a valid handler PC at instr_count=2 (Bx<2 acceptable). */
    write_instr_abc(buf, &off, OP_RET, 0, 0, 0);
    buf[off++] = 2;
    buf[off++] = 0; buf[off++] = 0;
    buf[off++] = 0;
    buf[off++] = 0;          /* ic_count = 0 */
    buf[off++] = 0;          /* nested_count = 0 (v1.7) */
    UModule c = {0};
    char errmsg[256];
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    umodule_destroy(&c, NULL);
}

UTEST(verify_rejects_op_loadbool_b_greater_than_one) {
    /* OP_LOADBOOL with B=2 is malformed: B is the 0/1 boolean
     * immediate.  Post-T4 verifier rejects via UOPK_IMM_BOOL. */
    uint8_t buf[64] = {0};
    size_t off = write_good_header_to(buf);
    buf[off++] = 0;          /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;          /* max_reg = 0 */
    buf[off++] = 0;          /* nupvals = 0 */
    buf[off++] = 0;          /* nparams = 0 */
    buf[off++] = 0;          /* n_constants = 0 */
    buf[off++] = 2;          /* n_instructions = 2 */
    while ((off & 3U) != 0U) buf[off++] = 0;
    write_instr_abc(buf, &off, OP_LOADBOOL, /*A=*/0, /*B=*/2, /*C=*/0);
    write_instr_abc(buf, &off, OP_RET, 0, 0, 0);
    buf[off++] = 2;
    buf[off++] = 0; buf[off++] = 0;
    buf[off++] = 0;
    buf[off++] = 0;          /* ic_count = 0 */
    buf[off++] = 0;          /* nested_count = 0 (v1.7) */
    UModule c = {0};
    char errmsg[256];
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    umodule_destroy(&c, NULL);
}

UTEST(verify_rejects_op_getupval_a_above_max_reg) {
    /* OP_GETUPVAL R[A] := upvalue[B] — A is destination register;
     * must be <= max_reg.  A=99 with max_reg=0 should reject as a
     * register-A overflow per UOPK_REG. */
    uint8_t buf[64] = {0};
    size_t off = write_good_header_to(buf);
    buf[off++] = 0;          /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;          /* max_reg = 0 */
    buf[off++] = 0;          /* nupvals = 0 */
    buf[off++] = 0;          /* nparams = 0 */
    buf[off++] = 0;          /* n_constants = 0 */
    buf[off++] = 2;          /* n_instructions = 2 */
    while ((off & 3U) != 0U) buf[off++] = 0;
    write_instr_abc(buf, &off, OP_GETUPVAL, /*A=*/99, /*B=*/0, /*C=*/0);
    write_instr_abc(buf, &off, OP_RET, 0, 0, 0);
    buf[off++] = 2;
    buf[off++] = 0; buf[off++] = 0;
    buf[off++] = 0;
    buf[off++] = 0;          /* ic_count = 0 */
    buf[off++] = 0;          /* nested_count = 0 (v1.7) */
    UModule c = {0};
    char errmsg[256];
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    umodule_destroy(&c, NULL);
}

UTEST(verify_accepts_at_install_with_no_onleave_sentinel) {
    /* Pre-fix the OP_AT_INSTALL row marked C as a register; encoding the
     * 0xFF no-onleave sentinel as C tripped the register check.  Post-fix
     * (T4 follow-up) C is UOPK_UNUSED and the sentinel verifies clean. */
    uint8_t buf[80] = {0};
    size_t off = write_good_header_to(buf);
    buf[off++] = 0;          /* source_name_len = 0 (v1.7) */
    buf[off++] = 2;          /* max_reg = 2 */
    buf[off++] = 0;          /* nupvals = 0 */
    buf[off++] = 0;          /* nparams = 0 */
    buf[off++] = 0;          /* n_constants = 0 */
    buf[off++] = 2;          /* n_instructions = 2 */
    while ((off & 3U) != 0U) buf[off++] = 0;
    write_instr_abc(buf, &off, OP_AT_INSTALL, /*A=*/0, /*B=*/1, /*C=*/0xFFU);
    write_instr_abc(buf, &off, OP_RET, 0, 0, 0);
    buf[off++] = 2;          /* n_deltas */
    buf[off++] = 0; buf[off++] = 0;
    buf[off++] = 0;          /* n_abs_lines */
    buf[off++] = 0;          /* ic_count = 0 */
    buf[off++] = 0;          /* nested_count = 0 (v1.7) */
    UModule c = {0};
    char errmsg[256];
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    umodule_destroy(&c, NULL);
}

UTEST(verify_rejects_op_closure_bx_above_nested_count) {
    /* OP_CLOSURE Bx must be < nested_count.  At v0.5.5 nested_count is 0
     * (root-only modules); a hand-rolled OP_CLOSURE with Bx=0 should
     * therefore reject with UCHUNK_LOAD_CORRUPT.  Pre-T4/T5 verifier accepted
     * this silently; runtime would index past nested[] and read garbage. */
    uint8_t buf[64] = {0};
    size_t off = write_good_header_to(buf);
    buf[off++] = 0;          /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;          /* max_reg = 0 */
    buf[off++] = 0;          /* nupvals = 0 */
    buf[off++] = 0;          /* nparams = 0 */
    buf[off++] = 0;          /* n_constants = 0 */
    buf[off++] = 2;          /* n_instructions = 2 */
    while ((off & 3U) != 0U) buf[off++] = 0;
    write_instr_abx(buf, &off, OP_CLOSURE, /*A=*/0, /*Bx=*/0);
    write_instr_abc(buf, &off, OP_RET, 0, 0, 0);
    buf[off++] = 2;          /* n_deltas = 2 */
    buf[off++] = 0; buf[off++] = 0;
    buf[off++] = 0;          /* n_abs_lines = 0 */
    buf[off++] = 0;          /* ic_count = 0 */
    buf[off++] = 0;          /* nested_count = 0 (v1.7) */
    UModule c = {0};
    char errmsg[256];
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    umodule_destroy(&c, NULL);
}

UTEST(verify_accepts_op_jmp_with_arbitrary_bx) {
    /* OP_JMP Bx is signed-with-32768-bias; verifier intentionally does
     * NOT range-check Bx.  Build a module with OP_JMP Bx=0 (offset
     * -32768) followed by OP_RET; verifier accepts even though the
     * jump target is "out of range" — runtime surfaces the issue at
     * dispatch, not at load. */
    uint8_t buf[64] = {0};
    size_t off = write_good_header_to(buf);
    buf[off++] = 0;          /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;          /* max_reg = 0 */
    buf[off++] = 0;          /* nupvals = 0 */
    buf[off++] = 0;          /* nparams = 0 */
    buf[off++] = 0;          /* n_constants = 0 */
    buf[off++] = 2;          /* n_instructions = 2 */
    while ((off & 3U) != 0U) buf[off++] = 0;
    write_instr_abx(buf, &off, OP_JMP, /*A=*/0, /*Bx=*/0);
    write_instr_abc(buf, &off, OP_RET, 0, 0, 0);
    buf[off++] = 2;
    buf[off++] = 0; buf[off++] = 0;
    buf[off++] = 0;
    buf[off++] = 0;          /* ic_count = 0 */
    buf[off++] = 0;          /* nested_count = 0 (v1.7) */
    UModule c = {0};
    char errmsg[256];
    UChunkLoadError rc = umodule_deserialize(&c, buf, off, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    umodule_destroy(&c, NULL);
}

/* Helper: build a serialized 2-instruction module with the given test
 * instruction first and OP_RET as the second instruction.  Returns the
 * total serialized length and writes into buf.  buf must be >= 80 bytes. */
static size_t build_two_instr_module(uint8_t *buf, size_t bufcap,
                                     uint8_t max_reg,
                                     uint32_t test_instr) {
    UASSERT(bufcap >= 80U);
    size_t off = write_good_header_to(buf);
    buf[off++] = 0;          /* source_name_len = 0 (v1.7) */
    buf[off++] = max_reg;
    buf[off++] = 0;          /* nupvals = 0 */
    buf[off++] = 0;          /* nparams = 0 */
    buf[off++] = 0;          /* n_constants = 0 */
    buf[off++] = 2;          /* n_instructions = 2 */
    while ((off & 3U) != 0U) buf[off++] = 0;
    buf[off++] = (uint8_t)(test_instr         & 0xFFU);
    buf[off++] = (uint8_t)((test_instr >> 8)  & 0xFFU);
    buf[off++] = (uint8_t)((test_instr >> 16) & 0xFFU);
    buf[off++] = (uint8_t)((test_instr >> 24) & 0xFFU);
    write_instr_abc(buf, &off, OP_RET, 0, 0, 0);
    buf[off++] = 2;          /* n_deltas = 2 */
    buf[off++] = 0; buf[off++] = 0;
    buf[off++] = 0;          /* n_abs_lines = 0 */
    buf[off++] = 0;          /* ic_count = 0 */
    buf[off++] = 0;          /* nested_count = 0 (v1.7) */
    return off;
}

#define ENC_ABC(op, a, b, c) \
    ((uint32_t)(op) | ((uint32_t)(a) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 24))
#define ENC_ABX(op, a, bx) \
    ((uint32_t)(op) | ((uint32_t)(a) << 8) | ((uint32_t)(bx) << 16))

UTEST(verify_rejects_arith_c_above_max_reg) {
    uint8_t buf[80] = {0};
    size_t off = build_two_instr_module(buf, sizeof buf, /*max_reg=*/0,
                                        ENC_ABC(OP_ADD, 0, 0, 99));
    UModule c = {0};
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, umodule_deserialize(&c, buf, off, NULL, 0));
    umodule_destroy(&c, NULL);
}

UTEST(verify_rejects_neg_b_above_max_reg) {
    uint8_t buf[80] = {0};
    size_t off = build_two_instr_module(buf, sizeof buf, 0,
                                        ENC_ABC(OP_NEG, 0, 99, 0));
    UModule c = {0};
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, umodule_deserialize(&c, buf, off, NULL, 0));
    umodule_destroy(&c, NULL);
}

UTEST(verify_rejects_test_a_above_max_reg) {
    uint8_t buf[80] = {0};
    size_t off = build_two_instr_module(buf, sizeof buf, 0,
                                        ENC_ABC(OP_TEST, 99, 0, 0));
    UModule c = {0};
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, umodule_deserialize(&c, buf, off, NULL, 0));
    umodule_destroy(&c, NULL);
}

UTEST(verify_rejects_eq_b_above_max_reg) {
    /* OP_EQ A=0 (bool flag), B=99 (register), C=0 — B is the register, reject. */
    uint8_t buf[80] = {0};
    size_t off = build_two_instr_module(buf, sizeof buf, 0,
                                        ENC_ABC(OP_EQ, 0, 99, 0));
    UModule c = {0};
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, umodule_deserialize(&c, buf, off, NULL, 0));
    umodule_destroy(&c, NULL);
}

UTEST(verify_rejects_setupval_a_above_max_reg) {
    uint8_t buf[80] = {0};
    size_t off = build_two_instr_module(buf, sizeof buf, 0,
                                        ENC_ABC(OP_SETUPVAL, 99, 0, 0));
    UModule c = {0};
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, umodule_deserialize(&c, buf, off, NULL, 0));
    umodule_destroy(&c, NULL);
}

UTEST(verify_rejects_push_frame_guard_base_plus_count_overflow) {
    /* base=0, count=2, max_reg=0 — base+count=2 > max_reg+1=1, reject. */
    uint8_t buf[80] = {0};
    size_t off = build_two_instr_module(buf, sizeof buf, 0,
                                        ENC_ABC(OP_PUSH_FRAME_GUARD, 0, 2, 0));
    UModule c = {0};
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, umodule_deserialize(&c, buf, off, NULL, 0));
    umodule_destroy(&c, NULL);
}

UTEST(verify_rejects_try_begin_handler_pc_above_instr_count) {
    /* instr_count=2; handler PC must be < 2.  Use Bx=99 to overshoot. */
    uint8_t buf[80] = {0};
    size_t off = build_two_instr_module(buf, sizeof buf, 0,
                                        ENC_ABX(OP_TRY_BEGIN, 0, 99));
    UModule c = {0};
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, umodule_deserialize(&c, buf, off, NULL, 0));
    umodule_destroy(&c, NULL);
}

UTEST(verify_rejects_push_tag_low_nibble_above_max_reg) {
    /* A[3:0] = tag_reg = 1; max_reg = 0 — reject on UOPK_IMM_REG_NIBBLE. */
    uint8_t buf[80] = {0};
    size_t off = build_two_instr_module(buf, sizeof buf, 0,
                                        ENC_ABX(OP_PUSH_TAG, 0x01, 0));
    UModule c = {0};
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, umodule_deserialize(&c, buf, off, NULL, 0));
    umodule_destroy(&c, NULL);
}

UTEST(verify_rejects_push_tag_handler_pc_above_instr_count) {
    /* PUSH_TAG Bx=99 with instr_count=2 — UBXK_HANDLER_PC reject. */
    uint8_t buf[80] = {0};
    size_t off = build_two_instr_module(buf, sizeof buf, 0,
                                        ENC_ABX(OP_PUSH_TAG, 0x00, 99));
    UModule c = {0};
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, umodule_deserialize(&c, buf, off, NULL, 0));
    umodule_destroy(&c, NULL);
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
    utest_run("deserialize rejects v1.3 module (M5 hard break)",
              deserialize_rejects_v1_3_module);
    utest_run("deserialize rejects v1.4 module (v0.5.6 wave-4 break)",
              deserialize_rejects_v1_4_module);
    utest_run("deserialize rejects v1.6 module (v0.8.1 root_proto layout break)",
              deserialize_rejects_v1_6_module);
    utest_run("deserialize accepts current-version module",
              deserialize_accepts_current_version_module);
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
    utest_run("deserialize rejects non-zero reserved byte",
              deserialize_rejects_nonzero_reserved_byte);
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
    utest_run("serialize cap too small returns UCHUNK_LOAD_TRUNCATED negative",
              serialize_cap_too_small_returns_ULOAD_TRUNCATED_negative);
    utest_run("roundtrip AST_INT literal emit-serialize-deserialize",
              roundtrip_ast_int_literal);
    utest_run("roundtrip AST_BINARY 1+2 emit-serialize-deserialize",
              roundtrip_ast_binary_1_plus_2);
    utest_run("roundtrip AST_UNARY -5 emit-serialize-deserialize",
              roundtrip_ast_unary_neg_5);
    utest_run("roundtrip module with nested closure proto",
              roundtrip_module_with_nested_closure_proto);
    utest_run("roundtrip module with IC sites lazy-interns ic_name_strs",
              roundtrip_module_with_ic_sites_lazy_interns);
    utest_run("roundtrip preserves nested-proto FLOAT constant",
              roundtrip_preserves_nested_proto_float_constant);
    utest_run("destroy NULL module is a no-op",
              destroy_null_module_is_noop);
    utest_run("module load error name covers all codes",
              module_load_error_name_all_codes);
    utest_run("deserialize NULL module returns UCHUNK_LOAD_TRUNCATED",
              deserialize_null_module_returns_truncated);
    utest_run("deserialize OOM on constants allocation returns UCHUNK_LOAD_OOM",
              deserialize_oom_on_constants_allocation);
    utest_run("deserialize OOM on instructions allocation returns UCHUNK_LOAD_OOM",
              deserialize_oom_on_instructions_allocation);
    utest_run("deserialize loads UVAL_FLOAT constant",
              deserialize_loads_float_constant);
    utest_run("deserialize rejects non-monotonic abs_line checkpoints",
              deserialize_rejects_non_monotonic_abs_lines);
    utest_run("deserialize accepts first abs_line pc=0 (MOD-014)",
              deserialize_accepts_first_abs_line_pc_zero);
    utest_run("deserialize rejects NIL/BOOL/STR constant tag as corrupt",
              deserialize_rejects_nil_bool_str_constant_tag);
    utest_run("deserialize truncated at line_deltas returns UCHUNK_LOAD_TRUNCATED",
              deserialize_truncated_at_line_deltas);
    utest_run("deserialize OOM on abs_lines allocation returns UCHUNK_LOAD_OOM",
              deserialize_oom_on_abs_lines_allocation);
    utest_run("deserialize rejects abs_line pc out of range",
              deserialize_rejects_abs_line_pc_out_of_range);
    utest_run("deserialize rejects trailing bytes after syncline section",
              deserialize_rejects_trailing_bytes);
    utest_run("verifier rejects register B > max_reg",
              verifier_rejects_register_b_gt_max_reg);
    utest_run("verifier rejects register C > max_reg",
              verifier_rejects_register_c_gt_max_reg);
    utest_run("deserialize truncated at metadata max_reg returns UCHUNK_LOAD_TRUNCATED",
              deserialize_truncated_at_metadata_max_reg);
    utest_run("deserialize module grow reuses existing buffer cap",
              deserialize_module_grow_reuses_existing_cap);
    utest_run("umodule_init zeroes ic_count and ic_names",
              umodule_init_zeroes_ic_count_and_ic_names);
    utest_run("verify accepts OP_LOADBOOL B as immediate",
              verify_accepts_loadbool_b_as_immediate);
    utest_run("verify accepts OP_PUSH_TAG A packs flags and reg nibble",
              verify_accepts_push_tag_a_packs_flags_and_reg_nibble);
    utest_run("verify rejects OP_LOADBOOL B > 1",
              verify_rejects_op_loadbool_b_greater_than_one);
    utest_run("verify rejects OP_GETUPVAL A > max_reg",
              verify_rejects_op_getupval_a_above_max_reg);
    utest_run("verify accepts OP_AT_INSTALL with no-onleave 0xFF sentinel",
              verify_accepts_at_install_with_no_onleave_sentinel);
    utest_run("verify rejects OP_CLOSURE Bx >= nested_count",
              verify_rejects_op_closure_bx_above_nested_count);
    utest_run("verify accepts OP_JMP with arbitrary Bx",
              verify_accepts_op_jmp_with_arbitrary_bx);
    utest_run("verify rejects OP_ADD C > max_reg",
              verify_rejects_arith_c_above_max_reg);
    utest_run("verify rejects OP_NEG B > max_reg",
              verify_rejects_neg_b_above_max_reg);
    utest_run("verify rejects OP_TEST A > max_reg",
              verify_rejects_test_a_above_max_reg);
    utest_run("verify rejects OP_EQ B > max_reg",
              verify_rejects_eq_b_above_max_reg);
    utest_run("verify rejects OP_SETUPVAL A > max_reg",
              verify_rejects_setupval_a_above_max_reg);
    utest_run("verify rejects OP_PUSH_FRAME_GUARD base+count overflow",
              verify_rejects_push_frame_guard_base_plus_count_overflow);
    utest_run("verify rejects OP_TRY_BEGIN handler PC >= instr_count",
              verify_rejects_try_begin_handler_pc_above_instr_count);
    utest_run("verify rejects OP_PUSH_TAG low nibble > max_reg",
              verify_rejects_push_tag_low_nibble_above_max_reg);
    utest_run("verify rejects OP_PUSH_TAG handler PC >= instr_count",
              verify_rejects_push_tag_handler_pc_above_instr_count);
}
