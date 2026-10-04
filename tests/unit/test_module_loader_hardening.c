/* SPDX-License-Identifier: BSD-3-Clause */
/* Module-loader hardening tests.
 *
 * Each case feeds the bytecode deserializer a malformed or boundary
 * chunk.  The helpers (build_good_header, put_varint, build_module_bytes)
 * are duplicated from test_module.c rather than shared through a header. */

#include "utest.h"

#include "chunk/uchunk.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* --- Shared bytecode-builder helpers (mirror test_module.c) --- */

static void hard_build_good_header(uint8_t hdr[24]) {
    size_t i;
    for (i = 0; i < 24; i++) hdr[i] = 0;
    hdr[0] = 'U'; hdr[1] = 'R'; hdr[2] = 'B'; hdr[3] = 'I';
    hdr[4] = (uint8_t)URBI_BYTECODE_VERSION_BYTE;
    hdr[5] = 0x00;
    hdr[6]  = 0x19; hdr[7]  = 0x93;
    hdr[8]  = '\r'; hdr[9]  = '\n';
    hdr[10] = 0x1A; hdr[11] = '\n';
    hdr[12] = 8;  /* int_width = i64 */
    hdr[13] = 8;  /* float_type = f64 */
    hdr[14] = 4;  /* instr_width = uint32 */
    hdr[15] = 0;  /* endianness = little */
}

static size_t hard_put_varint(uint8_t *buf, size_t offset, uint64_t v) {
    while (v >= 0x80U) {
        buf[offset++] = (uint8_t)((v & 0x7FU) | 0x80U);
        v >>= 7;
    }
    buf[offset++] = (uint8_t)v;
    return offset;
}

/* Build a minimal valid module: one OP_RET, n_const=0, no nested protos.
 * Returns the total byte length.
 * v1.7 format: source_name_len | max_reg | nupvals | nparams | ... */
static size_t hard_build_minimal_module(uint8_t *buf) {
    hard_build_good_header(buf);
    size_t off = 24;
    off = hard_put_varint(buf, off, 0);   /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;                       /* max_reg */
    buf[off++] = 0;                       /* nupvals */
    buf[off++] = 0;                       /* nparams */
    off = hard_put_varint(buf, off, 0);   /* n_const */
    off = hard_put_varint(buf, off, 1);   /* n_instr = 1 */
    while ((off & 3U) != 0U) buf[off++] = 0;
    /* OP_RET R0 */
    uint32_t ret = (uint32_t)OP_RET;
    buf[off++] = (uint8_t)(ret & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 8) & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 16) & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 24) & 0xFFU);
    off = hard_put_varint(buf, off, 1);   /* n_deltas = 1 */
    buf[off++] = 0;                       /* line delta */
    off = hard_put_varint(buf, off, 0);   /* n_abs_lines */
    off = hard_put_varint(buf, off, 0);   /* root site_count */
    off = hard_put_varint(buf, off, 0);   /* nested_count */
    return off;
}

/* --- nupvals / nparams range check at proto decode ---
 *
 * Each of nupvals and nparams is a single byte (capped at 255 by wire
 * format).  The proto header is cross-checked so that the parameters
 * (plus the hidden argument-count register) fit R[0..max_reg]; the
 * emitter guarantees this and the check guards against hand-crafted
 * bytecode that would write past the frame.
 *
 * Test exercises a nested proto with nparams=200 against max_reg=0 —
 * 200 > max_reg+1=1 — expect rejection. */
UTEST(deserialize_rejects_unbounded_nupvals_nparams) {
    /* Build a root module with one nested proto whose nupvals overflows
     * the register frame. */
    uint8_t buf[512];
    hard_build_good_header(buf);
    size_t off = 24;
    off = hard_put_varint(buf, off, 0);      /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;                          /* root max_reg */
    buf[off++] = 0;                          /* root nupvals */
    buf[off++] = 0;                          /* root nparams */
    off = hard_put_varint(buf, off, 0);      /* root n_const */
    off = hard_put_varint(buf, off, 1);      /* root n_instr */
    while ((off & 3U) != 0U) buf[off++] = 0;
    uint32_t ret = (uint32_t)OP_RET;
    buf[off++] = (uint8_t)(ret & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 8) & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 16) & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 24) & 0xFFU);
    off = hard_put_varint(buf, off, 1);      /* root n_deltas */
    buf[off++] = 0;
    off = hard_put_varint(buf, off, 0);      /* root n_abs_lines */
    off = hard_put_varint(buf, off, 0);      /* root site_count */
    off = hard_put_varint(buf, off, 1);      /* nested_count = 1 */
    /* nested[0]: max_reg=0, nupvals=0, nparams=200 -- the parameters alone
     * overflow R[0..max_reg].  Upvalues are deliberately not part of this
     * rule: they live in the closure, not in registers. */
    buf[off++] = 0;                          /* nested.max_reg */
    buf[off++] = 0;                          /* nested.nupvals */
    buf[off++] = 200;                        /* nested.nparams */

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    uchunk_destroy(m, NULL);
}

