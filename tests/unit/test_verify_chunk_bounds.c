/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for the verify_chunk_bounds deserialize-time pass (bytecode F2).
 *
 * Each test constructs a hand-crafted malformed bytecode buffer and asserts
 * uchunk_deserialize rejects it with the expected UCHUNK_LOAD_* error code.
 * One test (verify_well_formed_chunk_loads) confirms that no well-formed chunk
 * is rejected by the new pass.
 *
 * Builder helpers are deliberately self-contained (mirroring the convention
 * established in test_module_loader_hardening.c). */

#include "utest.h"
#include "chunk/uchunk.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* --- Shared bytecode-builder helpers --- */

static void vcb_build_good_header(uint8_t hdr[24]) {
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

static size_t vcb_put_varint(uint8_t *buf, size_t offset, uint64_t v) {
    while (v >= 0x80U) {
        buf[offset++] = (uint8_t)((v & 0x7FU) | 0x80U);
        v >>= 7;
    }
    buf[offset++] = (uint8_t)v;
    return offset;
}

/* Encode a little-endian uint32 instruction at buf[offset]. */
static size_t vcb_put_instr(uint8_t *buf, size_t offset, uint32_t instr) {
    buf[offset++] = (uint8_t)(instr & 0xFFU);
    buf[offset++] = (uint8_t)((instr >> 8)  & 0xFFU);
    buf[offset++] = (uint8_t)((instr >> 16) & 0xFFU);
    buf[offset++] = (uint8_t)((instr >> 24) & 0xFFU);
    return offset;
}

/* Helper: emit the root proto header + a given instruction array + empty
 * line-table + ic_count=0 + nested_count=N, then return offset.
 * Caller sets up nested protos after. */
static size_t vcb_emit_root_header(uint8_t *buf, size_t off,
                                    uint8_t max_reg, uint8_t nupvals,
                                    uint8_t nparams,
                                    const uint32_t *instrs, size_t n_instr,
                                    size_t nested_count) {
    buf[off++] = max_reg;
    buf[off++] = nupvals;
    buf[off++] = nparams;
    off = vcb_put_varint(buf, off, 0);         /* n_const */
    off = vcb_put_varint(buf, off, n_instr);   /* n_instr */
    /* 4-byte alignment pad for instruction array */
    while ((off & 3U) != 0U) buf[off++] = 0;
    for (size_t i = 0; i < n_instr; i++) {
        off = vcb_put_instr(buf, off, instrs[i]);
    }
    /* Line table: one delta entry per instruction, value 0. */
    off = vcb_put_varint(buf, off, n_instr);   /* n_deltas */
    for (size_t i = 0; i < n_instr; i++) buf[off++] = 0;
    off = vcb_put_varint(buf, off, 0);         /* n_abs_lines */
    off = vcb_put_varint(buf, off, 0);         /* ic_count */
    off = vcb_put_varint(buf, off, nested_count);
    return off;
}

/* Build a minimal valid module: one OP_RET, no nested protos. */
static size_t vcb_build_minimal(uint8_t *buf) {
    vcb_build_good_header(buf);
    size_t off = 24;
    off = vcb_put_varint(buf, off, 0);  /* source_name_len */
    uint32_t instrs[] = { (uint32_t)OP_RET };
    off = vcb_emit_root_header(buf, off, 0, 0, 0, instrs, 1, 0);
    return off;
}

/* =========================================================================
 * Test 1: well-formed chunk loads cleanly (regression guard)
 * ========================================================================= */

UTEST(verify_well_formed_chunk_loads)
{
    uint8_t buf[256];
    size_t sz = vcb_build_minimal(buf);
    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, sz, NULL, NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)rc);
    uchunk_destroy(m, NULL);
}

/* =========================================================================
 * Test 2: OP_JMP target forward out of bounds
 * ========================================================================= */

