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
}