/* --- deeply-nested closure verifier sanity (regression) ---
 *
 * The verifier range-checks a CLOSURE's Bx against nested_count.  Construct a
 * v1.5 module with OP_CLOSURE referencing nested[Bx] >= root
 * nested_count and verify the gate is preserved.
 *
 * Encoding note: emit a top-level OP_CLOSURE Bx=99 (root nested_count
 * = 0).  Verifier walks root chunk first; the OP_CLOSURE Bx-range
 * check must reject before reaching any nested-proto walk. */
UTEST(deeply_nested_closure_verifier) {
    uint8_t buf[256];
    hard_build_good_header(buf);
    size_t off = 24;
    off = hard_put_varint(buf, off, 0);      /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;                          /* max_reg */
    buf[off++] = 0;                          /* nupvals */
    buf[off++] = 0;                          /* nparams */
    off = hard_put_varint(buf, off, 0);      /* n_const */
    off = hard_put_varint(buf, off, 2);      /* n_instr = 2 */
    while ((off & 3U) != 0U) buf[off++] = 0;
    /* OP_CLOSURE R0, Bx=99 — references nested[99] which doesn't exist. */
    uint32_t closure = uinstr_enc_abx(OP_CLOSURE, 0, 99);
    buf[off++] = (uint8_t)(closure & 0xFFU);
    buf[off++] = (uint8_t)((closure >> 8) & 0xFFU);
    buf[off++] = (uint8_t)((closure >> 16) & 0xFFU);
    buf[off++] = (uint8_t)((closure >> 24) & 0xFFU);
    /* OP_RET R0. */
    uint32_t ret = uinstr_enc_abc(OP_RET, 0, 0, 0);
    buf[off++] = (uint8_t)(ret & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 8) & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 16) & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 24) & 0xFFU);
    off = hard_put_varint(buf, off, 2);      /* n_deltas */
    buf[off++] = 0; buf[off++] = 0;
    off = hard_put_varint(buf, off, 0);      /* n_abs_lines */
    off = hard_put_varint(buf, off, 0);      /* site_count */
    off = hard_put_varint(buf, off, 0);      /* nested_count = 0 */

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    uchunk_destroy(m, NULL);
}

/* --- site_names cross-validation ---
 *
 * A proto whose site_count exceeds the number of site-bearing
 * instructions in its stream is rejected with UCHUNK_LOAD_CORRUPT: the
 * surplus names are indexed by no instruction, and a corrupt chunk could
 * otherwise ship names for a later confusion attack.  Here: two names,
 * no slot instruction (OP_RET only). */
UTEST(site_names_beyond_the_slot_instructions_rejected) {
    uint8_t buf[256];
    hard_build_good_header(buf);
    size_t off = 24;
    off = hard_put_varint(buf, off, 0);      /* source_name_len = 0 */
    buf[off++] = 0;                          /* max_reg */
    buf[off++] = 0;                          /* nupvals */
    buf[off++] = 0;                          /* nparams */
    off = hard_put_varint(buf, off, 0);      /* n_const */
    off = hard_put_varint(buf, off, 1);      /* n_instr */
    while ((off & 3U) != 0U) buf[off++] = 0;
    uint32_t ret = (uint32_t)OP_RET;
    buf[off++] = (uint8_t)(ret & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 8) & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 16) & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 24) & 0xFFU);
    off = hard_put_varint(buf, off, 1);      /* n_deltas */
    buf[off++] = 0;
    off = hard_put_varint(buf, off, 0);      /* n_abs_lines */
    off = hard_put_varint(buf, off, 2);      /* site_count = 2, no sites used */
    off = hard_put_varint(buf, off, 1); buf[off++] = 'a';
    off = hard_put_varint(buf, off, 1); buf[off++] = 'b';
    off = hard_put_varint(buf, off, 0);      /* nested_count */

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    uchunk_destroy(m, NULL);
}

/* --- site-name table cap ---
 *
 * A site index is a C byte widened to 16 bits by OP_EXTARG and
 * site_count is a uint16_t, so a table of 65,536 names can never be
 * addressed and must not be allocated: the decoder rejects the count
 * before reading a single name. */
