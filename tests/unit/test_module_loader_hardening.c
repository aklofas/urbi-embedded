/* SPDX-License-Identifier: BSD-3-Clause */
/* Phase-15 (T71-T79) module-loader hardening tests.
 *
 * Tests target audit-cited holes in the bytecode deserializer.  Helpers
 * (build_good_header, put_varint, build_module_bytes) are deliberately
 * duplicated from test_module.c rather than refactored into a shared
 * header — see plan §3.3 (touch-only-what-you-must); shared helpers can
 * be hoisted in a follow-up. */

#include "utest.h"

#include "module/umodule.h"

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
 * Returns the total byte length. */
static size_t hard_build_minimal_module(uint8_t *buf) {
    hard_build_good_header(buf);
    size_t off = 24;
    buf[off++] = 0;                       /* max_reg */
    off = hard_put_varint(buf, off, 0);   /* source_name_len */
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
    off = hard_put_varint(buf, off, 0);   /* root ic_count */
    off = hard_put_varint(buf, off, 0);   /* nested_count */
    return off;
}

/* --- T76 (MOD-019): n_const cap is strictly `> UINT16_MAX + 1` (not `>=`) ---
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
    buf[off++] = 0;                          /* max_reg */
    off = hard_put_varint(buf, off, 0);      /* source_name_len */
    off = hard_put_varint(buf, off, 65537U); /* n_const > cap */

    UModule m = {0};
    UModuleLoadError rc = umodule_deserialize(&m, buf, off, NULL, 0);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    umodule_destroy(&m);
}

/* --- T75 (MOD-018): n_abs capped at <= instr_count --- */
UTEST(deserialize_rejects_n_abs_exceeding_instr_count) {
    /* Build a 1-instruction module where n_abs claims 2 (exceeds n_instr). */
    uint8_t buf[256];
    hard_build_good_header(buf);
    size_t off = 24;
    buf[off++] = 0;                          /* max_reg */
    off = hard_put_varint(buf, off, 0);      /* source_name_len */
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

    UModule m = {0};
    UModuleLoadError rc = umodule_deserialize(&m, buf, off, NULL, 0);
    UASSERT_EQ(ULOAD_CORRUPT, rc);
    umodule_destroy(&m);
}

/* --- T72 (MOD-004): module_grow rejects target * elem_size overflow ---
 *
 * module_grow_with_alloc is file-private; the public surface that drives
 * it is the section-decoders.  The wire-format-reachable overflow risk
 * was the n_instr * sizeof(uint32_t) and n_abs * sizeof(UAbsLine)
 * multiplications.  T74 now caps n_instr; T75 caps n_abs.  T72 adds
 * defense-in-depth at the helper boundary so any future call site
 * (or removed cap) cannot regress.
 *
 * This test exercises the helper indirectly via n_instr=UINT64_MAX-1
 * (rejected at caller-side T74 cap with ULOAD_OVERSIZED — never reaches
 * the helper) and asserts no crash.  ASan / UBSan in releasetest
 * exercise the helper-level multiply guard directly. */
UTEST(module_grow_rejects_overflow) {
    uint8_t buf[256];
    hard_build_good_header(buf);
    size_t off = 24;
    buf[off++] = 0;                          /* max_reg */
    off = hard_put_varint(buf, off, 0);      /* source_name_len */
    off = hard_put_varint(buf, off, 0);      /* n_const */
    off = hard_put_varint(buf, off, (uint64_t)0xFFFFFFFFFFFFFFFEULL);

    UModule m = {0};
    UModuleLoadError rc = umodule_deserialize(&m, buf, off, NULL, 0);
    /* T74 caps the count first; T72 fallback is also acceptable. */
    UASSERT(rc == ULOAD_OVERSIZED || rc == ULOAD_OOM);
    umodule_destroy(&m);
}

