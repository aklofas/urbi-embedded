/* SPDX-License-Identifier: BSD-3-Clause */
/* Loader hardening tests — decode depth cap and OP_CALL register-window check.
 *
 * Covers:
 *   B3/REPL-01: decode_proto recurses unboundedly; a chain of >64 nested protos
 *     must be rejected via UCHUNK_LOAD_CORRUPT before overflowing the MCU stack.
 *   VM-CORE-02: the verifier never cross-checks OP_CALL's A+B register window
 *     against max_reg+1; a chunk with A+B > max_reg+1 must be rejected.
 *
 * Builder approach: byte-level hand-built chunks using file-local helper
 * functions (same pattern as test_verifier_cross_byte.c / test_verify_chunk_bounds.c;
 * no shared header dependency). */

#include "utest.h"
#include "chunk/uchunk.h"
#include "stdlib/stdlib_boot.h"    /* urbi_stdlib_bytecode / _len */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define UTEST(name) static void name(void)

/* ── bytecode builder helpers ──────────────────────────────────────────────── */

static void clh_header(uint8_t hdr[24]) {
    size_t i;
    for (i = 0; i < 24; i++) hdr[i] = 0;
    hdr[0] = 'U'; hdr[1] = 'R'; hdr[2] = 'B'; hdr[3] = 'I';
    hdr[4] = (uint8_t)URBI_BYTECODE_VERSION_BYTE;
    hdr[5] = 0x00;
    hdr[6] = 0x19; hdr[7] = 0x93;
    hdr[8] = '\r'; hdr[9] = '\n';
    hdr[10] = 0x1A; hdr[11] = '\n';
    hdr[12] = 8; hdr[13] = 8; hdr[14] = 4; hdr[15] = 0;
}

static size_t clh_varint(uint8_t *buf, size_t off, uint64_t v) {
    while (v >= 0x80U) {
        buf[off++] = (uint8_t)((v & 0x7FU) | 0x80U);
        v >>= 7;
    }
    buf[off++] = (uint8_t)v;
    return off;
}

static size_t clh_instr(uint8_t *buf, size_t off, uint32_t w) {
    buf[off++] = (uint8_t)(w & 0xFFU);
    buf[off++] = (uint8_t)((w >> 8)  & 0xFFU);
    buf[off++] = (uint8_t)((w >> 16) & 0xFFU);
    buf[off++] = (uint8_t)((w >> 24) & 0xFFU);
    return off;
}

/* Serialize one proto with a single OP_RET instruction.
 * has_child: 1 → nested_count=1, 0 → nested_count=0. */
static size_t clh_emit_proto(uint8_t *buf, size_t off, int has_child) {
    buf[off++] = 0;  /* max_reg */
    buf[off++] = 0;  /* nupvals */
    buf[off++] = 0;  /* nparams */
    off = clh_varint(buf, off, 0);  /* n_const */
    off = clh_varint(buf, off, 1);  /* n_instr = 1 (OP_RET) */
    while ((off & 3U) != 0U) buf[off++] = 0;  /* 4-byte alignment */
    off = clh_instr(buf, off, (uint32_t)OP_RET);
    off = clh_varint(buf, off, 1);  /* n_deltas = 1 */
    buf[off++] = 0;                 /* delta = 0 */
    off = clh_varint(buf, off, 0);  /* n_abs_lines */
    off = clh_varint(buf, off, 0);  /* ic_count */
    off = clh_varint(buf, off, has_child ? 1U : 0U);  /* nested_count */
    return off;
}

/* Build a chunk with a linear chain: root → child_1 → … → child_N.
 * chain_depth = number of levels below root (N). buf must be >= 4096 bytes.
 * Returns total serialized size. */
static size_t clh_build_nested_chain(uint8_t *buf, int chain_depth) {
    clh_header(buf);
    size_t off = 24;
    off = clh_varint(buf, off, 0);  /* source_name_len */
    /* root: has a child iff chain_depth > 0 */
    off = clh_emit_proto(buf, off, chain_depth > 0);
    /* nested protos at depth 1 … chain_depth */
    for (int i = 1; i <= chain_depth; i++) {
        off = clh_emit_proto(buf, off, i < chain_depth);
    }
    return off;
}

/* ── Proto tree depth helper ──────────────────────────────────────────────── */

/* Recursively measure the maximum proto depth in a loaded tree.
 * root is at depth 0; each level of nesting increments by 1. */
static int clh_max_depth(const UProto *p, int cur) {
    if (p == NULL) return cur;
    int max = cur;
    size_t i;
    for (i = 0; i < p->nested_count; i++) {
        int d = clh_max_depth(p->nested[i], cur + 1);
        if (d > max) max = d;
    }
    return max;
}