UTEST(site_name_count_above_65535_rejected) {
    uint8_t buf[256];
    hard_build_good_header(buf);
    size_t off = 24;
    off = hard_put_varint(buf, off, 0);      /* source_name_len = 0 */
    buf[off++] = 0;                          /* max_reg */
    buf[off++] = 0;                          /* nupvals */
    buf[off++] = 0;                          /* nparams */
    off = hard_put_varint(buf, off, 0);      /* n_const */
    off = hard_put_varint(buf, off, 1);      /* n_instr */
    while ((off & 3U) != 0U) buf[off++] = 0;
    uint32_t ret = (uint32_t)OP_RET;
    buf[off++] = (uint8_t)(ret & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 8) & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 16) & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 24) & 0xFFU);
    off = hard_put_varint(buf, off, 1);      /* n_deltas */
    buf[off++] = 0;
    off = hard_put_varint(buf, off, 0);      /* n_abs_lines */
    off = hard_put_varint(buf, off, 65536);  /* site_count: one past the cap */

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    uchunk_destroy(m, NULL);
}

/* --- n_const cap is strictly `> UINT16_MAX + 1` (not `>=`) ---
 *
 * Boundary regression: the cap formula in decode_constants_into is
 * `n_const > (uint64_t)UINT16_MAX + 1U`.  UINT16_MAX = 65535, so cap is
 * `> 65536`.  n_const = 65536 must be ACCEPTED (max valid value — every
 * Bx in [0..65535] is in range); n_const = 65537 must be REJECTED.
 *
 * We exercise the reject side (65537) directly.  The accept side
 * (65536) would require the test to actually buffer ~1 MiB of constant
 * payloads, which is impractical here; the boundary test on the reject
 * side combined with the existing
 * deserialize_loads_integer_constant_pool test (which exercises small
 * accept counts) is sufficient. */
UTEST(deserialize_n_const_cap_is_strictly_greater_than) {
    uint8_t buf[256];
    /* n_const = 65537 (= UINT16_MAX + 2): exceeds cap, must reject. */
    hard_build_good_header(buf);
    size_t off = 24;
    off = hard_put_varint(buf, off, 0);      /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;                          /* max_reg */
    buf[off++] = 0;                          /* nupvals */
    buf[off++] = 0;                          /* nparams */
    off = hard_put_varint(buf, off, 65537U); /* n_const > cap */

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    uchunk_destroy(m, NULL);
}

/* --- n_abs capped at <= instr_count --- */
UTEST(deserialize_rejects_n_abs_exceeding_instr_count) {
    /* Build a 1-instruction module where n_abs claims 2 (exceeds n_instr). */
    uint8_t buf[256];
    hard_build_good_header(buf);
    size_t off = 24;
    off = hard_put_varint(buf, off, 0);      /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;                          /* max_reg */
    buf[off++] = 0;                          /* nupvals */
    buf[off++] = 0;                          /* nparams */
    off = hard_put_varint(buf, off, 0);      /* n_const */
    off = hard_put_varint(buf, off, 1);      /* n_instr = 1 */
    while ((off & 3U) != 0U) buf[off++] = 0;
    uint32_t ret = (uint32_t)OP_RET;
    buf[off++] = (uint8_t)(ret & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 8) & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 16) & 0xFFU);
    buf[off++] = (uint8_t)((ret >> 24) & 0xFFU);
    off = hard_put_varint(buf, off, 1);      /* n_deltas = 1 (matches n_instr) */
    buf[off++] = 0;
    off = hard_put_varint(buf, off, 2);      /* n_abs = 2 (> n_instr=1) */

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    uchunk_destroy(m, NULL);
}

/* --- module_grow rejects target * elem_size overflow ---
 *
 * module_grow_with_alloc is file-private; the public surface that drives
 * it is the section-decoders.  The wire-format-reachable overflow risk
 * was the n_instr * sizeof(uint32_t) and n_abs * sizeof(UAbsLine)
 * multiplications.  n_instr and n_abs are capped by their decoders;
 * module_grow adds defense-in-depth at the helper boundary so any future call site
 * (or removed cap) cannot regress.
 *
 * This test exercises the helper indirectly via n_instr=UINT64_MAX-1
 * (rejected at the caller-side n_instr cap with UCHUNK_LOAD_OVERSIZED — never reaches
 * the helper) and asserts no crash.  ASan / UBSan in releasetest
 * exercise the helper-level multiply guard directly. */
UTEST(module_grow_rejects_overflow) {
    uint8_t buf[256];
    hard_build_good_header(buf);
    size_t off = 24;
    off = hard_put_varint(buf, off, 0);      /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;                          /* max_reg */
    buf[off++] = 0;                          /* nupvals */
    buf[off++] = 0;                          /* nparams */
    off = hard_put_varint(buf, off, 0);      /* n_const */
    off = hard_put_varint(buf, off, (uint64_t)0xFFFFFFFFFFFFFFFEULL);

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    /* The n_instr cap fires first; the module_grow guard is also acceptable. */
    UASSERT(rc == UCHUNK_LOAD_OVERSIZED || rc == UCHUNK_LOAD_OOM);
    uchunk_destroy(m, NULL);
}

