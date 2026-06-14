/* SPDX-License-Identifier: BSD-3-Clause */
/* Verifier cross-byte tests for refactor-3 T10 rules.
 *
 * Covers:
 *   VM-13: OP_TAG_STOP round-trip — verifier must accept it (has full VM
 *          dispatch since v0.10.2; no compiler path produces it, but the
 *          loader must not reject it as reserved).
 *   VM-14: OP_JOIN_WAIT adjacency — must be immediately preceded by
 *          OP_FORK_JOIN with matching B (child handle) == JOIN_WAIT.A.
 *   VM-19: OP_SELF cross-byte — A writes R[A] and R[A+1]; A+1 must not
 *          exceed max_reg.
 *
 * Helper pattern mirrors test_module_loader_hardening.c to avoid a
 * shared-header dependency (touch-only-what-you-must). */

#include "utest.h"
#include "chunk/uchunk.h"

#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ── bytecode builder helpers ────────────────────────────────────────── */

static void xb_header(uint8_t hdr[24]) {
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

static size_t xb_varint(uint8_t *buf, size_t off, uint64_t v) {
    while (v >= 0x80U) {
        buf[off++] = (uint8_t)((v & 0x7FU) | 0x80U);
        v >>= 7;
    }
    buf[off++] = (uint8_t)v;
    return off;
}

static void xb_put32(uint8_t *buf, size_t off, uint32_t w) {
    buf[off + 0] = (uint8_t)(w & 0xFFU);
    buf[off + 1] = (uint8_t)((w >> 8) & 0xFFU);
    buf[off + 2] = (uint8_t)((w >> 16) & 0xFFU);
    buf[off + 3] = (uint8_t)((w >> 24) & 0xFFU);
}

/* Build a minimal valid module: header + source_name_len=0 + one proto
 * block (max_reg, nupvals=0, nparams=0, n_const=0, n_instr=n, then the
 * instructions, then n line-deltas = 0, n_abs_lines=0, ic_count=0,
 * nested_count=0).  buf must be >= 512 bytes.  Returns total length. */
/* IC-bearing opcodes (mirrors op_carries_ic_index in uchunk_io.c).
 * Must stay in sync if new IC opcodes are added. */
static int xb_is_ic_opcode(uint8_t op) {
    return op == 27U  /* OP_GETSLOT */
        || op == 28U  /* OP_SETSLOT */
        || op == 44U  /* OP_GETSLOT_CHANGE_EVENT */
        || op == 47U; /* OP_SELF */
}

static size_t xb_build(uint8_t *buf, uint8_t max_reg,
                        const uint32_t *instrs, size_t n) {
    size_t i;
    /* Count IC-bearing opcodes so ic_count matches ic_seen check in verifier.
     * OP_SELF (and others) carry an ic_idx in C that must be < ic_count. */
    size_t ic_count = 0;
    for (i = 0; i < n; i++) {
        if (xb_is_ic_opcode((uint8_t)(instrs[i] & 0xFFU))) {
            ic_count++;
        }
    }
    xb_header(buf);
    size_t off = 24;
    off = xb_varint(buf, off, 0);           /* source_name_len = 0 */
    buf[off++] = max_reg;
    buf[off++] = 0;                          /* nupvals = 0 */
    buf[off++] = 0;                          /* nparams = 0 */
    off = xb_varint(buf, off, 0);           /* n_const = 0 */
    off = xb_varint(buf, off, (uint64_t)n); /* n_instr */
    while ((off & 3U) != 0U) buf[off++] = 0; /* align to 4 */
    for (i = 0; i < n; i++) {
        xb_put32(buf, off, instrs[i]);
        off += 4;
    }
    off = xb_varint(buf, off, (uint64_t)n); /* n_deltas = n */
    for (i = 0; i < n; i++) buf[off++] = 0; /* all zero deltas */
    off = xb_varint(buf, off, 0);           /* n_abs_lines = 0 */
    /* ic_count: write count, then one empty-string entry per IC site. */
    off = xb_varint(buf, off, (uint64_t)ic_count);
    for (i = 0; i < ic_count; i++) {
        off = xb_varint(buf, off, 0);       /* ic_name[i] length = 0 */
    }
    off = xb_varint(buf, off, 0);           /* nested_count = 0 */
    return off;
}

/* ── VM-13: OP_TAG_STOP round-trip ──────────────────────────────────── */

/* OP_TAG_STOP (opcode 30) has a full VM dispatch since v0.10.2 (label_op_tag_stop
 * in uvm.c).  No compiler path produces it — scripted tag.stop() routes
 * through the C native tag_stop_native → urbi_tag_stop API rather than
 * emitting OP_TAG_STOP bytecode.  The loader must nonetheless accept it:
 * the "reserved" reject in verify_bounds_proto was stale (wire v1.8 note
 * that predated the v0.10.2 dispatch wiring).  Pinned by this test. */
UTEST(verifier_roundtrips_op_tag_stop)
{
    uint8_t buf[512];
    /* OP_TAG_STOP ABC: A=tag_reg=0, B=value_reg=0, C=0. */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_TAG_STOP, 0, 0, 0),
        uinstr_enc_abc(OP_RET,      0, 0, 0)
    };
    size_t total = xb_build(buf, /*max_reg=*/1, instrs, 2);
    UProto *p = NULL;
    char errmsg[256];
    UChunkLoadError rc = uchunk_deserialize(&p, buf, total,
                                            NULL, NULL, errmsg, sizeof errmsg);
    /* VM-13 pin: OP_TAG_STOP must round-trip cleanly. */
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    uchunk_destroy(p, NULL);
}