UTEST(jmp_target_forward_out_of_bounds)
{
    /* Two-instruction stream: OP_JMP with Bx=32769 (target=pc+1=1, instr_count=2)
     * Wait — target = pc + signed(Bx) - 32768.  pc=0, Bx=32769 → target=1.
     * That would be valid (target < 2).  Use Bx=32770 → target=2 >= instr_count=2. */
    uint8_t buf[512];
    vcb_build_good_header(buf);
    size_t off = 24;
    off = vcb_put_varint(buf, off, 0);  /* source_name_len */

    /* Bx=32770 → target = 0 + 32770 - 32768 = 2; instr_count=2 → out of bounds. */
    uint16_t bx = 32770;
    uint32_t jmp_instr = (uint32_t)OP_JMP | ((uint32_t)bx << 16);
    uint32_t instrs[] = { jmp_instr, (uint32_t)OP_RET };
    off = vcb_emit_root_header(buf, off, 0, 0, 0, instrs, 2, 0);

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_JMP_OUT_OF_BOUNDS, (int)rc);
    uchunk_destroy(m, NULL);
}

/* =========================================================================
 * Test 3: OP_JMP target backward out of bounds (underflow)
 * ========================================================================= */

UTEST(jmp_target_backward_out_of_bounds)
{
    /* Bx=0 → target = 0 + 0 - 32768 = -32768; negative, out of bounds. */
    uint8_t buf[512];
    vcb_build_good_header(buf);
    size_t off = 24;
    off = vcb_put_varint(buf, off, 0);

    uint32_t jmp_instr = (uint32_t)OP_JMP | ((uint32_t)0U << 16); /* Bx=0 */
    uint32_t instrs[] = { jmp_instr, (uint32_t)OP_RET };
    off = vcb_emit_root_header(buf, off, 0, 0, 0, instrs, 2, 0);

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_JMP_OUT_OF_BOUNDS, (int)rc);
    uchunk_destroy(m, NULL);
}

/* =========================================================================
 * Test 4: OP_JMP with valid (no-op) target loads cleanly
 * ========================================================================= */

UTEST(jmp_target_noop_loads_ok)
{
    /* Bx=32768 → target = 0 + 32768 - 32768 = 0 (self-jump); valid. */
    uint8_t buf[512];
    vcb_build_good_header(buf);
    size_t off = 24;
    off = vcb_put_varint(buf, off, 0);

    uint32_t jmp_instr = (uint32_t)OP_JMP | ((uint32_t)32768U << 16); /* Bx=32768 */
    uint32_t instrs[] = { jmp_instr, (uint32_t)OP_RET };
    off = vcb_emit_root_header(buf, off, 0, 0, 0, instrs, 2, 0);

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)rc);
    uchunk_destroy(m, NULL);
}

/* =========================================================================
 * Test 5: OP_CALL C low-7 == 0 rejected
 * ========================================================================= */

UTEST(call_nresults_zero_rejected)
{
    /* OP_CALL A=0, B=1 (nargs+1), C=0 (nresults+1=0 is malformed).
     * max_reg=1 so R[0] and R[1] are valid. */
    uint8_t buf[512];
    vcb_build_good_header(buf);
    size_t off = 24;
    off = vcb_put_varint(buf, off, 0);

    /* A=0, B=1, C=0 (method_flag=0, nresults+1=0) */
    uint32_t call_instr = (uint32_t)OP_CALL
        | ((uint32_t)0U << 8)   /* A */
        | ((uint32_t)1U << 16)  /* B */
        | ((uint32_t)0U << 24); /* C: low-7=0, method_flag=0 */
    uint32_t instrs[] = { call_instr, (uint32_t)OP_RET };
    off = vcb_emit_root_header(buf, off, 1, 0, 0, instrs, 2, 0);

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_CALL_NRESULTS_ZERO, (int)rc);
    uchunk_destroy(m, NULL);
}

/* =========================================================================
 * Test 6: OP_CALL C with method-flag set but low-7 == 0 rejected
 * ========================================================================= */