/* ── Test 1: depth-100 chain is rejected ──────────────────────────────────── */

UTEST(loader_rejects_overdeep_nesting)
{
    /* A chain of 100 nested protos must be rejected with UCHUNK_LOAD_CORRUPT
     * rather than silently overflowing the decode stack (B3/REPL-01). */
    uint8_t buf[4096];
    size_t sz = clh_build_nested_chain(buf, 100);
    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, sz, NULL, NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_CORRUPT, (int)rc);
    uchunk_destroy(m, NULL);
}

/* ── Test 2: depth-10 chain is accepted ───────────────────────────────────── */

UTEST(loader_accepts_reasonable_nesting)
{
    /* A chain of 10 nested protos is well within the cap; must load cleanly. */
    uint8_t buf[4096];
    size_t sz = clh_build_nested_chain(buf, 10);
    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, sz, NULL, NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)rc);
    uchunk_destroy(m, NULL);
}

/* ── Test 3: OP_CALL register-window overflow is rejected ────────────────── */

UTEST(verifier_rejects_op_call_window_overflow)
{
    /* Build a chunk with max_reg=5, OP_CALL A=3, B=5.
     * A+B = 8 > max_reg+1 = 6: the runtime would read R[3..7], but only
     * R[0..5] are allocated.  The verifier must reject this (VM-CORE-02). */
    uint8_t buf[512];
    clh_header(buf);
    size_t off = 24;
    off = clh_varint(buf, off, 0);  /* source_name_len */

    /* max_reg=5, nupvals=0, nparams=0 */
    buf[off++] = 5;
    buf[off++] = 0;
    buf[off++] = 0;
    off = clh_varint(buf, off, 0);  /* n_const */
    off = clh_varint(buf, off, 2);  /* n_instr: OP_CALL + OP_RET */
    while ((off & 3U) != 0U) buf[off++] = 0;
    /* OP_CALL A=3, B=5, C=1 (plain call, nresults+1=1) */
    uint32_t call_instr = (uint32_t)OP_CALL
        | ((uint32_t)3U << 8)   /* A: callee at R[3] */
        | ((uint32_t)5U << 16)  /* B: last arg at R[3+5-1]=R[7] > max_reg=5 */
        | ((uint32_t)1U << 24); /* C: nresults+1=1 (valid nresults field) */
    off = clh_instr(buf, off, call_instr);
    off = clh_instr(buf, off, (uint32_t)OP_RET);
    off = clh_varint(buf, off, 2);  /* n_deltas */
    buf[off++] = 0; buf[off++] = 0;
    off = clh_varint(buf, off, 0);  /* n_abs_lines */
    off = clh_varint(buf, off, 0);  /* ic_count */
    off = clh_varint(buf, off, 0);  /* nested_count */

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_CORRUPT, (int)rc);
    uchunk_destroy(m, NULL);
}

/* ── Test 4: stdlib blob depth is within cap ──────────────────────────────── */

UTEST(stdlib_blob_depth_within_cap)
{
    /* Load the full stdlib blob and measure its maximum proto nesting depth.
     * The cap is UCHUNK_MAX_PROTO_DEPTH (64).  The grounding estimate is ~10.
     * This test both confirms the cap does not trip on real code AND records
     * the measured depth for the commit message and report. */
    UASSERT(urbi_stdlib_bytecode_len > 0u);
    UProto *root = NULL;
    char errmsg[256];
    UChunkLoadError rc = uchunk_deserialize(&root, urbi_stdlib_bytecode,
                                             urbi_stdlib_bytecode_len,
                                             NULL, NULL, errmsg, sizeof(errmsg));
    if (rc != UCHUNK_LOAD_OK) {
        printf("  [stdlib load failed: %s]\n", errmsg);
    }
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)rc);
    if (root != NULL) {
        int max_depth = clh_max_depth(root, 0);
        printf("  [stdlib max_proto_depth = %d  (cap = 64)]\n", max_depth);
        UASSERT(max_depth < 60);  /* well below the 64 cap with margin */
        uchunk_destroy(root, NULL);
    }
}

/* ── Suite registration ───────────────────────────────────────────────────── */

void test_chunk_loader_hardening_suite(void) {
    utest_run("loader_rejects_overdeep_nesting",
              loader_rejects_overdeep_nesting);
    utest_run("loader_accepts_reasonable_nesting",
              loader_accepts_reasonable_nesting);
    utest_run("verifier_rejects_op_call_window_overflow",
              verifier_rejects_op_call_window_overflow);
    utest_run("stdlib_blob_depth_within_cap",
              stdlib_blob_depth_within_cap);
}