/* ── VM-14: OP_JOIN_WAIT adjacency ──────────────────────────────────── */

/* JOIN_WAIT at pc 0 has no predecessor — must reject. */
UTEST(verifier_rejects_join_wait_at_pc0)
{
    uint8_t buf[512];
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_JOIN_WAIT, 0, 0, 0), /* pc=0, no predecessor */
        uinstr_enc_abc(OP_RET,       0, 0, 0)
    };
    size_t total = xb_build(buf, /*max_reg=*/1, instrs, 2);
    UProto *p = NULL;
    char errmsg[256];
    UChunkLoadError rc = uchunk_deserialize(&p, buf, total,
                                            NULL, NULL, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    uchunk_destroy(p, NULL);
}

/* JOIN_WAIT with a non-fork predecessor must reject. */
UTEST(verifier_rejects_join_wait_with_non_fork_predecessor)
{
    uint8_t buf[512];
    /* OP_MOVE at pc=0 (not a fork), OP_JOIN_WAIT at pc=1. */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_MOVE,     1, 0, 0),
        uinstr_enc_abc(OP_JOIN_WAIT, 1, 0, 0),
        uinstr_enc_abc(OP_RET,      0, 0, 0)
    };
    size_t total = xb_build(buf, /*max_reg=*/1, instrs, 3);
    UProto *p = NULL;
    char errmsg[256];
    UChunkLoadError rc = uchunk_deserialize(&p, buf, total,
                                            NULL, NULL, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    uchunk_destroy(p, NULL);
}

/* JOIN_WAIT where FORK_JOIN.B (child handle reg) != JOIN_WAIT.A must reject.
 * FORK_JOIN writes the child handle into R[B]; JOIN_WAIT reads it from R[A].
 * A mismatch means JOIN_WAIT would read a different (potentially stale/freed)
 * register — the UAF the adjacency rule guards against. */
UTEST(verifier_rejects_join_wait_with_mismatched_child_reg)
{
    uint8_t buf[512];
    /* FORK_JOIN: closure in R[0], child handle written to R[1].
     * JOIN_WAIT: reads R[2] — mismatch with FORK_JOIN.B=1. */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_FORK_JOIN, 0, 1, 0), /* B=1: handle → R[1] */
        uinstr_enc_abc(OP_JOIN_WAIT, 2, 0, 0), /* A=2: reads R[2]: wrong */
        uinstr_enc_abc(OP_RET,       0, 0, 0)
    };
    size_t total = xb_build(buf, /*max_reg=*/2, instrs, 3);
    UProto *p = NULL;
    char errmsg[256];
    UChunkLoadError rc = uchunk_deserialize(&p, buf, total,
                                            NULL, NULL, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    uchunk_destroy(p, NULL);
}

/* Valid FORK_JOIN immediately before JOIN_WAIT with FORK_JOIN.B == JOIN_WAIT.A
 * must accept. */
UTEST(verifier_accepts_fork_join_then_join_wait)
{
    uint8_t buf[512];
    /* FORK_JOIN: closure in R[0], child handle in R[1].
     * JOIN_WAIT:  reads R[1] — matches FORK_JOIN.B. */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_FORK_JOIN, 0, 1, 0), /* B=1: handle → R[1] */
        uinstr_enc_abc(OP_JOIN_WAIT, 1, 0, 0), /* A=1: reads R[1]: correct */
        uinstr_enc_abc(OP_RET,       0, 0, 0)
    };
    size_t total = xb_build(buf, /*max_reg=*/1, instrs, 3);
    UProto *p = NULL;
    char errmsg[256];
    UChunkLoadError rc = uchunk_deserialize(&p, buf, total,
                                            NULL, NULL, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    uchunk_destroy(p, NULL);
}

/* ── VM-14 follow-up: bare OP_FORK_JOIN B-operand bounds ─────────────────
 *
 * OP_FORK_JOIN's dispatch WRITES R[B] (the child strand handle), but B was
 * marked UOPK_UNUSED in the shape table, so verify_byte_operand never
 * bounds-checked it.  The VM-14 adjacency rule transitively pins B for the
 * normal emit pattern (FORK_JOIN followed by JOIN_WAIT, B == JOIN_WAIT.A <=
 * max_reg) — but a BARE OP_FORK_JOIN (no following JOIN_WAIT) with B >
 * max_reg slips through and dispatch writes a register OUTSIDE the declared
 * window: an OOB write from an untrusted chunk.  Closed by marking
 * OP_FORK_JOIN.B as UOPK_REG. */