UTEST(call_method_flag_nresults_zero_rejected)
{
    /* C=0x80: method-flag set (bit 7), nresults+1=0 (low-7=0). Still malformed. */
    uint8_t buf[512];
    vcb_build_good_header(buf);
    size_t off = 24;
    off = vcb_put_varint(buf, off, 0);

    uint32_t call_instr = (uint32_t)OP_CALL
        | ((uint32_t)0U   << 8)
        | ((uint32_t)1U   << 16)
        | ((uint32_t)0x80U << 24); /* method-flag set, nresults+1=0 */
    uint32_t instrs[] = { call_instr, (uint32_t)OP_RET };
    off = vcb_emit_root_header(buf, off, 1, 0, 0, instrs, 2, 0);

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_CALL_NRESULTS_ZERO, (int)rc);
    uchunk_destroy(m, NULL);
}

/* =========================================================================
 * Test 7: OP_CALL C with low-7 >= 1 loads cleanly
 * ========================================================================= */

UTEST(call_nresults_one_loads_ok)
{
    /* C=1: nresults+1=1 (one result). Valid. */
    uint8_t buf[512];
    vcb_build_good_header(buf);
    size_t off = 24;
    off = vcb_put_varint(buf, off, 0);

    uint32_t call_instr = (uint32_t)OP_CALL
        | ((uint32_t)0U << 8)
        | ((uint32_t)1U << 16)
        | ((uint32_t)1U << 24); /* nresults+1=1 */
    uint32_t instrs[] = { call_instr, (uint32_t)OP_RET };
    off = vcb_emit_root_header(buf, off, 1, 0, 0, instrs, 2, 0);

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)rc);
    uchunk_destroy(m, NULL);
}

/* =========================================================================
 * Test 8: OP_TAG_STOP rejected as reserved opcode
 * ========================================================================= */

UTEST(tag_stop_reserved_opcode_rejected)
{
    uint8_t buf[512];
    vcb_build_good_header(buf);
    size_t off = 24;
    off = vcb_put_varint(buf, off, 0);

    /* OP_TAG_STOP A=0 B=0: requires max_reg >= 0 (always true). */
    uint32_t tag_stop = (uint32_t)OP_TAG_STOP;
    uint32_t instrs[] = { tag_stop, (uint32_t)OP_RET };
    off = vcb_emit_root_header(buf, off, 0, 0, 0, instrs, 2, 0);

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_RESERVED_OPCODE, (int)rc);
    uchunk_destroy(m, NULL);
}

/* =========================================================================
 * Test 9: OP_CLOSURE upvalue prelude truncated (extends past end)
 * ========================================================================= */

