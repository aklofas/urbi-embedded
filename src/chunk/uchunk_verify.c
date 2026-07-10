/* SPDX-License-Identifier: BSD-3-Clause */
/* uchunk_verify.c — bytecode verifier passes (F2 bounds + F3 ic-index).
 *
 * The security-load-bearing load-time verifier, split out of uchunk_io.c so
 * the shape-table operand check, per-sequence bounds pass, and ic-index DFS
 * mirror check are independently readable.  Driven by uchunk_deserialize via
 * the entry points declared in uchunk_internal.h.  Freestanding. */

#include "chunk/uchunk.h"
#include "chunk/uchunk_internal.h"   /* MDecCtx + verifier entry-point decls */
#include "uopcode_shape.h"

#include <stdarg.h>               /* va_list / va_start / va_end — freestanding-ok */
#include <stddef.h>
#include <stdint.h>

/* Safe snprintf-style diagnostic sink, chunk-verify-local mirror of the
 * uchunk_io.c helper.  No-op when errmsg==NULL or errcap==0, and fully
 * suppressed under -ffreestanding. */
#if __STDC_HOSTED__
#  include <stdio.h>

static void set_errmsg(char *errmsg, size_t errcap, const char *fmt, ...) {
    if (errmsg == NULL || errcap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    /* False positive: ap is initialized by va_start, consumed by vsnprintf,
     * then cleared by va_end.  Analyzer cannot see through the va_list
     * contract on the vsnprintf prototype. */
    (void)vsnprintf(errmsg, errcap, fmt, ap);  /* NOLINT(clang-analyzer-valist.Uninitialized) — ap initialized by va_start above */
    va_end(ap);
}
#else  /* freestanding */

static void set_errmsg(char *errmsg, size_t errcap, const char *fmt, ...) {
    (void)errmsg;
    (void)errcap;
    (void)fmt;
}
#endif  /* __STDC_HOSTED__ */

/* Verify a single byte field per its UOperandKind.  max_reg is the
   per-block bound (root chunk uses module->max_reg; nested protos use
   p->max_reg). */
static UChunkLoadError verify_byte_operand(MDecCtx *d, uint8_t op,
                                            uint8_t value, UOperandKind kind,
                                            const char *which, size_t pc,
                                            uint8_t max_reg) {
    switch (kind) {
        case UOPK_UNUSED:
            return UCHUNK_LOAD_OK;
        case UOPK_REG:
            if (value > max_reg) {
                set_errmsg(d->errmsg, d->errcap,
                           "register %s=%u > max_reg=%u at pc %zu (op=%u)",
                           which, (unsigned)value,
                           (unsigned)max_reg, pc, (unsigned)op);
                return UCHUNK_LOAD_CORRUPT;
            }
            return UCHUNK_LOAD_OK;
        case UOPK_IMM_BOOL:
            if (value > 1U) {
                set_errmsg(d->errmsg, d->errcap,
                           "%s=%u not a 0/1 immediate at pc %zu (op=%u)",
                           which, (unsigned)value, pc, (unsigned)op);
                return UCHUNK_LOAD_CORRUPT;
            }
            return UCHUNK_LOAD_OK;
        case UOPK_IMM_FLAGS:
            return UCHUNK_LOAD_OK;  /* full byte accepted; flag bits unconstrained */
        case UOPK_IMM_REG_NIBBLE: {
            /* The byte packs flags (high nibble) + reg_idx (low nibble).
             * tag_reg is constrained to [0,15] AND <= max_reg. */
            uint8_t reg_idx = value & 0x0FU;
            if (reg_idx > max_reg) {
                set_errmsg(d->errmsg, d->errcap,
                           "%s tag_reg=%u > max_reg=%u at pc %zu (op=%u)",
                           which, (unsigned)reg_idx,
                           (unsigned)max_reg, pc, (unsigned)op);
                return UCHUNK_LOAD_CORRUPT;
            }
            return UCHUNK_LOAD_OK;
        }
        case UOPK_UPVAL_IDX:
            /* Runtime-checked at OP_GETUPVAL/OP_SETUPVAL dispatch (UClosure
             * carries the upvalue array length).  No static range. */
            return UCHUNK_LOAD_OK;
        case UOPK_FRAME_REG_BASE:
            /* OP_PUSH_FRAME_GUARD A is base register; <= max_reg. */
            if (value > max_reg) {
                set_errmsg(d->errmsg, d->errcap,
                           "%s frame guard base=%u > max_reg=%u at pc %zu (op=%u)",
                           which, (unsigned)value, (unsigned)max_reg,
                           pc, (unsigned)op);
                return UCHUNK_LOAD_CORRUPT;
            }
            return UCHUNK_LOAD_OK;
        case UOPK_FRAME_REG_COUNT:
            /* No standalone range check — OP_PUSH_FRAME_GUARD A+B
             * boundary check happens at the per-instruction arm below
             * because we need both bytes simultaneously. */
            return UCHUNK_LOAD_OK;
    }
    return UCHUNK_LOAD_OK;
}

/* OP_JMP Bx range note:
 *   Bx is a 16-bit unsigned field treated as signed with bias 32768
 *   (effective range -32768..+32767).  The shape-table verifier (verify_walk_block)
 *   accepts UBXK_JUMP_SIGNED with no per-instruction bounds because it operates
 *   one instruction at a time without absolute PC context.  The per-sequence
 *   verify_chunk_bounds pass (bytecode F2, v0.10.7) computes
 *   target = pc + signed(Bx) - 32768 and rejects targets outside [0, instr_count)
 *   with UCHUNK_LOAD_JMP_OUT_OF_BOUNDS, replacing the prior runtime-fatal path. */
/* Return true if `op` is an IC-bearing opcode (carries an ic_idx in C).
 * Mirror at v1.6: OP_GETSLOT, OP_SETSLOT, OP_GETSLOT_CHANGE_EVENT, OP_SELF.
 * Mirror discipline: any new IC-bearing opcode added in a future
 * milestone must be added here AND in uemit_assign_ic_index call sites. */
static bool op_carries_ic_index(uint8_t op) {
    return op == (uint8_t)OP_GETSLOT
        || op == (uint8_t)OP_SETSLOT
        || op == (uint8_t)OP_GETSLOT_CHANGE_EVENT
        || op == (uint8_t)OP_SELF;
}

/* Walk one block of instructions (root chunk OR a nested proto) against
   the opcode-shape table, applying per-block bounds (max_reg /
   const_count / instr_count / nested_count / ic_count).  Callers pass
   the root-level nested_count for both root and per-proto walks since
   the v1.5 emitter allocates all function literals as flat siblings
   under the root UModule's nested[] (an OP_CLOSURE inside a nested
   proto refers to a sibling slot in the same root array). */
static UChunkLoadError verify_walk_block(MDecCtx *d,
                                          uint8_t max_reg,
                                          size_t const_count,
                                          size_t instr_count,
                                          size_t nested_count,
                                          uint16_t ic_count,
                                          const uint32_t *instructions) {
    /* MOD-016: count IC-bearing opcodes seen during the walk so we
     * can cross-validate ic_count after the loop.  Every ic_idx must be
     * < ic_count (per-instruction); ic_count must be <= ic_seen
     * (count check; rejects modules that lie about ic_count without
     * emitting matching IC sites). */
    size_t ic_seen = 0;
    size_t vi;
    for (vi = 0; vi < instr_count; vi++) {
        uint32_t ins = instructions[vi];
        uint8_t  op  = (uint8_t)uinstr_op(ins);
        if (op >= (uint8_t)OP_MAX) {
            set_errmsg(d->errmsg, d->errcap, "corrupt opcode %u at pc %zu",
                       (unsigned)op, vi);
            return UCHUNK_LOAD_CORRUPT;
        }
        const UOpcodeShape *sh = &urbi_opcode_shapes[op];

        uint8_t a = uinstr_a(ins);
        UChunkLoadError rc = verify_byte_operand(d, op, a, sh->a_kind, "A", vi, max_reg);
        if (rc != UCHUNK_LOAD_OK) return rc;

        /* Cross-validate IC index for IC-bearing opcodes. */
        if (op_carries_ic_index(op)) {
            uint8_t ic_idx = uinstr_c(ins);
            if ((uint16_t)ic_idx >= ic_count) {
                set_errmsg(d->errmsg, d->errcap,
                           "ic_idx=%u >= ic_count=%u at pc %zu (op=%u)",
                           (unsigned)ic_idx, (unsigned)ic_count, vi, (unsigned)op);
                return UCHUNK_LOAD_CORRUPT;
            }
            ic_seen++;
        }

        if (sh->format == UOPF_ABC) {
            uint8_t b = uinstr_b(ins);
            uint8_t c = uinstr_c(ins);
            rc = verify_byte_operand(d, op, b, sh->b_kind, "B", vi, max_reg);
            if (rc != UCHUNK_LOAD_OK) return rc;
            rc = verify_byte_operand(d, op, c, sh->c_kind, "C", vi, max_reg);
            if (rc != UCHUNK_LOAD_OK) return rc;

            /* OP_PUSH_FRAME_GUARD: cross-byte invariant base+count <= max_reg+1. */
            if (op == (uint8_t)OP_PUSH_FRAME_GUARD) {
                if ((unsigned)a + (unsigned)b > (unsigned)max_reg + 1U) {
                    set_errmsg(d->errmsg, d->errcap,
                               "frame guard base+count=%u exceeds max_reg+1=%u at pc %zu",
                               (unsigned)a + (unsigned)b,
                               (unsigned)max_reg + 1U, vi);
                    return UCHUNK_LOAD_CORRUPT;
                }
            }
            /* VM-14: OP_JOIN_WAIT's dead-child fast path reads a
             * strand handle that eager DEAD-reap may have freed; the adjacency
             * invariant (OP_FORK_JOIN immediately before, FORK_JOIN.B ==
             * JOIN_WAIT.A) is the only pin.  Enforce at load time so corrupt or
             * hand-built chunks cannot exploit the UAF. */
            if (op == (uint8_t)OP_JOIN_WAIT) {
                if (vi == 0U) {
                    set_errmsg(d->errmsg, d->errcap,
                               "OP_JOIN_WAIT at pc 0: no preceding OP_FORK_JOIN");
                    return UCHUNK_LOAD_CORRUPT;
                }
                uint32_t prev = instructions[vi - 1U];
                uint8_t  pop  = (uint8_t)uinstr_op(prev);
                if (pop != (uint8_t)OP_FORK_JOIN || uinstr_b(prev) != a) {
                    set_errmsg(d->errmsg, d->errcap,
                               "OP_JOIN_WAIT at pc %zu not adjacent to its"
                               " OP_FORK_JOIN (prev op=%u, prev B=%u,"
                               " JOIN_WAIT A=%u)",
                               vi, (unsigned)pop,
                               (unsigned)uinstr_b(prev), (unsigned)a);
                    return UCHUNK_LOAD_CORRUPT;
                }
            }
            /* VM-19: OP_SELF writes R[A] (looked-up slot value) and
             * R[A+1] (self/receiver copy for OP_CALL).  The shape table only
             * verifies A <= max_reg; the cross-byte check also requires A+1. */
            if (op == (uint8_t)OP_SELF) {
                if ((unsigned)a + 1U > (unsigned)max_reg) {
                    set_errmsg(d->errmsg, d->errcap,
                               "OP_SELF A+1=%u exceeds max_reg=%u at pc %zu",
                               (unsigned)a + 1U, (unsigned)max_reg, vi);
                    return UCHUNK_LOAD_CORRUPT;
                }
            }
            /* VM-CORE-02: OP_CALL reads R[A..A+B-1] at dispatch (plain call:
             * R[A]=callee, R[A+1..A+B-1]=args; method: R[A+1]=self too).
             * The shape table validates A and B independently (each <= max_reg)
             * but not their sum; a chunk with A+B > max_reg+1 would read beyond
             * the allocated register frame.  Mirrors the OP_PUSH_FRAME_GUARD
             * check above (same pattern: base+count <= max_reg+1). */
            if (op == (uint8_t)OP_CALL) {
                if ((unsigned)a + (unsigned)b > (unsigned)max_reg + 1U) {
                    set_errmsg(d->errmsg, d->errcap,
                               "OP_CALL A+B=%u exceeds max_reg+1=%u at pc %zu"
                               " (register window overflow)",
                               (unsigned)a + (unsigned)b,
                               (unsigned)max_reg + 1U, vi);
                    return UCHUNK_LOAD_CORRUPT;
                }
            }
        } else {
            /* UOPF_ABX — Bx range check per shape table. */
            uint16_t bx = uinstr_bx(ins);
            switch (sh->bx_kind) {
                case UBXK_UNUSED:
                    break;
                case UBXK_POOL_INDEX:
                    if ((size_t)bx >= const_count) {
                        set_errmsg(d->errmsg, d->errcap,
                                   "Bx=%u >= const_count=%zu at pc %zu (op=%u)",
                                   (unsigned)bx, const_count, vi, (unsigned)op);
                        return UCHUNK_LOAD_CORRUPT;
                    }
                    break;
                case UBXK_NESTED_INDEX:
                    if ((size_t)bx >= nested_count) {
                        set_errmsg(d->errmsg, d->errcap,
                                   "Bx=%u >= nested_count=%zu at pc %zu (op=%u)",
                                   (unsigned)bx, nested_count, vi, (unsigned)op);
                        return UCHUNK_LOAD_CORRUPT;
                    }
                    break;
                case UBXK_JUMP_SIGNED:
                    /* No static range check; OP_JMP target out-of-range
                     * surfaces at runtime when pc + signed(Bx) - 32768
                     * leaves [0, instr_count). */
                    break;
                case UBXK_HANDLER_PC:
                    if ((size_t)bx >= instr_count) {
                        set_errmsg(d->errmsg, d->errcap,
                                   "handler-PC Bx=%u >= instr_count=%zu at pc %zu (op=%u)",
                                   (unsigned)bx, instr_count, vi, (unsigned)op);
                        return UCHUNK_LOAD_CORRUPT;
                    }
                    break;
                case UBXK_SYMBOL_ID:
                    /* At v1.5 the verifier accepts the full 0..65535
                     * symbol-id range; runtime resolves at dispatch. */
                    break;
            }
        }
    }
    /* Last instruction must be OP_RET (preserved from pre-v0.5.0 behavior).
     *
     * v1.x relaxation note: this strict trailing-OP_RET requirement
     * assumes the emitter always closes a chunk with an explicit return.
     * If a future bytecode revision allows fall-through-to-end semantics
     * (e.g. an implicit RET, or a tail-call that elides RET), this check
     * will need to widen.  At v0.5.6 every chunk uemit produces ends in
     * OP_RET, so the strict form catches truncated/corrupt bytecode
     * early. */
    if (instr_count > 0U) {
        uint32_t last = instructions[instr_count - 1U];
        if (uinstr_op(last) != OP_RET) {
            set_errmsg(d->errmsg, d->errcap, "last instruction is not OP_RET");
            return UCHUNK_LOAD_CORRUPT;
        }
    }
    /* MOD-016: ic_count must not exceed the count of IC-bearing
     * opcodes in the instruction stream.  Each ic_name (and the
     * corresponding runtime UIC entry) is keyed off an emitted
     * GETSLOT/SETSLOT/GETSLOT_CHANGE_EVENT site; lying about ic_count
     * would either leave UIC entries unused (waste) or — worse — leave
     * ic_name_strs[k>=ic_seen] holding a name that no instruction
     * indexes (eligible for confusion attacks at later milestones when
     * ic_index becomes wider). */
    if ((size_t)ic_count > ic_seen) {
        set_errmsg(d->errmsg, d->errcap,
                   "ic_count=%u exceeds %zu IC-bearing opcodes seen",
                   (unsigned)ic_count, ic_seen);
        return UCHUNK_LOAD_CORRUPT;
    }
    return UCHUNK_LOAD_OK;
}

/* v0.8.5: recursive verifier walk.  Each UProto is verified against its
 * OWN nested_count (per-parent OP_CLOSURE Bx index space), matching the
 * truly-recursive emitter contract.  Pre-v0.8.5 the verifier passed
 * the root-level nested_count for every nested proto because the flat
 * emitter routed every OP_CLOSURE to root's nested[] regardless of
 * lexical scope. */
static UChunkLoadError verify_proto_recursive(MDecCtx *d, const UProto *p) {
    if (p == NULL) return UCHUNK_LOAD_OK;
    UChunkLoadError rc = verify_walk_block(d,
                                            p->max_reg,
                                            p->const_count,
                                            p->instr_count,
                                            p->nested_count,
                                            p->ic_count,
                                            p->instructions);
    if (rc != UCHUNK_LOAD_OK) return rc;
    for (size_t i = 0; i < p->nested_count; i++) {
        rc = verify_proto_recursive(d, p->nested[i]);
        if (rc != UCHUNK_LOAD_OK) return rc;
    }
    return UCHUNK_LOAD_OK;
}

UChunkLoadError urbi_chunk_decode_verify(MDecCtx *d) {
    return verify_proto_recursive(d, d->rp);
}

/* --- bytecode F2: deserialize-time per-instruction operand bounds pass ---
 *
 * verify_chunk_bounds walks every UProto in the tree (DFS, mirrors
 * verify_proto_recursive above) and applies bounds checks that require
 * understanding instruction *sequences* or cross-instruction context, which
 * is more than the per-opcode shape table in verify_walk_block can express:
 *
 *   OP_CLOSURE upvalue prelude — the nupvals pseudo-instructions that follow
 *     an OP_CLOSURE must lie within the instruction array, and each must
 *     encode a valid (in_stack, src_idx) pair:
 *       in_stack = B in {0, 1}
 *       src_idx  = C; if in_stack==1, C <= proto->max_reg (local register);
 *                     if in_stack==0, C < proto->nupvals (re-capture from parent)
 *   OP_JMP target — Bx is a signed offset biased by 32768; the resolved target
 *     pc' = pc + signed(Bx) - 32768 must satisfy 0 <= pc' < instr_count.
 *     (The bias means Bx=32768 is a no-op jump; Bx=0 jumps backward 32768.)
 *   OP_CALL C low-7 — encodes nresults+1; must be >= 1 (0 means 0 results
 *     which is legal at runtime but the emitter never produces it; a
 *     hand-crafted module with C & 0x7F == 0 is malformed per the wire spec).
 *   OP_TAG_STOP — has full VM dispatch since v0.10.2 (label_op_tag_stop in
 *     uvm.c).  The compiler never emits it (scripted tag.stop() routes through
 *     the C API), but hand-built chunks may contain it.  Accepted at load time;
 *     see the REPL-N4 note in the code below and pinned by
 *     test_verify_chunk_bounds.c (tag_stop_roundtrips_ok).
 *
 * Design note: add ic_index DFS pre-order check here.  The function
 * receives the proto tree already decoded; a future pass can walk the tree and verify
 * that each proto's ic_index equals its DFS visit index without touching
 * the existing shape-table verifier. */
static UChunkLoadError verify_bounds_proto(MDecCtx *d, const UProto *p) {
    if (p == NULL) return UCHUNK_LOAD_OK;

    const uint32_t *instructions = p->instructions;
    size_t instr_count = p->instr_count;
    uint8_t max_reg    = p->max_reg;

    size_t vi = 0;
    while (vi < instr_count) {
        uint32_t ins = instructions[vi];
        uint8_t  op  = (uint8_t)(ins & 0xFFU);

        if (op == (uint8_t)OP_CLOSURE) {
            /* Read the child proto index (Bx) — already bounds-checked by
             * verify_walk_block against nested_count; no re-check needed.
             * What we verify here is the upvalue prelude that follows. */
            uint16_t bx = (uint16_t)((ins >> 16) & 0xFFFFU);
            /* Fetch nupvals from the referenced child proto. */
            size_t nupvals = 0;
            if ((size_t)bx < p->nested_count && p->nested[bx] != NULL) {
                nupvals = p->nested[bx]->nupvals;
            }
            /* The prelude is nupvals pseudo-instructions immediately after. */
            if (vi + nupvals >= instr_count) {
                /* Last instruction is always OP_RET; vi + nupvals must point
                 * AT or BEFORE the last instruction (which is OP_RET at
                 * instr_count - 1).  The prelude occupies slots vi+1 .. vi+nupvals;
                 * the slot vi+nupvals+1 is the next real instruction (or the OP_RET).
                 * If vi + nupvals >= instr_count the prelude would read past the end. */
                set_errmsg(d->errmsg, d->errcap,
                           "OP_CLOSURE at pc %zu: upvalue prelude (%zu entries)"
                           " extends past bytecode end (instr_count=%zu)",
                           vi, nupvals, instr_count);
                return UCHUNK_LOAD_TRUNCATED_UPVALUES;
            }
            /* Validate each upvalue pseudo-instruction. */
            for (size_t k = 1; k <= nupvals; k++) {
                uint32_t pv = instructions[vi + k];
                /* Only bits [8..15] (A), [16..23] (B = in_stack), [24..31] (C = src_idx)
                 * matter.  The opcode byte is not checked — the emitter sets it to
                 * OP_MOVE but the VM ignores it; accepting any opcode byte here
                 * avoids a future compat issue if a different encoder is used. */
                uint8_t in_stack = (uint8_t)((pv >> 16) & 0xFFU);  /* B */
                uint8_t src_idx  = (uint8_t)((pv >> 24) & 0xFFU);  /* C */
                if (in_stack > 1U) {
                    set_errmsg(d->errmsg, d->errcap,
                               "OP_CLOSURE at pc %zu: upvalue[%zu] in_stack=%u is not 0 or 1",
                               vi, k - 1U, (unsigned)in_stack);
                    return UCHUNK_LOAD_MALFORMED_UPVALUE;
                }
                if (in_stack) {
                    /* Local register capture: src_idx must be a valid register. */
                    if (src_idx > max_reg) {
                        set_errmsg(d->errmsg, d->errcap,
                                   "OP_CLOSURE at pc %zu: upvalue[%zu] in_stack=1"
                                   " src_idx=%u > max_reg=%u",
                                   vi, k - 1U, (unsigned)src_idx, (unsigned)max_reg);
                        return UCHUNK_LOAD_MALFORMED_UPVALUE;
                    }
                } else {
                    /* Re-capture from parent closure: src_idx must be a valid
                     * parent upvalue index.  p->nupvals is the parent's count.
                     * If the parent has no upvalues at all, any src_idx is
                     * out of range (there is nothing to re-capture). */
                    if (src_idx >= p->nupvals) {
                        set_errmsg(d->errmsg, d->errcap,
                                   "OP_CLOSURE at pc %zu: upvalue[%zu] in_stack=0"
                                   " src_idx=%u >= parent nupvals=%u",
                                   vi, k - 1U, (unsigned)src_idx,
                                   (unsigned)p->nupvals);
                        return UCHUNK_LOAD_MALFORMED_UPVALUE;
                    }
                }
            }
            /* Skip past the prelude: the outer loop increments vi once for the
             * OP_CLOSURE itself; advance by nupvals more. */
            vi += nupvals;

        } else if (op == (uint8_t)OP_JMP) {
            /* Bx encodes a signed offset biased by 32768:
             *   target = pc + signed(Bx) - 32768
             * where pc is the index of the OP_JMP instruction itself.
             * After the jump, execution resumes at the target; valid range
             * is [0, instr_count).  The bias means Bx=32768 is a no-op
             * (target == vi), Bx<32768 jumps backward, Bx>32768 jumps forward. */
            uint16_t bx = (uint16_t)((ins >> 16) & 0xFFFFU);
            /* Compute target as signed arithmetic, guarding against underflow. */
            int64_t signed_bx  = (int64_t)bx;
            int64_t target_i64 = (int64_t)vi + signed_bx - (int64_t)32768;
            if (target_i64 < 0 || (size_t)target_i64 >= instr_count) {
                set_errmsg(d->errmsg, d->errcap,
                           "OP_JMP at pc %zu: Bx=%u resolves to target=%lld"
                           " outside [0, %zu)",
                           vi, (unsigned)bx,
                           (long long)target_i64, instr_count);
                return UCHUNK_LOAD_JMP_OUT_OF_BOUNDS;
            }

        } else if (op == (uint8_t)OP_CALL) {
            /* C encodes: bit 7 = method-call flag; low 7 bits = nresults+1.
             * nresults+1 == 0 is nonsensical (zero results slots allocated
             * but the call tries to write at least one result).  The emitter
             * never produces 0 here; reject as malformed. */
            uint8_t c = (uint8_t)((ins >> 24) & 0xFFU);
            if ((c & 0x7FU) == 0U) {
                set_errmsg(d->errmsg, d->errcap,
                           "OP_CALL at pc %zu: C low-7=0 (nresults+1 must be >= 1)",
                           vi);
                return UCHUNK_LOAD_CALL_NRESULTS_ZERO;
            }

        }
        /* VM-13: OP_TAG_STOP (opcode 30) has full VM dispatch since
         * v0.10.2 (label_op_tag_stop in uvm.c).  The compiler never emits it —
         * scripted tag.stop() routes through tag_stop_native → urbi_tag_stop
         * C API — but hand-built or future chunks may include it.  The stale
         * "reserved" reject (from wire v1.8 when the dispatch arm was absent)
         * is removed here; OP_TAG_STOP is accepted at load time.
         * Pinned by tests/unit/test_verifier_cross_byte.c. */

        vi++;
    }

    /* Recurse into nested protos (DFS, matching verify_proto_recursive order). */
    for (size_t i = 0; i < p->nested_count; i++) {
        UChunkLoadError rc = verify_bounds_proto(d, p->nested[i]);
        if (rc != UCHUNK_LOAD_OK) return rc;
    }
    return UCHUNK_LOAD_OK;
}

/* Entry point: run verify_chunk_bounds from the root proto. */
UChunkLoadError urbi_chunk_verify_bounds(MDecCtx *d) {
    return verify_bounds_proto(d, d->rp);
}

/* --- bytecode F3: ic_index DFS pre-order verifier ---
 *
 * v0.8.5 truly-recursive emit assigns ic_index via uproto_alloc_nested's
 * ++root->next_proto_serial in DFS pre-order.  The deserializer mirrors this
 * at decode time (decode_proto recursive descent).  A corrupted chunk with
 * mis-ordered nested[] would produce in-range but wrong proto-instance lookups
 * in the OP_CLOSURE VM hot path (uvm.c).
 *
 * verify_ic_index_dfs walks the tree in DFS pre-order, matching each proto's
 * ic_index against a running counter.  Root must be 0; children are visited
 * left-to-right (nested[0] before nested[1]) and recursed depth-first,
 * matching the DFS pre-order assignment that emit uses. */
static UChunkLoadError verify_ic_index_dfs(const UProto *proto,
                                           uint16_t *next_idx,
                                           char *errmsg, size_t errcap) {
    if (proto == NULL) return UCHUNK_LOAD_OK;
    if (proto->ic_index != *next_idx) {
        set_errmsg(errmsg, errcap,
                   "ic_index mismatch: proto->ic_index=%u expected %u"
                   " (DFS pre-order invariant violated; bytecode F3)",
                   (unsigned)proto->ic_index, (unsigned)*next_idx);
        return UCHUNK_LOAD_IC_INDEX_MISMATCH;
    }
    (*next_idx)++;
    for (uint16_t i = 0U; i < (uint16_t)proto->nested_count; i++) {
        UChunkLoadError err = verify_ic_index_dfs(proto->nested[i],
                                                  next_idx, errmsg, errcap);
        if (err != UCHUNK_LOAD_OK) return err;
    }
    return UCHUNK_LOAD_OK;
}

UChunkLoadError uchunk_verify_ic_index(const UProto *root,
                                       char *errmsg, size_t errcap) {
    if (root == NULL) {
        set_errmsg(errmsg, errcap, "uchunk_verify_ic_index: root is NULL");
        return UCHUNK_LOAD_INVALID_ARG;
    }
    uint16_t next_idx = 0U;
    return verify_ic_index_dfs(root, &next_idx, errmsg, errcap);
}