/* Bare OP_FORK_JOIN (no following JOIN_WAIT) with B > max_reg must reject. */
UTEST(verifier_rejects_bare_fork_join_b_oob)
{
    uint8_t buf[512];
    /* max_reg=0: only R[0] exists.  FORK_JOIN A=0 (closure) B=5 (handle dst).
     * R[5] is outside the window — dispatch would OOB-write.  No JOIN_WAIT
     * follows, so the VM-14 adjacency rule does not catch it. */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_FORK_JOIN, 0, 5, 0),
        uinstr_enc_abc(OP_RET,       0, 0, 0)
    };
    size_t total = xb_build(buf, /*max_reg=*/0, instrs, 2);
    UProto *p = NULL;
    char errmsg[256];
    UChunkLoadError rc = uchunk_deserialize(&p, buf, total,
                                            NULL, NULL, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    uchunk_destroy(p, NULL);
}

/* Bare OP_FORK_JOIN with B <= max_reg (in-window) must still accept — the
 * B-operand check must not over-reject valid bare forks. */
UTEST(verifier_accepts_bare_fork_join_b_in_window)
{
    uint8_t buf[512];
    /* max_reg=1: R[0], R[1] exist.  FORK_JOIN A=0 B=1: handle → R[1], in window. */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_FORK_JOIN, 0, 1, 0),
        uinstr_enc_abc(OP_RET,       0, 0, 0)
    };
    size_t total = xb_build(buf, /*max_reg=*/1, instrs, 2);
    UProto *p = NULL;
    char errmsg[256];
    UChunkLoadError rc = uchunk_deserialize(&p, buf, total,
                                            NULL, NULL, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    uchunk_destroy(p, NULL);
}

/* ── VM-19: OP_SELF cross-byte ───────────────────────────────────────── */

/* OP_SELF writes R[A] (the looked-up slot value) and R[A+1] (self / receiver
 * copy so OP_CALL can read it without the deprecated vm->last_recv channel).
 * The shape table verifies A <= max_reg individually; the cross-byte check
 * also requires A+1 <= max_reg.  Reject when A == max_reg. */
UTEST(verifier_rejects_op_self_a_eq_max_reg)
{
    uint8_t buf[512];
    /* max_reg=1: R[0] and R[1] exist.
     * OP_SELF A=1, B=0: writes R[1] and R[2]; R[2] > max_reg → reject. */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_SELF, 1, 0, 0),
        uinstr_enc_abc(OP_RET,  0, 0, 0)
    };
    size_t total = xb_build(buf, /*max_reg=*/1, instrs, 2);
    UProto *p = NULL;
    char errmsg[256];
    UChunkLoadError rc = uchunk_deserialize(&p, buf, total,
                                            NULL, NULL, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, rc);
    uchunk_destroy(p, NULL);
}

/* OP_SELF with A == max_reg - 1: A+1 == max_reg is in range → accept. */
UTEST(verifier_accepts_op_self_a_lt_max_reg)
{
    uint8_t buf[512];
    /* max_reg=2: R[0], R[1], R[2] exist.
     * OP_SELF A=1, B=0: writes R[1] and R[2]; both <= max_reg=2 → accept. */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_SELF, 1, 0, 0),
        uinstr_enc_abc(OP_RET,  0, 0, 0)
    };
    size_t total = xb_build(buf, /*max_reg=*/2, instrs, 2);
    UProto *p = NULL;
    char errmsg[256];
    UChunkLoadError rc = uchunk_deserialize(&p, buf, total,
                                            NULL, NULL, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    uchunk_destroy(p, NULL);
}

/* ── Suite entry point ───────────────────────────────────────────────── */

void
test_verifier_cross_byte_suite(void)
{
    printf("test_verifier_cross_byte\n");
    utest_run("VM-13: verifier roundtrips OP_TAG_STOP",
              verifier_roundtrips_op_tag_stop);
    utest_run("VM-14: verifier rejects JOIN_WAIT at pc 0",
              verifier_rejects_join_wait_at_pc0);
    utest_run("VM-14: verifier rejects JOIN_WAIT with non-fork predecessor",
              verifier_rejects_join_wait_with_non_fork_predecessor);
    utest_run("VM-14: verifier rejects JOIN_WAIT with mismatched child reg",
              verifier_rejects_join_wait_with_mismatched_child_reg);
    utest_run("VM-14: verifier accepts FORK_JOIN then JOIN_WAIT",
              verifier_accepts_fork_join_then_join_wait);
    utest_run("VM-14: verifier rejects bare FORK_JOIN with B > max_reg",
              verifier_rejects_bare_fork_join_b_oob);
    utest_run("VM-14: verifier accepts bare FORK_JOIN with B in window",
              verifier_accepts_bare_fork_join_b_in_window);
    utest_run("VM-19: verifier rejects OP_SELF A==max_reg",
              verifier_rejects_op_self_a_eq_max_reg);
    utest_run("VM-19: verifier accepts OP_SELF A==max_reg-1",
              verifier_accepts_op_self_a_lt_max_reg);
}