UTEST(closure_upvalue_prelude_truncated)
{
    /* Build a root proto with one nested child (nupvals=1) and an OP_CLOSURE
     * instruction, but NO room for the pseudo-instruction that follows.
     * Instructions: [OP_CLOSURE Bx=0, OP_RET]
     * Child proto: max_reg=0, nupvals=1 — so OP_CLOSURE requires 1 prelude instr.
     * With [OP_CLOSURE, OP_RET] that is 2 total instructions; the prelude
     * would occupy slot 1 (the OP_RET), leaving no OP_RET past the prelude.
     * The verifier computes vi + nupvals >= instr_count → 0 + 1 >= 2? No (1 < 2).
     * We need nupvals=2 to trigger: 0 + 2 >= 2 → out of bounds.
     *
     * So: child nupvals=2, instructions=[OP_CLOSURE Bx=0, pseudo1, OP_RET] but
     * we write only 2 instructions total so prelude extends past end. */
    uint8_t buf[1024];
    vcb_build_good_header(buf);
    size_t off = 24;
    off = vcb_put_varint(buf, off, 0);  /* source_name_len */

    /* Root proto: max_reg=0, nupvals=0, nparams=0 */
    buf[off++] = 0;   /* max_reg */
    buf[off++] = 0;   /* nupvals */
    buf[off++] = 0;   /* nparams */
    off = vcb_put_varint(buf, off, 0);  /* n_const */

    /* Root instructions: [OP_CLOSURE Bx=0, OP_RET] — only 2 instructions.
     * Child nupvals=2 means prelude needs 2 pseudo-instrs after OP_CLOSURE,
     * but only 1 slot remains (OP_RET), so vi+nupvals=0+2=2 >= instr_count=2. */
    off = vcb_put_varint(buf, off, 2);  /* n_instr */
    while ((off & 3U) != 0U) buf[off++] = 0;
    /* OP_CLOSURE A=0 Bx=0 */
    uint32_t closure_instr = (uint32_t)OP_CLOSURE | ((uint32_t)0U << 8) | ((uint32_t)0U << 16);
    off = vcb_put_instr(buf, off, closure_instr);
    /* OP_RET (the only other instruction) */
    off = vcb_put_instr(buf, off, (uint32_t)OP_RET);

    /* Line table */
    off = vcb_put_varint(buf, off, 2);
    buf[off++] = 0; buf[off++] = 0;
    off = vcb_put_varint(buf, off, 0);   /* n_abs_lines */
    off = vcb_put_varint(buf, off, 0);   /* root ic_count */
    off = vcb_put_varint(buf, off, 1);   /* nested_count = 1 */

    /* Nested child: max_reg=0, nupvals=2, nparams=0; one OP_RET. */
    buf[off++] = 0;    /* child max_reg */
    buf[off++] = 2;    /* child nupvals = 2 (nupvals+nparams=2 <= max_reg+1=1 would fail
                          W4 check nupvals+nparams <= max_reg+1 → 2 > 1: corrupt!
                          Use max_reg=1 to pass W4 check). */
    /* Redo child: max_reg=1, nupvals=2, nparams=0 → 2+0=2 <= 1+1=2: OK. */
    buf[off - 2] = 1;  /* child max_reg = 1 */
    buf[off++] = 0;    /* child nparams */
    off = vcb_put_varint(buf, off, 0);   /* child n_const */
    off = vcb_put_varint(buf, off, 1);   /* child n_instr */
    while ((off & 3U) != 0U) buf[off++] = 0;
    off = vcb_put_instr(buf, off, (uint32_t)OP_RET);
    off = vcb_put_varint(buf, off, 1);   /* child n_deltas */
    buf[off++] = 0;
    off = vcb_put_varint(buf, off, 0);   /* child n_abs_lines */
    off = vcb_put_varint(buf, off, 0);   /* child ic_count */
    off = vcb_put_varint(buf, off, 0);   /* child nested_count */

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_TRUNCATED_UPVALUES, (int)rc);
    uchunk_destroy(m, NULL);
}

/* =========================================================================
 * Test 10: OP_CLOSURE upvalue pseudo-instr with invalid in_stack value (>1)
 * ========================================================================= */

UTEST(closure_upvalue_in_stack_invalid)
{
    /* Root: [OP_CLOSURE Bx=0, pseudo(in_stack=2, src_idx=0), OP_RET]
     * Child: nupvals=1.  The pseudo B field encodes in_stack; value 2 is invalid
     * (only 0 or 1 are legal).
     *
     * The shape-table verifier checks the pseudo-instruction as OP_MOVE with
     * B=UOPK_REG, so B must be <= max_reg to pass that pass.  We use max_reg=5
     * so that B=2 is a valid register operand for the shape table, but the
     * bounds verifier catches in_stack=2 > 1 as malformed. */
    uint8_t buf[1024];
    vcb_build_good_header(buf);
    size_t off = 24;
    off = vcb_put_varint(buf, off, 0);

    buf[off++] = 5;  /* root max_reg = 5 (so B=2 passes shape-table REG check) */
    buf[off++] = 0;  /* root nupvals */
    buf[off++] = 0;  /* root nparams */
    off = vcb_put_varint(buf, off, 0);  /* n_const */
    off = vcb_put_varint(buf, off, 3);  /* n_instr = 3: OP_CLOSURE + pseudo + OP_RET */
    while ((off & 3U) != 0U) buf[off++] = 0;
    /* OP_CLOSURE A=0 Bx=0 */
    off = vcb_put_instr(buf, off, (uint32_t)OP_CLOSURE);
    /* pseudo: op=OP_MOVE, A=0, B=2 (in_stack=2: invalid, but <= max_reg=5 so
     * shape-table accepts it as a register), C=0 (src_idx) */
    uint32_t pseudo = (uint32_t)OP_MOVE
        | ((uint32_t)0U << 8)
        | ((uint32_t)2U << 16)  /* in_stack = 2 (invalid: must be 0 or 1) */
        | ((uint32_t)0U << 24);
    off = vcb_put_instr(buf, off, pseudo);
    off = vcb_put_instr(buf, off, (uint32_t)OP_RET);
    /* Line table */
    off = vcb_put_varint(buf, off, 3);
    buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
    off = vcb_put_varint(buf, off, 0);  /* n_abs_lines */
    off = vcb_put_varint(buf, off, 0);  /* ic_count */
    off = vcb_put_varint(buf, off, 1);  /* nested_count = 1 */

    /* Child: max_reg=5, nupvals=1, nparams=0 (nupvals+nparams=1 <= max_reg+1=6) */
    buf[off++] = 5; buf[off++] = 1; buf[off++] = 0;
    off = vcb_put_varint(buf, off, 0);  /* n_const */
    off = vcb_put_varint(buf, off, 1);  /* n_instr */
    while ((off & 3U) != 0U) buf[off++] = 0;
    off = vcb_put_instr(buf, off, (uint32_t)OP_RET);
    off = vcb_put_varint(buf, off, 1);
    buf[off++] = 0;
    off = vcb_put_varint(buf, off, 0);  /* n_abs_lines */
    off = vcb_put_varint(buf, off, 0);  /* ic_count */
    off = vcb_put_varint(buf, off, 0);  /* nested_count */

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_MALFORMED_UPVALUE, (int)rc);
    uchunk_destroy(m, NULL);
}

