/* SPDX-License-Identifier: BSD-3-Clause */
/* uchunk_verify.c — the two load-time bytecode verifier passes.
 *
 * The security-load-bearing load-time verifier, split out of uchunk_io.c so
 * the per-instruction shape check (pass 1) and the per-sequence bounds pass
 * (pass 2) are independently readable.  Driven by uchunk_deserialize via
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
   per-block bound.  UOPK_IMM_SITE is checked by the caller, which holds
   the pending OP_EXTARG bits and the block's site_count. */
static UChunkLoadError verify_byte_operand(MDecCtx *d, uint8_t op,
                                            uint8_t value, UOperandKind kind,
                                            const char *which, size_t pc,
                                            uint8_t max_reg) {
    switch (kind) {
        case UOPK_UNUSED:
        case UOPK_IMM_FLAGS:      /* per-opcode rules below constrain the bits */
        case UOPK_UPVAL_IDX:      /* runtime-checked: UClosure carries the count */
        case UOPK_IMM_SITE:       /* checked by verify_walk_block */
        case UOPK_IMM_DEPTH:      /* a scope count; any byte */
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
        case UOPK_REG_OR_NONE:
            if (value != 0xFFU && value > max_reg) {
                set_errmsg(d->errmsg, d->errcap,
                           "register %s=%u > max_reg=%u (and not 0xFF) at pc %zu (op=%u)",
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
        case UOPK_IMM_MODE:
            if (value < 1U || value > 7U) {
                set_errmsg(d->errmsg, d->errcap,
                           "%s=%u not a mode in 1..7 at pc %zu (op=%u)",
                           which, (unsigned)value, pc, (unsigned)op);
                return UCHUNK_LOAD_CORRUPT;
            }
            return UCHUNK_LOAD_OK;
    }
    return UCHUNK_LOAD_OK;
}

/* The opcodes whose C operand is a site index, and so the only ones an
 * OP_EXTARG may precede. */
static bool op_takes_site(uint8_t op) {
    return op == (uint8_t)OP_GETSLOT || op == (uint8_t)OP_SETSLOT
        || op == (uint8_t)OP_SETSLOT_UPDATE || op == (uint8_t)OP_SELF
        || op == (uint8_t)OP_GETSLOT_CHANGE_EVENT;
}

/* The cross-byte rules a shape row cannot express, for the ABC opcodes
 * that have one.  `instructions` and `vi` are there for JOIN_WAIT, whose
 * rule is about its predecessor. */
static UChunkLoadError verify_abc_rules(MDecCtx *d, uint8_t op,
                                         uint8_t a, uint8_t b, uint8_t c,
                                         const uint32_t *instructions,
                                         size_t vi, uint8_t max_reg) {
    switch (op) {
        case OP_SELF:
            if ((unsigned)a + 1U > (unsigned)max_reg) {
                set_errmsg(d->errmsg, d->errcap,
                           "OP_SELF A+1=%u exceeds max_reg=%u at pc %zu",
                           (unsigned)a + 1U, (unsigned)max_reg, vi);
                return UCHUNK_LOAD_CORRUPT;
            }
            break;
        case OP_CALL:
            if ((unsigned)a + (unsigned)b > (unsigned)max_reg + 1U) {
                set_errmsg(d->errmsg, d->errcap,
                           "OP_CALL A+B=%u exceeds max_reg+1=%u at pc %zu"
                           " (register window overflow)",
                           (unsigned)a + (unsigned)b,
                           (unsigned)max_reg + 1U, vi);
                return UCHUNK_LOAD_CORRUPT;
            }
            break;
        case OP_INSTALL:
            /* Source, body and alternate sit in R[A], R[A+1], R[A+2]
             * whether or not the flags say the last two are present. */
            if ((unsigned)a + 2U > (unsigned)max_reg) {
                set_errmsg(d->errmsg, d->errcap,
                           "OP_INSTALL A+2=%u exceeds max_reg=%u at pc %zu",
                           (unsigned)a + 2U, (unsigned)max_reg, vi);
                return UCHUNK_LOAD_CORRUPT;
            }
            if ((c & ~(UINSTALL_F_HAS_BODY | UINSTALL_F_HAS_ALT)) != 0U
                || (b == UINSTALL_WAITUNTIL && c != 0U)) {
                set_errmsg(d->errmsg, d->errcap,
                           "OP_INSTALL flags C=0x%02x invalid for mode %u at pc %zu",
                           (unsigned)c, (unsigned)b, vi);
                return UCHUNK_LOAD_CORRUPT;
            }
            break;
        case OP_FORK:
            /* A detached child has no handle; a joined one writes its
             * handle into R[B]. */
            if ((c == UFORK_DETACH && b != 0xFFU) || (c == UFORK_JOIN && b == 0xFFU)) {
                set_errmsg(d->errmsg, d->errcap,
                           "OP_FORK mode %u with handle register B=%u at pc %zu",
                           (unsigned)c, (unsigned)b, vi);
                return UCHUNK_LOAD_CORRUPT;
            }
            break;
        case OP_JOIN_WAIT: {
            /* JOIN_WAIT's dead-child fast path reads a strand handle that
             * eager DEAD-reap may have freed; the adjacency invariant (a
             * joining FORK immediately before, FORK.B == JOIN_WAIT.A) is
             * the only pin.  Enforce it at load time so corrupt or
             * hand-built chunks cannot exploit the use-after-free. */
            uint32_t prev = (vi > 0U) ? instructions[vi - 1U] : 0U;
            if (vi == 0U || uinstr_op(prev) != OP_FORK
                || uinstr_c(prev) != UFORK_JOIN || uinstr_b(prev) != a) {
                set_errmsg(d->errmsg, d->errcap,
                           "OP_JOIN_WAIT at pc %zu not adjacent to its joining OP_FORK",
                           vi);
                return UCHUNK_LOAD_CORRUPT;
            }
            break;
        }
        case OP_SCOPE_POP: {
            uint8_t kind = (uint8_t)(a & 0x3U);
            if ((kind != USCOPE_POP_TRY && kind != USCOPE_POP_TAG)
                || ((a & USCOPE_POP_RUN_FINALLY) != 0U && kind != USCOPE_POP_TRY)) {
                set_errmsg(d->errmsg, d->errcap,
                           "OP_SCOPE_POP A=0x%02x names no valid scope kind at pc %zu",
                           (unsigned)a, vi);
                return UCHUNK_LOAD_CORRUPT;
            }
            break;
        }
        default:
            break;
    }
    return UCHUNK_LOAD_OK;
}

/* Bx range check for one ABX instruction, per its shape row. */
static UChunkLoadError verify_bx(MDecCtx *d, uint8_t op, uint16_t bx,
                                 UBxKind kind, size_t vi,
                                 size_t const_count, size_t instr_count,
                                 size_t nested_count) {
    switch (kind) {
        case UBXK_UNUSED:
        case UBXK_SYMBOL_ID:
        case UBXK_EXTARG:
        case UBXK_JUMP_SIGNED:   /* the target is resolved and range-checked by pass 2 */
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
        case UBXK_HANDLER_PC:
            if ((size_t)bx >= instr_count) {
                set_errmsg(d->errmsg, d->errcap,
                           "target-PC Bx=%u >= instr_count=%zu at pc %zu (op=%u)",
                           (unsigned)bx, instr_count, vi, (unsigned)op);
                return UCHUNK_LOAD_CORRUPT;
            }
            break;
    }
    return UCHUNK_LOAD_OK;
}

/* Pass 1: walk one proto's instructions against the opcode-shape table,
   applying the proto's bounds (max_reg / const_count / instr_count /
   nested_count / site_count).  An OP_EXTARG sets the high bits of the
   next instruction's site index; that instruction must take one. */
static UChunkLoadError verify_walk_block(MDecCtx *d,
                                          uint8_t max_reg,
                                          size_t const_count,
                                          size_t instr_count,
                                          size_t nested_count,
                                          uint16_t site_count,
                                          const uint32_t *instructions) {
    uint32_t ext = 0;
    bool ext_pending = false;
    size_t sites_seen = 0;
    size_t vi;
    for (vi = 0; vi < instr_count; vi++) {
        uint32_t ins = instructions[vi];
        uint8_t  op  = (uint8_t)uinstr_op(ins);
        if (op >= (uint8_t)OP_MAX) {
            set_errmsg(d->errmsg, d->errcap, "corrupt opcode %u at pc %zu",
                       (unsigned)op, vi);
            return UCHUNK_LOAD_CORRUPT;
        }
        if (ext_pending && !op_takes_site(op)) {
            set_errmsg(d->errmsg, d->errcap,
                       "EXTARG at pc %zu precedes op %u, which takes no site index",
                       vi - 1U, (unsigned)op);
            return UCHUNK_LOAD_BAD_EXTARG;
        }
        if (op == (uint8_t)OP_EXTARG) {
            if (vi + 1U >= instr_count) {
                set_errmsg(d->errmsg, d->errcap,
                           "EXTARG at pc %zu is the last instruction", vi);
                return UCHUNK_LOAD_BAD_EXTARG;
            }
            ext = (uint32_t)uinstr_bx(ins) << 8;
            ext_pending = true;
            continue;
        }
        const UOpcodeShape *sh = &urbi_opcode_shapes[op];

        uint8_t a = uinstr_a(ins);
        UChunkLoadError rc = verify_byte_operand(d, op, a, sh->a_kind, "A", vi, max_reg);
        if (rc != UCHUNK_LOAD_OK) return rc;

        if (sh->format == UOPF_ABC) {
            uint8_t b = uinstr_b(ins);
            uint8_t c = uinstr_c(ins);
            rc = verify_byte_operand(d, op, b, sh->b_kind, "B", vi, max_reg);
            if (rc != UCHUNK_LOAD_OK) return rc;
            rc = verify_byte_operand(d, op, c, sh->c_kind, "C", vi, max_reg);
            if (rc != UCHUNK_LOAD_OK) return rc;
            if (sh->c_kind == UOPK_IMM_SITE) {
                uint32_t site = (uint32_t)c | ext;
                if (site >= (uint32_t)site_count) {
                    set_errmsg(d->errmsg, d->errcap,
                               "site %u >= site_count %u at pc %zu (op=%u)",
                               (unsigned)site, (unsigned)site_count, vi, (unsigned)op);
                    return UCHUNK_LOAD_CORRUPT;
                }
                sites_seen++;
            }
            rc = verify_abc_rules(d, op, a, b, c, instructions, vi, max_reg);
            if (rc != UCHUNK_LOAD_OK) return rc;
        } else {
            rc = verify_bx(d, op, uinstr_bx(ins), sh->bx_kind, vi,
                           const_count, instr_count, nested_count);
            if (rc != UCHUNK_LOAD_OK) return rc;
        }
        ext = 0;
        ext_pending = false;
    }
    /* Last instruction must be OP_RET.
     *
     * Relaxation note: this strict trailing-OP_RET requirement assumes
     * the emitter always closes a chunk with an explicit return.  If a
     * future bytecode revision allows fall-through-to-end semantics
     * (e.g. an implicit RET, or a tail-call that elides RET), this
     * check will need to widen.  Every chunk the emitter currently
     * produces ends in OP_RET, so the strict form catches
     * truncated/corrupt bytecode early. */
    if (instr_count > 0U) {
        uint32_t last = instructions[instr_count - 1U];
        if (uinstr_op(last) != OP_RET) {
            set_errmsg(d->errmsg, d->errcap, "last instruction is not OP_RET");
            return UCHUNK_LOAD_CORRUPT;
        }
    }
    /* site_count must not exceed the number of site-bearing instructions.
     * Every name is keyed to an emitted slot instruction; a table that
     * claims more sites than the stream uses carries names no instruction
     * indexes, which a confused or hostile chunk could later have bound. */
    if ((size_t)site_count > sites_seen) {
        set_errmsg(d->errmsg, d->errcap,
                   "site_count=%u exceeds %zu site-bearing instructions",
                   (unsigned)site_count, sites_seen);
        return UCHUNK_LOAD_CORRUPT;
    }
    return UCHUNK_LOAD_OK;
}

static UChunkLoadError verify_proto_recursive(MDecCtx *d, const UProto *p) {
    if (p == NULL) return UCHUNK_LOAD_OK;
    UChunkLoadError rc = verify_walk_block(d,
                                            p->max_reg,
                                            p->const_count,
                                            p->instr_count,
                                            p->nested_count,
                                            p->site_count,
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

/* --- pass 2: per-sequence bounds ---
 *
 * verify_bounds_proto walks every UProto in the tree (DFS, mirroring
 * verify_proto_recursive above) and applies the checks that need
 * instruction *sequences* or cross-instruction context, which is more
 * than the per-opcode shape table in verify_walk_block can express:
 *
 *   OP_CLOSURE upvalue prelude — the nupvals pseudo-instructions that follow
 *     an OP_CLOSURE must lie within the instruction array, and each must
 *     encode a valid (in_stack, src_idx) pair:
 *       in_stack = B in {0, 1}
 *       src_idx  = C; if in_stack==1, C <= proto->max_reg (local register);
 *                     if in_stack==0, C < proto->nupvals (re-capture from parent)
 *   OP_JMP target — Bx is a signed offset biased by 32768; the resolved
 *     target must satisfy 0 <= target < instr_count.
 *   OP_CALL C low-7 — encodes nresults+1; must be >= 1 (0 means 0 results
 *     which is legal at runtime but the emitter never produces it; a
 *     hand-crafted module with C & 0x7F == 0 is malformed per the wire spec).
 *   No control transfer lands between an OP_EXTARG and the instruction it
 *     widens: neither a JMP target nor a SCOPE_TRY / SCOPE_TAG / UNWIND_TO
 *     target may be the instruction right after an EXTARG, or that
 *     instruction would run with its high site bits dropped. */
static bool lands_after_extarg(const uint32_t *ins, size_t target) {
    return target > 0U && uinstr_op(ins[target - 1U]) == OP_EXTARG;
}

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
            /* Bx encodes a signed offset biased by 32768, and the two
             * directions resolve differently -- the emitter has two
             * encoders for exactly this reason (uemit_jmp_offset /
             * uemit_jmp_offset_backward):
             *
             *   forward  (off >= 0): target = pc + off + 1
             *   backward (off <  0): target = pc + off
             *
             * because a forward offset is relative to the instruction
             * AFTER the jump while a backward one is relative to the jump
             * itself.  Resolving both with the backward rule left the
             * accepted range one short at the top: a forward jump landing
             * on exactly instr_count passed every pass and then executed
             * the uninitialised slack between instr_count and instr_cap.
             * Valid range is [0, instr_count). */
            uint16_t bx = (uint16_t)((ins >> 16) & 0xFFFFU);
            /* Compute target as signed arithmetic, guarding against underflow. */
            int64_t signed_bx  = (int64_t)bx - (int64_t)32768;
            int64_t target_i64 = (int64_t)vi + signed_bx + (signed_bx >= 0 ? 1 : 0);
            if (target_i64 < 0 || (size_t)target_i64 >= instr_count) {
                set_errmsg(d->errmsg, d->errcap,
                           "OP_JMP at pc %zu: Bx=%u resolves to target=%lld"
                           " outside [0, %zu)",
                           vi, (unsigned)bx,
                           (long long)target_i64, instr_count);
                return UCHUNK_LOAD_JMP_OUT_OF_BOUNDS;
            }
            if (lands_after_extarg(instructions, (size_t)target_i64)) {
                set_errmsg(d->errmsg, d->errcap,
                           "OP_JMP at pc %zu lands on pc %lld, right after an EXTARG",
                           vi, (long long)target_i64);
                return UCHUNK_LOAD_BAD_EXTARG;
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
        } else if (op < (uint8_t)OP_MAX
                   && urbi_opcode_shapes[op].format == UOPF_ABX
                   && urbi_opcode_shapes[op].bx_kind == UBXK_HANDLER_PC) {
            /* SCOPE_TRY / SCOPE_TAG / UNWIND_TO: pass 1 has range-checked
             * Bx against instr_count. */
            size_t target = (size_t)uinstr_bx(ins);
            if (lands_after_extarg(instructions, target)) {
                set_errmsg(d->errmsg, d->errcap,
                           "op %u at pc %zu targets pc %zu, right after an EXTARG",
                           (unsigned)op, vi, target);
                return UCHUNK_LOAD_BAD_EXTARG;
            }
        }

        vi++;
    }

    /* Recurse into nested protos (DFS, matching verify_proto_recursive order). */
    for (size_t i = 0; i < p->nested_count; i++) {
        UChunkLoadError rc = verify_bounds_proto(d, p->nested[i]);
        if (rc != UCHUNK_LOAD_OK) return rc;
    }
    return UCHUNK_LOAD_OK;
}

/* Entry point: run pass 2 from the root proto. */
UChunkLoadError urbi_chunk_verify_bounds(MDecCtx *d) {
    return verify_bounds_proto(d, d->rp);
}