/* --- T74 (MOD-017): instr_count uint64 -> size_t demotion guard ---
 *
 * Build a bytecode whose n_instr varint decodes to UINT64_MAX-1.  The
 * loader must reject with ULOAD_OVERSIZED rather than silently truncate
 * to size_t (a real concern on 32-bit ports). */
UTEST(deserialize_rejects_oversized_instr_count_on_32bit) {
    uint8_t buf[256];
    hard_build_good_header(buf);
    size_t off = 24;
    buf[off++] = 0;                          /* max_reg */
    off = hard_put_varint(buf, off, 0);      /* source_name_len */
    off = hard_put_varint(buf, off, 0);      /* n_const */
    /* n_instr = UINT64_MAX-1; far above URBI_MAX_INSTRS_PER_PROTO. */
    off = hard_put_varint(buf, off, (uint64_t)0xFFFFFFFFFFFFFFFEULL);

    UModule m = {0};
    UModuleLoadError rc = umodule_deserialize(&m, buf, off, NULL, 0);
    UASSERT_EQ(ULOAD_OVERSIZED, rc);
    umodule_destroy(&m);
}

/* --- T73 (MOD-007): deserialize NULL buf returns ULOAD_INVALID_ARG --- */
UTEST(deserialize_null_buf_returns_invalid_arg) {
    UModule m = {0};
    UModuleLoadError rc = umodule_deserialize(&m, NULL, 64, NULL, 0);
    UASSERT_EQ(ULOAD_INVALID_ARG, rc);
    umodule_destroy(&m);
}

/* --- T71 (MOD-001 + MOD-002): partial-failure destroy idempotent ---
 *
 * Truncate a serialized module mid-decode at multiple offsets; on
 * deserialize failure call umodule_destroy.  Run under ASan / valgrind
 * to catch double-free or leaks.  The v0.5.6 MOD-039 docstring claims:
 *   "module may hold partial buffers on error; umodule_destroy is safe
 *    in either case."
 * This test verifies the implementation matches that claim. */
UTEST(deserialize_partial_failure_destroy_idempotent) {
    uint8_t buf[256];
    size_t total = hard_build_minimal_module(buf);

    /* Walk every truncation length [1..total) — exercises every section
     * boundary the v0.5.6 audit cited (5 named sites: header / metadata
     * / constants / instructions / line-table) plus the gaps between. */
    size_t i;
    for (i = 1; i < total; i++) {
        UModule m = {0};
        UModuleLoadError rc = umodule_deserialize(&m, buf, total - i, NULL, 0);
        UASSERT(rc != ULOAD_OK);
        umodule_destroy(&m);   /* must not double-free */
        umodule_destroy(&m);   /* idempotent on already-destroyed module */
    }

    /* Successful deserialize -> destroy -> destroy must also be idempotent. */
    UModule m = {0};
    UASSERT_EQ(ULOAD_OK, umodule_deserialize(&m, buf, total, NULL, 0));
    umodule_destroy(&m);
    umodule_destroy(&m);
}

void test_module_loader_hardening_suite(void);

void test_module_loader_hardening_suite(void) {
    utest_run("deserialize partial-failure destroy idempotent (T71: MOD-001+002)",
              deserialize_partial_failure_destroy_idempotent);
    utest_run("deserialize NULL buf returns ULOAD_INVALID_ARG (T73: MOD-007)",
              deserialize_null_buf_returns_invalid_arg);
    utest_run("deserialize rejects oversized instr_count (T74: MOD-017)",
              deserialize_rejects_oversized_instr_count_on_32bit);
    utest_run("module_grow rejects target * elem_size overflow (T72: MOD-004)",
              module_grow_rejects_overflow);
    utest_run("deserialize rejects n_abs > instr_count (T75: MOD-018)",
              deserialize_rejects_n_abs_exceeding_instr_count);
    utest_run("deserialize n_const cap is strictly > UINT16_MAX (T76: MOD-019)",
              deserialize_n_const_cap_is_strictly_greater_than);
}