/* =========================================================================
 * Test 11: OP_CLOSURE upvalue pseudo-instr in_stack=1 but src_idx > max_reg
 * ========================================================================= */

UTEST(closure_upvalue_in_stack_src_idx_out_of_range)
{
    /* Root: max_reg=1, so registers 0 and 1 are valid.
     * pseudo: in_stack=1, src_idx=5 (> max_reg=1). */
    uint8_t buf[1024];
    vcb_build_good_header(buf);
    size_t off = 24;
    off = vcb_put_varint(buf, off, 0);

    buf[off++] = 1;  /* root max_reg = 1 */
    buf[off++] = 0;  /* root nupvals */
    buf[off++] = 0;  /* root nparams */
    off = vcb_put_varint(buf, off, 0);  /* n_const */
    off = vcb_put_varint(buf, off, 3);  /* n_instr */
    while ((off & 3U) != 0U) buf[off++] = 0;
    off = vcb_put_instr(buf, off, (uint32_t)OP_CLOSURE);
    /* pseudo: in_stack=1, src_idx=5 (> max_reg=1) */
    uint32_t pseudo = (uint32_t)OP_MOVE
        | ((uint32_t)0U << 8)
        | ((uint32_t)1U << 16)  /* in_stack = 1 */
        | ((uint32_t)5U << 24); /* src_idx = 5 > max_reg=1 */
    off = vcb_put_instr(buf, off, pseudo);
    off = vcb_put_instr(buf, off, (uint32_t)OP_RET);
    off = vcb_put_varint(buf, off, 3);
    buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
    off = vcb_put_varint(buf, off, 0);  /* n_abs_lines */
    off = vcb_put_varint(buf, off, 0);  /* ic_count */
    off = vcb_put_varint(buf, off, 1);  /* nested_count = 1 */

    /* Child: max_reg=0, nupvals=1, nparams=0 */
    buf[off++] = 0; buf[off++] = 1; buf[off++] = 0;
    off = vcb_put_varint(buf, off, 0);
    off = vcb_put_varint(buf, off, 1);
    while ((off & 3U) != 0U) buf[off++] = 0;
    off = vcb_put_instr(buf, off, (uint32_t)OP_RET);
    off = vcb_put_varint(buf, off, 1);
    buf[off++] = 0;
    off = vcb_put_varint(buf, off, 0);
    off = vcb_put_varint(buf, off, 0);
    off = vcb_put_varint(buf, off, 0);

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_MALFORMED_UPVALUE, (int)rc);
    uchunk_destroy(m, NULL);
}

/* =========================================================================
 * Test 12: OP_CLOSURE with valid prelude (in_stack=1, src_idx=0) loads OK
 * ========================================================================= */

