/* SPDX-License-Identifier: BSD-3-Clause */
/* Verifier cross-byte tests: the rules a shape-table row cannot express.
 *
 * Covers:
 *   VM-14: OP_JOIN_WAIT adjacency — must be immediately preceded by a
 *          joining OP_FORK with matching B (child handle) == JOIN_WAIT.A.
 *   VM-19: OP_SELF cross-byte — A writes R[A] and R[A+1]; A+1 must not
 *          exceed max_reg.
 *   Wire v2: FORK mode vs handle register, SCOPE_POP kinds, INSTALL
 *          flags, CALL register window, scope / unwind target range.
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
 * instructions, then n line-deltas = 0, n_abs_lines=0, one empty site
 * name per site-taking instruction, nested_count=0).  buf must be >= 512
 * bytes.  Returns total length. */
/* Site-taking opcodes (mirrors op_takes_site in uchunk_verify.c). */
static int xb_is_site_opcode(uint8_t op) {
    return op == (uint8_t)OP_GETSLOT
        || op == (uint8_t)OP_SETSLOT
        || op == (uint8_t)OP_SETSLOT_UPDATE
        || op == (uint8_t)OP_GETSLOT_CHANGE_EVENT
        || op == (uint8_t)OP_SELF;
}

static size_t xb_build(uint8_t *buf, uint8_t max_reg,
                        const uint32_t *instrs, size_t n) {
    size_t i;
    /* One site per site-taking instruction, so a C operand of 0 is in
     * range wherever one appears. */
    size_t site_count = 0;
    for (i = 0; i < n; i++) {
        if (xb_is_site_opcode((uint8_t)(instrs[i] & 0xFFU))) {
            site_count++;
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
    /* site_count: write count, then one empty-string entry per site. */
    off = xb_varint(buf, off, (uint64_t)site_count);
    for (i = 0; i < site_count; i++) {
        off = xb_varint(buf, off, 0);       /* site_name[i] length = 0 */
    }
    off = xb_varint(buf, off, 0);           /* nested_count = 0 */
    return off;
}

/* Build and load; returns the loader's verdict. */
static UChunkLoadError xb_load(uint8_t max_reg, const uint32_t *instrs, size_t n)
{
    uint8_t buf[512];
    size_t total = xb_build(buf, max_reg, instrs, n);
    UProto *p = NULL;
    char errmsg[256];
    UChunkLoadError rc = uchunk_deserialize(&p, buf, total,
                                            NULL, NULL, errmsg, sizeof errmsg);
    uchunk_destroy(p, NULL);
    return rc;
}

/* ── wire v2 per-opcode rules ────────────────────────────────────────── */

/* A detached FORK has no handle (B must be 0xFF); a joining one must
 * name a register. */
UTEST(verifier_fork_mode_matches_handle_register)
{
    const uint32_t detach_ok[]  = { uinstr_enc_abc(OP_FORK, 0, 0xFF, UFORK_DETACH), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    const uint32_t detach_bad[] = { uinstr_enc_abc(OP_FORK, 0, 1, UFORK_DETACH),    uinstr_enc_abc(OP_RET, 0, 0, 0) };
    const uint32_t join_bad[]   = { uinstr_enc_abc(OP_FORK, 0, 0xFF, UFORK_JOIN),   uinstr_enc_abc(OP_RET, 0, 0, 0) };
    const uint32_t mode_bad[]   = { uinstr_enc_abc(OP_FORK, 0, 0xFF, 2),            uinstr_enc_abc(OP_RET, 0, 0, 0) };
    UASSERT_EQ(UCHUNK_LOAD_OK,      xb_load(1, detach_ok, 2));
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, xb_load(1, detach_bad, 2));
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, xb_load(1, join_bad, 2));
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, xb_load(1, mode_bad, 2));
}

/* SCOPE_POP's low two bits must name exactly TRY or TAG, and the
 * run-finally bit is for a TRY pop only. */