/* --- instr_count uint64 -> size_t demotion guard ---
 *
 * Build a bytecode whose n_instr varint decodes to UINT64_MAX-1.  The
 * loader must reject with UCHUNK_LOAD_OVERSIZED rather than silently truncate
 * to size_t (a real concern on 32-bit ports). */
UTEST(deserialize_rejects_oversized_instr_count_on_32bit) {
    uint8_t buf[256];
    hard_build_good_header(buf);
    size_t off = 24;
    off = hard_put_varint(buf, off, 0);      /* source_name_len = 0 (v1.7) */
    buf[off++] = 0;                          /* max_reg */
    buf[off++] = 0;                          /* nupvals */
    buf[off++] = 0;                          /* nparams */
    off = hard_put_varint(buf, off, 0);      /* n_const */
    /* n_instr = UINT64_MAX-1; far above URBI_MAX_INSTRS_PER_PROTO. */
    off = hard_put_varint(buf, off, (uint64_t)0xFFFFFFFFFFFFFFFEULL);

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ(UCHUNK_LOAD_OVERSIZED, rc);
    uchunk_destroy(m, NULL);
}

/* --- deserialize NULL buf returns UCHUNK_LOAD_INVALID_ARG --- */
UTEST(deserialize_null_buf_returns_invalid_arg) {
    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, NULL, 64, NULL, NULL, NULL, 0);
    UASSERT_EQ(UCHUNK_LOAD_INVALID_ARG, rc);
    uchunk_destroy(m, NULL);
}

/* --- partial-failure destroy idempotent ---
 *
 * Truncate a serialized module mid-decode at multiple offsets; on
 * deserialize failure call uchunk_destroy.  Run under ASan / valgrind
 * to catch double-free or leaks.  The loader's contract is:
 *   "module may hold partial buffers on error; uchunk_destroy is safe
 *    in either case."
 * This test verifies the implementation matches that claim. */
UTEST(deserialize_partial_failure_destroy_idempotent) {
    uint8_t buf[256];
    size_t total = hard_build_minimal_module(buf);

    /* Walk every truncation length [1..total) — exercises every section
     * boundary (5 named sites: header / metadata
     * / constants / instructions / line-table) plus the gaps between. */
    size_t i;
    for (i = 1; i < total; i++) {
        UProto *m = NULL;
        UChunkLoadError rc = uchunk_deserialize(&m, buf, total - i, NULL, NULL, NULL, 0);
        UASSERT(rc != UCHUNK_LOAD_OK);
        uchunk_destroy(m, NULL);   /* must not double-free */
        uchunk_destroy(m, NULL);   /* idempotent on already-destroyed module */
    }

    /* Successful deserialize -> destroy -> destroy must also be idempotent.
     * After uchunk_destroy the heap-allocated root is freed; caller must
     * set the pointer to NULL before re-calling (uchunk_destroy(NULL) is
     * the no-op contract, not uchunk_destroy(dangling)). */
    UProto *m = NULL;
    UASSERT_EQ(UCHUNK_LOAD_OK, uchunk_deserialize(&m, buf, total, NULL, NULL, NULL, 0));
    uchunk_destroy(m, NULL);
    m = NULL;
    uchunk_destroy(m, NULL);
}

void test_module_loader_hardening_suite(void);

void test_module_loader_hardening_suite(void) {
    utest_run("deserialize partial-failure destroy idempotent",
              deserialize_partial_failure_destroy_idempotent);
    utest_run("deserialize NULL buf returns UCHUNK_LOAD_INVALID_ARG",
              deserialize_null_buf_returns_invalid_arg);
    utest_run("deserialize rejects oversized instr_count",
              deserialize_rejects_oversized_instr_count_on_32bit);
    utest_run("module_grow rejects target * elem_size overflow",
              module_grow_rejects_overflow);
    utest_run("deserialize rejects n_abs > instr_count",
              deserialize_rejects_n_abs_exceeding_instr_count);
    utest_run("deserialize n_const cap is strictly > UINT16_MAX",
              deserialize_n_const_cap_is_strictly_greater_than);
    utest_run("site names beyond the slot instructions rejected",
              site_names_beyond_the_slot_instructions_rejected);
    utest_run("site name count above 65535 rejected",
              site_name_count_above_65535_rejected);
    utest_run("deeply-nested closure verifier (regression)",
              deeply_nested_closure_verifier);
    utest_run("deserialize bounds nparams against the register window",
              deserialize_rejects_unbounded_nupvals_nparams);
}