UTEST(closure_upvalue_valid_loads_ok)
{
    /* Root: max_reg=1, nupvals=0.
     * Instructions: [OP_CLOSURE Bx=0, pseudo(in_stack=1,src_idx=0), OP_RET]
     * Child: max_reg=0, nupvals=1. */
    uint8_t buf[1024];
    vcb_build_good_header(buf);
    size_t off = 24;
    off = vcb_put_varint(buf, off, 0);

    buf[off++] = 1;  /* root max_reg */
    buf[off++] = 0;  /* root nupvals */
    buf[off++] = 0;  /* root nparams */
    off = vcb_put_varint(buf, off, 0);
    off = vcb_put_varint(buf, off, 3);
    while ((off & 3U) != 0U) buf[off++] = 0;
    off = vcb_put_instr(buf, off, (uint32_t)OP_CLOSURE);
    /* pseudo: in_stack=1, src_idx=0 (R[0] is valid: 0 <= max_reg=1) */
    uint32_t pseudo = (uint32_t)OP_MOVE
        | ((uint32_t)0U << 8)
        | ((uint32_t)1U << 16)  /* in_stack=1 */
        | ((uint32_t)0U << 24); /* src_idx=0 */
    off = vcb_put_instr(buf, off, pseudo);
    off = vcb_put_instr(buf, off, (uint32_t)OP_RET);
    off = vcb_put_varint(buf, off, 3);
    buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
    off = vcb_put_varint(buf, off, 0);
    off = vcb_put_varint(buf, off, 0);
    off = vcb_put_varint(buf, off, 1);

    /* Child: max_reg=0, nupvals=1 */
    buf[off++] = 0; buf[off++] = 1; buf[off++] = 0;
    off = vcb_put_varint(buf, off, 0);
    off = vcb_put_varint(buf, off, 1);
    while ((off & 3U) != 0U) buf[off++] = 0;
    off = vcb_put_instr(buf, off, (uint32_t)OP_RET);
    off = vcb_put_varint(buf, off, 1);
    buf[off++] = 0;
    off = vcb_put_varint(buf, off, 0);
    off = vcb_put_varint(buf, off, 0);
    off = vcb_put_varint(buf, off, 0);

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)rc);
    uchunk_destroy(m, NULL);
}

/* =========================================================================
 * Test 13: OP_CLOSURE re-capture (in_stack=0) with src_idx >= parent nupvals
 * ========================================================================= */

UTEST(closure_upvalue_recapture_src_idx_out_of_range)
{
    /* Root: nupvals=0 (no parent upvalues).  A child nested inside root
     * tries to re-capture upvalue 0 from root's closure (in_stack=0),
     * but root has nupvals=0 — so src_idx=0 >= nupvals=0 is out of range. */
    uint8_t buf[1024];
    vcb_build_good_header(buf);
    size_t off = 24;
    off = vcb_put_varint(buf, off, 0);

    buf[off++] = 1;  /* root max_reg */
    buf[off++] = 0;  /* root nupvals = 0 */
    buf[off++] = 0;  /* root nparams */
    off = vcb_put_varint(buf, off, 0);
    off = vcb_put_varint(buf, off, 3);
    while ((off & 3U) != 0U) buf[off++] = 0;
    off = vcb_put_instr(buf, off, (uint32_t)OP_CLOSURE);
    /* pseudo: in_stack=0 (re-capture), src_idx=0 — but root nupvals=0 */
    uint32_t pseudo = (uint32_t)OP_MOVE
        | ((uint32_t)0U << 8)
        | ((uint32_t)0U << 16)  /* in_stack=0 */
        | ((uint32_t)0U << 24); /* src_idx=0 >= root.nupvals=0 → out of range */
    off = vcb_put_instr(buf, off, pseudo);
    off = vcb_put_instr(buf, off, (uint32_t)OP_RET);
    off = vcb_put_varint(buf, off, 3);
    buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
    off = vcb_put_varint(buf, off, 0);
    off = vcb_put_varint(buf, off, 0);
    off = vcb_put_varint(buf, off, 1);

    /* Child: max_reg=0, nupvals=1 */
    buf[off++] = 0; buf[off++] = 1; buf[off++] = 0;
    off = vcb_put_varint(buf, off, 0);
    off = vcb_put_varint(buf, off, 1);
    while ((off & 3U) != 0U) buf[off++] = 0;
    off = vcb_put_instr(buf, off, (uint32_t)OP_RET);
    off = vcb_put_varint(buf, off, 1);
    buf[off++] = 0;
    off = vcb_put_varint(buf, off, 0);
    off = vcb_put_varint(buf, off, 0);
    off = vcb_put_varint(buf, off, 0);

    UProto *m = NULL;
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_MALFORMED_UPVALUE, (int)rc);
    uchunk_destroy(m, NULL);
}