UTEST(verifier_scope_pop_kind_rules)
{
    const uint32_t try_ok[]  = { uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TRY, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    const uint32_t tryf_ok[] = { uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TRY | USCOPE_POP_RUN_FINALLY, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    const uint32_t tag_ok[]  = { uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TAG, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    const uint32_t none[]    = { uinstr_enc_abc(OP_SCOPE_POP, 0, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    const uint32_t both[]    = { uinstr_enc_abc(OP_SCOPE_POP, 0x3, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    const uint32_t tagf[]    = { uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TAG | USCOPE_POP_RUN_FINALLY, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    UASSERT_EQ(UCHUNK_LOAD_OK,      xb_load(0, try_ok, 2));
    UASSERT_EQ(UCHUNK_LOAD_OK,      xb_load(0, tryf_ok, 2));
    UASSERT_EQ(UCHUNK_LOAD_OK,      xb_load(0, tag_ok, 2));
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, xb_load(0, none, 2));
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, xb_load(0, both, 2));
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, xb_load(0, tagf, 2));
}

/* INSTALL flags are a subset of HAS_BODY | HAS_ALT, and waituntil takes
 * neither; mode 0 is not a mode. */
UTEST(verifier_install_flag_rules)
{
    const uint32_t alt_ok[]   = { uinstr_enc_abc(OP_INSTALL, 0, UINSTALL_WHENEVER_EVENT, UINSTALL_F_HAS_BODY | UINSTALL_F_HAS_ALT), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    const uint32_t wait_ok[]  = { uinstr_enc_abc(OP_INSTALL, 0, UINSTALL_WAITUNTIL, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    const uint32_t wait_bad[] = { uinstr_enc_abc(OP_INSTALL, 0, UINSTALL_WAITUNTIL, UINSTALL_F_HAS_BODY), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    const uint32_t bit_bad[]  = { uinstr_enc_abc(OP_INSTALL, 0, UINSTALL_AT_COND, 0x4), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    const uint32_t mode0[]    = { uinstr_enc_abc(OP_INSTALL, 0, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    UASSERT_EQ(UCHUNK_LOAD_OK,      xb_load(2, alt_ok, 2));
    UASSERT_EQ(UCHUNK_LOAD_OK,      xb_load(2, wait_ok, 2));
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, xb_load(2, wait_bad, 2));
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, xb_load(2, bit_bad, 2));
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, xb_load(2, mode0, 2));
}

/* CALL's argument window A..A+B-1 stays inside the frame. */
UTEST(verifier_call_register_window)
{
    const uint32_t ok[]  = { uinstr_enc_abc(OP_CALL, 1, 2, 2), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    const uint32_t bad[] = { uinstr_enc_abc(OP_CALL, 1, 3, 2), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    UASSERT_EQ(UCHUNK_LOAD_OK,      xb_load(2, ok, 2));
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, xb_load(2, bad, 2));
}

/* SCOPE_TRY, SCOPE_TAG and UNWIND_TO name an instruction of the proto. */
UTEST(verifier_scope_and_unwind_targets_in_range)
{
    const uint32_t ok[]      = { uinstr_enc_abx(OP_UNWIND_TO, 0, 1), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    const uint32_t unwind[]  = { uinstr_enc_abx(OP_UNWIND_TO, 0, 2), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    const uint32_t try_oob[] = { uinstr_enc_abx(OP_SCOPE_TRY, USCOPE_F_HAS_CATCH, 2), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    const uint32_t tag_oob[] = { uinstr_enc_abx(OP_SCOPE_TAG, USCOPE_NO_REG, 2), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    UASSERT_EQ(UCHUNK_LOAD_OK,      xb_load(0, ok, 2));
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, xb_load(0, unwind, 2));
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, xb_load(0, try_oob, 2));
    UASSERT_EQ(UCHUNK_LOAD_CORRUPT, xb_load(0, tag_oob, 2));
}

/* An UNWIND_TO target right after an EXTARG is rejected like a jump's. */
UTEST(verifier_unwind_target_after_extarg_rejected)
{
    const uint32_t instrs[] = {
        uinstr_enc_abx(OP_UNWIND_TO, 0, 2),
        uinstr_enc_abx(OP_EXTARG, 0, 0),
        uinstr_enc_abc(OP_GETSLOT, 0, 0, 0),
        uinstr_enc_abc(OP_RET, 0, 0, 0)
    };
    UASSERT_EQ(UCHUNK_LOAD_BAD_EXTARG, xb_load(0, instrs, 4));
}

/* Two EXTARGs in a row: the first precedes an opcode that takes no site. */
UTEST(verifier_double_extarg_rejected)
{
    const uint32_t instrs[] = {
        uinstr_enc_abx(OP_EXTARG, 0, 0),
        uinstr_enc_abx(OP_EXTARG, 0, 0),
        uinstr_enc_abc(OP_GETSLOT, 0, 0, 0),
        uinstr_enc_abc(OP_RET, 0, 0, 0)
    };
    UASSERT_EQ(UCHUNK_LOAD_BAD_EXTARG, xb_load(0, instrs, 4));
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

/* JOIN_WAIT where joining FORK.B (child handle reg) != JOIN_WAIT.A must reject.
 * joining FORK writes the child handle into R[B]; JOIN_WAIT reads it from R[A].
 * A mismatch means JOIN_WAIT would read a different (potentially stale/freed)
 * register — the UAF the adjacency rule guards against. */
UTEST(verifier_rejects_join_wait_with_mismatched_child_reg)
{
    uint8_t buf[512];
    /* joining FORK: closure in R[0], child handle written to R[1].
     * JOIN_WAIT: reads R[2] — mismatch with joining FORK.B=1. */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_FORK, 0, 1, UFORK_JOIN), /* B=1: handle → R[1] */
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

/* Valid joining FORK immediately before JOIN_WAIT with joining FORK.B == JOIN_WAIT.A
 * must accept. */
UTEST(verifier_accepts_fork_join_then_join_wait)
{
    uint8_t buf[512];
    /* joining FORK: closure in R[0], child handle in R[1].
     * JOIN_WAIT:  reads R[1] — matches joining FORK.B. */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_FORK, 0, 1, UFORK_JOIN), /* B=1: handle → R[1] */
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

/* ── VM-14 follow-up: bare joining OP_FORK B-operand bounds ─────────────────
 *
 * joining OP_FORK's dispatch WRITES R[B] (the child strand handle), but B was
 * marked UOPK_UNUSED in the shape table, so verify_byte_operand never
 * bounds-checked it.  The VM-14 adjacency rule transitively pins B for the
 * normal emit pattern (joining FORK followed by JOIN_WAIT, B == JOIN_WAIT.A <=
 * max_reg) — but a BARE joining OP_FORK (no following JOIN_WAIT) with B >
 * max_reg slips through and dispatch writes a register OUTSIDE the declared
 * window: an OOB write from an untrusted chunk.  Closed by marking
 * OP_FORK.B as a register operand. */

/* Bare joining OP_FORK (no following JOIN_WAIT) with B > max_reg must reject. */
UTEST(verifier_rejects_bare_fork_join_b_oob)
{
    uint8_t buf[512];
    /* max_reg=0: only R[0] exists.  joining FORK A=0 (closure) B=5 (handle dst).
     * R[5] is outside the window — dispatch would OOB-write.  No JOIN_WAIT
     * follows, so the VM-14 adjacency rule does not catch it. */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_FORK, 0, 5, UFORK_JOIN),
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

/* Bare joining OP_FORK with B <= max_reg (in-window) must still accept — the
 * B-operand check must not over-reject valid bare forks. */
UTEST(verifier_accepts_bare_fork_join_b_in_window)
{
    uint8_t buf[512];
    /* max_reg=1: R[0], R[1] exist.  joining FORK A=0 B=1: handle → R[1], in window. */
    const uint32_t instrs[] = {
        uinstr_enc_abc(OP_FORK, 0, 1, UFORK_JOIN),
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
    utest_run("VM-14: verifier rejects JOIN_WAIT at pc 0",
              verifier_rejects_join_wait_at_pc0);
    utest_run("VM-14: verifier rejects JOIN_WAIT with non-fork predecessor",
              verifier_rejects_join_wait_with_non_fork_predecessor);
    utest_run("VM-14: verifier rejects JOIN_WAIT with mismatched child reg",
              verifier_rejects_join_wait_with_mismatched_child_reg);
    utest_run("VM-14: verifier accepts joining FORK then JOIN_WAIT",
              verifier_accepts_fork_join_then_join_wait);
    utest_run("VM-14: verifier rejects bare joining FORK with B > max_reg",
              verifier_rejects_bare_fork_join_b_oob);
    utest_run("VM-14: verifier accepts bare joining FORK with B in window",
              verifier_accepts_bare_fork_join_b_in_window);
    utest_run("VM-19: verifier rejects OP_SELF A==max_reg",
              verifier_rejects_op_self_a_eq_max_reg);
    utest_run("VM-19: verifier accepts OP_SELF A==max_reg-1",
              verifier_accepts_op_self_a_lt_max_reg);
    utest_run("v2: FORK mode matches its handle register",
              verifier_fork_mode_matches_handle_register);
    utest_run("v2: SCOPE_POP kind rules",
              verifier_scope_pop_kind_rules);
    utest_run("v2: INSTALL flag rules",
              verifier_install_flag_rules);
    utest_run("v2: CALL register window",
              verifier_call_register_window);
    utest_run("v2: scope and unwind targets in range",
              verifier_scope_and_unwind_targets_in_range);
    utest_run("v2: UNWIND_TO target after EXTARG rejected",
              verifier_unwind_target_after_extarg_rejected);
    utest_run("v2: two EXTARGs in a row rejected",
              verifier_double_extarg_rejected);
}