/* =========================================================================
 * Test 14: error name lookup for all new codes
 * ========================================================================= */

UTEST(new_error_codes_have_names)
{
    UASSERT(uchunk_load_error_name(UCHUNK_LOAD_TRUNCATED_UPVALUES) != NULL);
    UASSERT(uchunk_load_error_name(UCHUNK_LOAD_MALFORMED_UPVALUE)  != NULL);
    UASSERT(uchunk_load_error_name(UCHUNK_LOAD_JMP_OUT_OF_BOUNDS)  != NULL);
    UASSERT(uchunk_load_error_name(UCHUNK_LOAD_CALL_NRESULTS_ZERO) != NULL);
    UASSERT(uchunk_load_error_name(UCHUNK_LOAD_RESERVED_OPCODE)    != NULL);
    /* None should map to the generic fallback. */
    UASSERT(strcmp(uchunk_load_error_name(UCHUNK_LOAD_TRUNCATED_UPVALUES),
                   "UCHUNK_LOAD_UNKNOWN") != 0);
    UASSERT(strcmp(uchunk_load_error_name(UCHUNK_LOAD_MALFORMED_UPVALUE),
                   "UCHUNK_LOAD_UNKNOWN") != 0);
    UASSERT(strcmp(uchunk_load_error_name(UCHUNK_LOAD_JMP_OUT_OF_BOUNDS),
                   "UCHUNK_LOAD_UNKNOWN") != 0);
    UASSERT(strcmp(uchunk_load_error_name(UCHUNK_LOAD_CALL_NRESULTS_ZERO),
                   "UCHUNK_LOAD_UNKNOWN") != 0);
    UASSERT(strcmp(uchunk_load_error_name(UCHUNK_LOAD_RESERVED_OPCODE),
                   "UCHUNK_LOAD_UNKNOWN") != 0);
}

/* =========================================================================
 * Suite registration
 * ========================================================================= */

void test_verify_chunk_bounds_suite(void) {
    utest_run("verify_well_formed_chunk_loads",
              verify_well_formed_chunk_loads);
    utest_run("jmp_target_forward_out_of_bounds",
              jmp_target_forward_out_of_bounds);
    utest_run("jmp_target_backward_out_of_bounds",
              jmp_target_backward_out_of_bounds);
    utest_run("jmp_target_noop_loads_ok",
              jmp_target_noop_loads_ok);
    utest_run("call_nresults_zero_rejected",
              call_nresults_zero_rejected);
    utest_run("call_method_flag_nresults_zero_rejected",
              call_method_flag_nresults_zero_rejected);
    utest_run("call_nresults_one_loads_ok",
              call_nresults_one_loads_ok);
    utest_run("tag_stop_reserved_opcode_rejected",
              tag_stop_reserved_opcode_rejected);
    utest_run("closure_upvalue_prelude_truncated",
              closure_upvalue_prelude_truncated);
    utest_run("closure_upvalue_in_stack_invalid",
              closure_upvalue_in_stack_invalid);
    utest_run("closure_upvalue_in_stack_src_idx_out_of_range",
              closure_upvalue_in_stack_src_idx_out_of_range);
    utest_run("closure_upvalue_valid_loads_ok",
              closure_upvalue_valid_loads_ok);
    utest_run("closure_upvalue_recapture_src_idx_out_of_range",
              closure_upvalue_recapture_src_idx_out_of_range);
    utest_run("new_error_codes_have_names",
              new_error_codes_have_names);
}
