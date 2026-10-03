/* SPDX-License-Identifier: BSD-3-Clause */
/* Contains urbi_emit_expr arm helpers for the 13 leaf-expression AST kinds:
 *   AST_INT       — integer literal (OP_LOADK)
 *   AST_BOOL      — boolean literal (OP_LOADBOOL)
 *   AST_NIL       — nil literal (OP_LOADNIL)
 *   AST_NOOP      — no-op statement (OP_LOADNIL)
 *   AST_UNARY     — negation (OP_NEG)
 *   AST_BINARY    — arithmetic (OP_ADD/SUB/MUL/DIV)
 *   AST_COMPARE   — comparison → bool via 4-instruction idiom
 *   AST_IDENT     — identifier: local / upvalue / realm-global fallback
 *   AST_VAR_DECL  — variable declaration (local or chunk-top global)
 *   AST_ASSIGN    — assignment (local / upvalue / global via OP_SETSLOT)
 *   AST_SEQ       — separator list (SEP_SEMI `;` / SEP_COMMA `,` / SEP_PIPE
 *                   `|` / SEP_AMP `&`)
 *   AST_BLOCK     — braced block scope `{ ... }`
 *
 * NOTE: AST_SEQ SEP_PIPE carries a known raw next_reg-- register-
 * aliasing quirk (documented at its arm below).  AST_IDENT global-slot
 * fallback carries the freereg-sync that AST_AT_EVENT depends on.
 * AST_VAR_DECL / AST_ASSIGN carry their own known freereg-sync quirks.
 * Do NOT fix any of these here — each has a tracked disposition
 * elsewhere.
 */

#include "emit/uemit_internal.h"  /* uemit_internal.h pulls in umacros.h (urbi_zero) */
#include "emit/uintern.h"        /* ustr_intern */
#include "emit/uemit.h"
#include "chunk/uchunk.h"
#include "parse/uast.h"
#include "util/umacros.h"
#include <stddef.h>
#include <stdint.h>

/* --- AST_INT --- */

uint8_t urbi_emit_int_arm(UEmitter *e, const UAstNode *n) {
    const uint8_t r = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    const uint16_t k = urbi_emit_add_const_int(e, n->u.i);
    if (e->error != EMIT_OK) return 0U;
    urbi_emit_instr(e, uinstr_enc_abx(OP_LOADK, r, k), (uint32_t)n->line);
    return r;
}

/* --- AST_FLOAT_LIT --- */

uint8_t urbi_emit_float_arm(UEmitter *e, const UAstNode *n) {
    const uint16_t k = urbi_emit_add_const_float(e, n->u.f);
    if (e->error != EMIT_OK) return 0U;
    const uint8_t r = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    urbi_emit_instr(e, uinstr_enc_abx(OP_LOADK, r, k), (uint32_t)n->line);
    return r;
}

/* `this` resolves to the receiver object, which the OP_CALL convention
 * places in register R0 of the callee's frame.  Emits OP_MOVE dst, R0.
 *
 * Top-level `this` (fs->parent == NULL) is a v1.x feature (lobby alias);
 * raise EMIT_NO_THIS_OUTSIDE_METHOD for now. */

uint8_t urbi_emit_this_arm(UEmitter *e, const UAstNode *n) {
    const UFuncState *fs = e->current_fs;
    if (fs == NULL || fs->parent == NULL) {
        e->error = EMIT_NO_THIS_OUTSIDE_METHOD;
        urbi_emit_diag_error(e, n, "this used outside a method or nested closure");
        return 0U;
    }
    const uint8_t dst = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOAD_RECV, dst, 0U, 0U), (uint32_t)n->line);
    return dst;
}

/* --- AST_BOOL --- */

uint8_t urbi_emit_bool_arm(UEmitter *e, const UAstNode *n) {
    uint8_t r = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADBOOL, r, n->u.b ? 1U : 0U, 0U),
               (uint32_t)n->line);
    return r;
}

/* --- AST_NIL --- */

uint8_t urbi_emit_nil_arm(UEmitter *e, const UAstNode *n) {
    uint8_t r = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U),
               (uint32_t)n->line);
    return r;
}

/* --- AST_STR ---
 *
 * Phase-1 routing: parser fed us escape-resolved + concat-folded bytes in
 * the arena; we intern those bytes against the VM's per-VM intern table
 * (pointer-equal for byte-equal content) and add a UVAL_STR slot to the
 * current constant pool.  OP_LOADK loads the slot into a fresh register.
 */

uint8_t urbi_emit_string_arm(UEmitter *e, const UAstNode *n) {
    /* Intern the escape-resolved bytes; ustr_intern returns a pointer-stable
     * canonical address per (vm, content) pair.  vm is non-NULL by emitter
     * contract (uemit_init wires it). */
    const char *interned = ustr_intern(e->vm, n->u.str_lit.bytes,
                                       (size_t)n->u.str_lit.len);
    if (interned == NULL) { e->error = EMIT_OOM; return 0U; }

    const uint16_t k = urbi_emit_add_const_str(e, interned);
    if (e->error != EMIT_OK) return 0U;

    const uint8_t r = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    urbi_emit_instr(e, uinstr_enc_abx(OP_LOADK, r, k), (uint32_t)n->line);
    return r;
}

/* --- AST_NOOP --- */

uint8_t urbi_emit_noop_arm(UEmitter *e, const UAstNode *n) {
    /* No-op: load nil as the value. */
    uint8_t r = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U),
               (uint32_t)n->line);
    return r;
}

/* --- AST_UNARY --- */

uint8_t urbi_emit_unary_arm(UEmitter *e, UAstNode *n) {
    const uint8_t src_reg = urbi_emit_expr(e, n->u.unary.operand);
    if (e->error != EMIT_OK) return 0U;

    if (n->u.unary.op == UOP_NOT) {
        /* Logical NOT — the 4-instruction OP_TEST/OP_LOADBOOL branch idiom
         * (mirrors urbi_emit_compare_arm; no new opcode):
         *   TEST src, 0, 0     ; skip next when src is falsy
         *   JMP +1             ; truthy -> false arm
         *   LOADBOOL rd, 1, 1  ; rd = true; pc++ (skip false arm)
         *   LOADBOOL rd, 0, 0  ; rd = false */
        const uint8_t rd = src_reg;  /* in-place, same as NEG */
        urbi_emit_instr(e, uinstr_enc_abc(OP_TEST, src_reg, 0U, 0U),
                   (uint32_t)n->line);
        urbi_emit_instr(e, uinstr_enc_abx(OP_JMP, 0U,
                   (uint16_t)UEMIT_JMP_FALLTHROUGH_BIAS), (uint32_t)n->line);
        urbi_emit_instr(e, uinstr_enc_abc(OP_LOADBOOL, rd, 1U, 1U),
                   (uint32_t)n->line);
        urbi_emit_instr(e, uinstr_enc_abc(OP_LOADBOOL, rd, 0U, 0U),
                   (uint32_t)n->line);
        return rd;
    }

    /* UOP_NEG: arithmetic negation (parser strips unary '+' at parse time). */
    urbi_emit_instr(e, uinstr_enc_abc(OP_NEG, src_reg, src_reg, 0U),
               (uint32_t)n->line);
    return src_reg;   /* dest reuses src; no free_reg */
}

/* --- AST_BINARY --- */

uint8_t urbi_emit_binary_arm(UEmitter *e, UAstNode *n) {
    const uint8_t lhs_reg = urbi_emit_expr(e, n->u.binary.lhs);
    if (e->error != EMIT_OK) return 0U;
    const uint8_t rhs_reg = urbi_emit_expr(e, n->u.binary.rhs);
    if (e->error != EMIT_OK) return 0U;
    urbi_emit_instr(e,
               uinstr_enc_abc(urbi_emit_binop_to_opcode(n->u.binary.op),
                              lhs_reg, lhs_reg, rhs_reg),
               (uint32_t)n->line);
    free_reg(e);              /* rhs released; lhs holds result in place */
    return lhs_reg;
}

/* --- AST_COMPARE --- */

uint8_t urbi_emit_compare_arm(UEmitter *e, UAstNode *n) {
    /* Compile LHS into rb, RHS into the next register. */
    uint8_t rb = e->next_reg;
    uint8_t lhs_reg = urbi_emit_expr(e, n->u.cmp.lhs);
    if (e->error != EMIT_OK) return 0U;
    (void)lhs_reg;  /* rb == lhs_reg; named for clarity */
    uint8_t rc_reg = e->next_reg;
    uint8_t rhs_reg = urbi_emit_expr(e, n->u.cmp.rhs);
    if (e->error != EMIT_OK) return 0U;
    (void)rhs_reg;  /* rc_reg == rhs_reg */

    /* Pick opcode + A bit per operator.
       CMP_GT / CMP_GE are emitted as swapped OP_LT / OP_LE.
       The dispatch arm skips the next instruction when (result != A).
       For bool production: "skip" → LOADBOOL true. So we want skip
       when the comparison is TRUE → (result != A) must be true when
       result=true → A=0.  Exception: CMP_NEQ wants skip when eq=false
       → (false != A) true when A=1. */
    UOpcode op;
    uint8_t a_bit;
    uint8_t b_reg, c_reg;
    switch (n->u.cmp.op) {
        case CMP_EQ:  op = OP_EQ;  a_bit = 0U; b_reg = rb;     c_reg = rc_reg; break;
        case CMP_NEQ: op = OP_EQ;  a_bit = 1U; b_reg = rb;     c_reg = rc_reg; break;
        case CMP_LT:  op = OP_LT;  a_bit = 0U; b_reg = rb;     c_reg = rc_reg; break;
        case CMP_LE:  op = OP_LE;  a_bit = 0U; b_reg = rb;     c_reg = rc_reg; break;
        case CMP_GT:  op = OP_LT;  a_bit = 0U; b_reg = rc_reg; c_reg = rb;     break;
        case CMP_GE:  op = OP_LE;  a_bit = 0U; b_reg = rc_reg; c_reg = rb;     break;
        default:      e->error = EMIT_UNSUPPORTED_AST; return 0U;
    }

    /* Free LHS+RHS temps; result goes into rb. */
    e->next_reg = rb + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs != NULL) {
        if (e->next_reg > e->current_fs->max_reg_seen)
            e->current_fs->max_reg_seen = e->next_reg;
    }

    /* 4-instruction Lua-style branch idiom:
         op A, b_reg, c_reg    ; skip next if comparison matches a_bit
         JMP +1                ; jump past LOADBOOL true (→ false arm)
         LOADBOOL rb, 1, 1     ; rb = true; pc++ (skip false arm)
         LOADBOOL rb, 0, 0     ; rb = false
    */
    urbi_emit_instr(e, uinstr_enc_abc(op, a_bit, b_reg, c_reg), (uint32_t)n->line);
    urbi_emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, (uint16_t)UEMIT_JMP_FALLTHROUGH_BIAS), (uint32_t)n->line);
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADBOOL, rb, 1U, 1U), (uint32_t)n->line);
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADBOOL, rb, 0U, 0U), (uint32_t)n->line);

    return rb;
}

/* --- AST_LOGICAL — short-circuit && / || ---
 *
 * Lowers to the Lua-style OP_TESTSET + OP_JMP short-circuit idiom; no new
 * opcode.  The result lives in rd (the register the LHS landed in); when the
 * LHS settles the result the RHS is skipped entirely.
 *
 * OP_TESTSET semantics (uvm.c): `if truthy(R[B]) == C then pc++ else R[A]:=R[B]`.
 * We emit `TESTSET rd, rd, C` followed by an OP_JMP that skips the RHS:
 *
 *   for &&:  short-circuit (skip RHS, keep falsy LHS) when LHS is FALSY.
 *            We want to FALL THROUGH to RHS when LHS is truthy, i.e. pc++ on
 *            truthy → C = 1.  On falsy: R[rd]:=rd (no-op) then JMP skips RHS.
 *   for ||:  short-circuit (skip RHS, keep truthy LHS) when LHS is TRUTHY.
 *            Fall through to RHS when LHS is falsy → pc++ on falsy → C = 0.
 *
 * Register protocol mirrors urbi_emit_compare_arm: rd is a TEMP holding the
 * expression value; reset next_reg to rd+1 and bump max_reg_seen so callers
 * can allocate above the result. */
uint8_t urbi_emit_logical_arm(UEmitter *e, UAstNode *n) {
    /* Evaluate LHS into rd (the result register). */
    uint8_t rd = e->next_reg;
    uint8_t lhs_reg = urbi_emit_expr(e, n->u.logical.lhs);
    if (e->error != EMIT_OK) return 0U;
    (void)lhs_reg;  /* rd == lhs_reg */

    /* TESTSET rd, rd, c — see polarity reasoning above. */
    const uint8_t c = n->u.logical.is_or ? 0U : 1U;
    urbi_emit_instr(e, uinstr_enc_abc(OP_TESTSET, rd, rd, c), (uint32_t)n->line);

    /* JMP placeholder — when taken, skips the RHS evaluation (short-circuit). */
    int jmp_skip = emit_fwd_jmp(e, (uint32_t)n->line);

    /* RHS path: evaluate RHS starting at rd (reset cursor so RHS reuses the
     * temp zone above rd), then move the value into rd if it landed elsewhere. */
    e->next_reg = rd;
    uint8_t rhs_reg = urbi_emit_expr(e, n->u.logical.rhs);
    if (e->error != EMIT_OK) return 0U;
    if (rhs_reg != rd) {
        urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, rd, rhs_reg, 0U), (uint32_t)n->line);
    }

    /* Patch the short-circuit JMP to land just past the RHS path. */
    patch_fwd_jmp_here(e, jmp_skip);

    /* Result is in rd; free the RHS temps. */
    e->next_reg = rd + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs != NULL) {
        if (e->next_reg > e->current_fs->max_reg_seen)
            e->current_fs->max_reg_seen = e->next_reg;
    }
    return rd;
}

/* --- AST_IDENT --- */

uint8_t urbi_emit_ident_arm(UEmitter *e, const UAstNode *n) {
    if (e->vm == NULL || e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    const char *canonical = ustr_intern(e->vm, n->u.ident.start,
                                        (size_t)n->u.ident.len);
    if (canonical == NULL) {
        e->error = EMIT_OOM;
        return 0U;
    }

    /* Local lookup — scan active locals from innermost to outermost. */
    UFuncState *fs = e->current_fs;
    int slot = -1;
    bool is_lazy_local = false;
    for (int i = fs->nactvar - 1; i >= 0; i--) {
        if (fs->actvars[i].name == canonical) {
            slot = (int)fs->actvars[i].slot;
            is_lazy_local = fs->actvars[i].is_lazy;
            break;
        }
    }
    if (slot >= 0) {
        uint8_t dst = e->next_reg;
        if (dst >= (uint8_t)(UFS_MAX_REGS - 1)) {
            e->error = EMIT_REG_EXHAUSTED;
            return 0U;
        }
        urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, dst, (uint8_t)slot, 0U),
                   (uint32_t)n->line);
        e->next_reg++;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        if (e->next_reg > fs->max_reg_seen) fs->max_reg_seen = e->next_reg;

        /* Implicit force for lazy locals (spec §4.1).
         * Skip when lazy_arg_context is set — caller is passing this
         * value as a call argument (pass-through, spec §4.2). */
        if (is_lazy_local && !e->lazy_arg_context) {
            /* dst currently holds the thunk closure.  Force it:
             * OP_CALL dst, 1, 2 — zero args, 1 result, result in dst. */
            urbi_emit_instr(e, uinstr_enc_abc(OP_CALL, dst, 1U, 2U),
                       (uint32_t)n->line);
        }
        return dst;
    }

    /* Upvalue cascade. */
    int up = urbi_vm_find_or_install_upvalue(e, fs, canonical, n->u.ident.len);
    if (up >= 0) {
        uint8_t dst = e->next_reg;
        if (dst >= (uint8_t)(UFS_MAX_REGS - 1)) {
            e->error = EMIT_REG_EXHAUSTED;
            return 0U;
        }
        urbi_emit_instr(e, uinstr_enc_abc(OP_GETUPVAL, dst, (uint8_t)up, 0U),
                   (uint32_t)n->line);
        e->next_reg++;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        if (e->next_reg > fs->max_reg_seen) fs->max_reg_seen = e->next_reg;
        return dst;
    }

    /* Realm-global fallback (spec #5 §5.1).
     * The identifier did not resolve as a local or upvalue; fall through
     * to the realm's global_object slot table via OP_GETSLOT.
     *
     * r_global_slot is claimed at most once per function.  For nested
     * function bodies it is pre-reserved above the last param in
     * emit_function_body_impl (global_slot_reserved = true) so that
     * if/while temp-resets cannot clobber it even if the first global
     * reference appears inside a branch arm.  For top-level functions it
     * is claimed lazily here on first use. */
    if (!fs->references_global) {
        if (!fs->global_slot_reserved) {
            /* Top-level lazy path: claim r_global_slot at current freereg. */
            if (fs->freereg >= (uint8_t)(UFS_MAX_REGS - 1)) {
                e->error = EMIT_REG_EXHAUSTED;
                return 0U;
            }
            fs->r_global_slot = fs->freereg;
            fs->global_slot_reserved = true;
            fs->freereg++;
            if (fs->freereg > fs->max_reg_seen)
                fs->max_reg_seen = fs->freereg;
            /* Sync the emitter's temp cursor upward — the claimed slot must
             * not be overwritten by subsequent temp allocations. */
            if (fs->freereg > e->next_reg) {
                e->next_reg = fs->freereg;
                if (e->next_reg > e->max_reg_seen)
                    e->max_reg_seen = e->next_reg;
            }
        }
        /* Mark first actual global read (for nested functions the slot
         * was pre-reserved but references_global starts false). */
        fs->references_global = true;
        /* OP_LOAD_REALM_GLOBAL is emitted as a function prologue by the
         * frame finalizer (uemit_close_function, ), not inline here.
         * The register stays stable (local-zone floor) for the remainder
         * of the function body regardless of when the first reference
         * appears — including inside branch arms that may not execute. */
    }
    {
        uint8_t dst = e->next_reg;
        if (dst >= (uint8_t)(UFS_MAX_REGS - 1)) {
            e->error = EMIT_REG_EXHAUSTED;
            return 0U;
        }
        int ic_idx = uemit_assign_ic_index(e, (USymbol *)canonical);
        if (ic_idx < 0) return 0U;  /* error already set */
        urbi_emit_instr(e, uinstr_enc_abc(OP_GETSLOT, dst, fs->r_global_slot,
                                     (uint8_t)ic_idx),
                   (uint32_t)n->line);
        e->next_reg++;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        if (e->next_reg > fs->max_reg_seen) fs->max_reg_seen = e->next_reg;
        return dst;
    }
}

/* --- AST_VAR_DECL --- */

uint8_t urbi_emit_var_decl_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    UFuncState *fs = e->current_fs;

    /* Intern the variable name. */
    const char *canonical = ustr_intern(e->vm, n->u.var_decl.name_start,
                                        (size_t)n->u.var_decl.name_len);
    if (canonical == NULL) { e->error = EMIT_OOM; return 0U; }

    /* === Chunk-top path (spec #5 §5.2): write to realm global slot ===
     *
     * When this function is at chunk-top (no enclosing function), `var x`
     * declares a realm global rather than a frame local.  Emit:
     *   OP_SETSLOT  init_reg, r_global_slot, ic_idx
     * where ic_idx is the IC site for the slot name.
     *
     * The emit prologue fills r_global_slot with realm->global_object at
     * function entry; the OP_SETSLOT then writes `init_value` into
     * the correct slot on the global object.
     */
    if (fs->parent == NULL && fs->nblocks == 0) {
        /* Reserve r_global_slot on first global use (same as ).
         * Uses the same global_slot_reserved / references_global two-flag
         * protocol as the AST_IDENT global fallback. */
        if (!fs->references_global) {
            if (!fs->global_slot_reserved) {
                if (fs->freereg >= (uint8_t)(UFS_MAX_REGS - 1)) {
                    e->error = EMIT_REG_EXHAUSTED;
                    return 0U;
                }
                fs->r_global_slot = fs->freereg;
                fs->global_slot_reserved = true;
                fs->freereg++;
                if (fs->freereg > fs->max_reg_seen)
                    fs->max_reg_seen = fs->freereg;
                e->next_reg = fs->freereg;
                if (e->next_reg > e->max_reg_seen)
                    e->max_reg_seen = e->next_reg;
            }
            fs->references_global = true;
            /* OP_LOAD_REALM_GLOBAL is prepended as a function prologue by
             * uemit_close_function — not emitted inline here. */
        }

        /* Emit init expression into a temp register. */
        uint8_t init_reg = urbi_emit_expr(e, n->u.var_decl.init);
        if (e->error != EMIT_OK) return 0U;

        /* Intern slot name and assign IC index. */
        int ic_idx = uemit_assign_ic_index(e, (USymbol *)canonical);
        if (ic_idx < 0) return 0U;

        /* Write value into the global slot. */
        urbi_emit_instr(e, uinstr_enc_abc(OP_SETSLOT, init_reg, fs->r_global_slot,
                                     (uint8_t)ic_idx),
                   (uint32_t)n->line);

        /* Track the declared global name so that subsequent AST_ASSIGN
         * nodes (e.g. `n = n + 1` after `var n = 0` at chunk-top) can
         * route to the global slot rather than raising EMIT_UNRESOLVED_NAME.
         * Also record the function signature (global_var_sigs) so that
         * lazy-arg wrapping works at call sites that reference globals. */
        if (fs->n_global_vars < UFS_MAX_LOCALS) {
            int gidx = fs->n_global_vars++;
            fs->global_var_names[gidx] = canonical;
            UFuncSig *gsig = &fs->global_var_sigs[gidx];
            urbi_zero(gsig, sizeof(*gsig));
            if (n->u.var_decl.init->kind == AST_FUNCTION) {
                UAstNode *fn = n->u.var_decl.init;
                gsig->resolved  = true;
                gsig->nparams   = fn->u.func.param_count;
                {
                    int pi;
                    for (pi = 0; pi < fn->u.func.param_count && pi < 16; pi++) {
                        gsig->param_is_lazy[pi] =
                            fn->u.func.params[pi]->u.param.is_lazy;
                    }
                }
            }
        }

        /* Return init_reg as the expression value (the REPL displays it).
         * Do NOT call free_reg here: the caller (NARY separator or
         * uemit_statement) releases temps via next_reg = freereg reset.
         * This is consistent with the local var-decl path which absorbs
         * the temp into the local zone without freeing it. */
        return init_reg;
    }

    /* === Normal path: allocate a frame local === */

    /* Redeclare check within current block (or whole actvar table). */
    int search_from = (fs->nblocks > 0)
        ? fs->blocks[fs->nblocks - 1].first_local_idx
        : 0;
    for (int i = search_from; i < fs->nactvar; i++) {
        if (fs->actvars[i].name == canonical) {
            e->error = EMIT_LOCAL_REDECLARE;
            urbi_emit_diag_error(e, n, "variable '%.*s' already declared in this scope",
                            n->u.var_decl.name_len, n->u.var_decl.name_start);
            return 0U;
        }
    }
    if (fs->nactvar >= UFS_MAX_LOCALS) {
        e->error = EMIT_REG_EXHAUSTED;
        urbi_emit_diag_error(e, n, "too many local variables in function (max %d)",
                        UFS_MAX_LOCALS);
        return 0U;
    }

    /* Record where the init will land: current top-of-stack register.
       The init expression is emitted at this slot via alloc_reg(). */
    uint8_t reg_before = e->next_reg;

    /* Emit init expression — lands at reg_before (alloc_reg gives it
       the next free slot, which is e->next_reg == reg_before). */
    uint8_t init_reg = urbi_emit_expr(e, n->u.var_decl.init);
    if (e->error != EMIT_OK) return 0U;

    /* Sanity: init must have landed at exactly reg_before. */
    if (init_reg != reg_before) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    /* Absorb the temp into the local zone: register at reg_before is
       now the local's permanent slot. Do NOT free the register. */
    int local_idx = fs->nactvar;
    ULocalVar *lv = &fs->actvars[fs->nactvar++];
    lv->name       = canonical;
    lv->name_len   = n->u.var_decl.name_len;
    lv->slot       = reg_before;
    lv->is_captured = false;
    lv->is_lazy    = false;

    /* If init is a literal function, record its lazy-param signature
     * so the call-site emit can wrap args correctly (spec §2.2). */
    if (n->u.var_decl.init->kind == AST_FUNCTION) {
        UAstNode *fn = n->u.var_decl.init;
        UFuncSig *sig = &fs->actvar_sigs[local_idx];
        sig->resolved = true;
        sig->nparams = fn->u.func.param_count;
        {
            int pi;
            for (pi = 0; pi < fn->u.func.param_count && pi < 16; pi++) {
                sig->param_is_lazy[pi] =
                    fn->u.func.params[pi]->u.param.is_lazy;
            }
        }
    }

    /* Sync freereg: it now equals e->next_reg (one past the local's slot).
       The local occupies [reg_before]; e->next_reg is already reg_before+1. */
    fs->freereg = e->next_reg;
    if (fs->freereg > fs->max_reg_seen) fs->max_reg_seen = fs->freereg;

    /* var-decl "returns" the value in its slot (for use as an expression
       in separator chains). Caller can free_reg as normal; the slot
       remains because it is now a local (tracked by nactvar). */
    return init_reg;
}

/* --- AST_ASSIGN --- */

uint8_t urbi_emit_assign_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    UFuncState *fs = e->current_fs;

    const char *canonical = ustr_intern(e->vm, n->u.assign.name_start,
                                        (size_t)n->u.assign.name_len);
    if (canonical == NULL) { e->error = EMIT_OOM; return 0U; }

    /* Resolve target: local first. */
    int local_slot = -1;
    for (int i = fs->nactvar - 1; i >= 0; i--) {
        if (fs->actvars[i].name == canonical) {
            /* Reject assignment to lazy parameter (spec §4.5). */
            if (fs->actvars[i].is_lazy) {
                e->error = EMIT_LAZY_PARAM_ASSIGN;
                urbi_emit_diag_error(e, n, "cannot assign to lazy parameter '%.*s'",
                                n->u.assign.name_len, n->u.assign.name_start);
                return 0U;
            }
            local_slot = (int)fs->actvars[i].slot;
            break;
        }
    }

    int upvalue_idx = -1;
    bool is_global_assign = false;
    if (local_slot < 0) {
        upvalue_idx = urbi_vm_find_or_install_upvalue(e, fs, canonical,
                                              n->u.assign.name_len);
        if (upvalue_idx < 0) {
            /* Neither a local nor an upvalue: the target is a realm
             * global.  This is the WRITE half of legacy top-level
             * scoping, and it deliberately does not require the name to
             * have been declared in THIS chunk -- the REPL compiles one
             * chunk per line, so `var x = 5` and `x = 9` almost never
             * share a compilation unit, and a fork thunk writing a
             * chunk-top var is a nested funcstate that never saw the
             * declaration either.
             *
             * Whether the slot exists is the runtime's question, not the
             * compiler's: OP_SETSLOT on the realm globals behaves
             * exactly as the explicit `Realm.x = v` form does. */
            is_global_assign = true;
            if (!urbi_emit_reserve_global_slot(e)) return 0U;
        }
    }

    /* Emit RHS into top temp. */
    uint8_t reg_before = e->next_reg;
    uint8_t rhs_reg = urbi_emit_expr(e, n->u.assign.value);
    if (e->error != EMIT_OK) return 0U;

    /* Move into the target slot. */
    if (local_slot >= 0) {
        urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, (uint8_t)local_slot,
                                     rhs_reg, 0U),
                   (uint32_t)n->line);
    } else if (is_global_assign) {
        /* Write to the global slot on the realm object.
         * When called from a thunk (fs->parent != NULL) the thunk's own
         * r_global_slot is pre-reserved by urbi_emit_function_literal; marking
         * references_global here causes uemit_close_function to prepend the
         * OP_LOAD_REALM_GLOBAL prologue so the register is live at runtime. */
        if (!fs->references_global) {
            fs->references_global = true;
        }
        int ic_idx = uemit_assign_ic_index(e, (USymbol *)canonical);
        if (ic_idx < 0) return 0U;
        /* SETSLOT_UPDATE, not SETSLOT: a bare `x = v` rebinds an existing
         * global and raises LookupError when the name resolves nowhere.
         * Only `var x = v` (the declaration arm above) and the explicit
         * `Realm.x = v` member form declare one. */
        urbi_emit_instr(e, uinstr_enc_abc(OP_SETSLOT_UPDATE, rhs_reg, fs->r_global_slot,
                                     (uint8_t)ic_idx),
                   (uint32_t)n->line);
        /* If RHS is a literal function, update global_var_sigs so
         * subsequent calls to this global get correct lazy-arg wrapping.
         * At chunk-top (fs->n_global_vars > 0) this updates the root's
         * sigs; inside a thunk (fs->n_global_vars == 0) the loop is a
         * no-op (root sigs were set by the original var declaration). */
        for (int gi = 0; gi < fs->n_global_vars; gi++) {
            if (fs->global_var_names[gi] == canonical) {
                UFuncSig *gsig = &fs->global_var_sigs[gi];
                urbi_zero(gsig, sizeof(*gsig));
                if (n->u.assign.value->kind == AST_FUNCTION) {
                    UAstNode *fn = n->u.assign.value;
                    gsig->resolved = true;
                    gsig->nparams  = fn->u.func.param_count;
                    {
                        int pi;
                        for (pi = 0; pi < fn->u.func.param_count && pi < 16; pi++) {
                            gsig->param_is_lazy[pi] =
                                fn->u.func.params[pi]->u.param.is_lazy;
                        }
                    }
                }
                break;
            }
        }
    } else {
        urbi_emit_instr(e, uinstr_enc_abc(OP_SETUPVAL, rhs_reg,
                                     (uint8_t)upvalue_idx, 0U),
                   (uint32_t)n->line);
    }
    /* Free the temp — it was only needed for the RHS. */
    e->next_reg = reg_before;
    /* Assignment expression value is the target slot's value; return it. */
    return (local_slot >= 0) ? (uint8_t)local_slot : reg_before;
}

/* --- AST_SEQ --- */

/* SEP_PIPE: run every child but the last purely for side effect, in
 * program order, then the last child's value is the SEQ's value.  A
 * `|`-chain used to nest as `((c0|c1)|c2)...`; since each level just
 * discards lhs and forwards rhs, running every child but the last in a
 * flat loop produces the identical observable result.
 *
 * Sync next_reg to the FuncState freereg before each next child.  The
 * bare next_reg-- is wrong when a child leaves freereg promoted (e.g. a
 * child ending in a function literal, which lifts freereg via
 * urbi_emit_function_literal's `freereg++`); after next_reg-- the
 * cursor would sit BELOW freereg and the next child's allocation would
 * clobber a still-live temp. */
static uint8_t emit_seq_pipe(UEmitter *e, UAstNode **children, int count) {
    uint8_t r = 0U;
    for (int i = 0; i < count; i++) {
        r = urbi_emit_expr(e, children[i]);
        if (e->error != EMIT_OK) return 0U;
        if (i < count - 1) e->next_reg = e->current_fs->freereg;
    }
    return r;
}

/* SEP_AMP: `&` fork-join (closure-spawn), left-nested exactly like the
 * separator's old pairwise form: `((c0&c1)&c2)&c3` forks c1 and joins it
 * while c0 runs inline, THEN forks c2 and joins it while that whole
 * construct runs inline, and so on.  Recursing over the range
 * `children[0 .. range_count-2]` as the "run inline" side reproduces
 * this fold without reintroducing a binary AST node.
 *
 * Spec §3 row 4 + §3.2 + §7.2: spawn the range's last child as a child
 * strand, wait for it to complete, then produce void as the result.
 *
 * Emit sequence per level:
 *   1. Compile the range's last child to a closure (thunk) → closure_reg.
 *   2. Compile every child before it inline (recursing when there is
 *      more than one left), the parent strand continuing.
 *   3. OP_FORK_JOIN  A=closure_reg  B=child_reg  → spawns + stores handle.
 *   4. OP_JOIN_WAIT  A=child_reg                 → block until child DEAD.
 *   5. OP_LOADVOID   A=result_reg                → result is void (spec §7.2).
 */
static uint8_t emit_seq_amp(UEmitter *e, UAstNode **children, int range_count, uint32_t line) {
    if (range_count == 1) return urbi_emit_expr(e, children[0]);

    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    /* Step 1: compile the range's last child to a closure. */
    uint8_t closure_reg = urbi_emit_lazy_thunk(e, children[range_count - 1]);
    if (e->error != EMIT_OK) return 0U;

    /* Step 1b: adopt closure_reg as a hidden DECLARED local for the
     * duration of the inline compile below.  The closure register must
     * stay live until OP_FORK_JOIN reads it, but statement emitters
     * inside that range reset freereg to urbi_emit_fs_temp_floor() — and
     * the floor is COUNT-based (nactvar + global_slot_reserved), so a raw
     * temp below subsequently-declared locals breaks the math twice over:
     * the emitter's next LOADNIL/temp lands on the closure register
     * (runtime TypeError from OP_FORK_JOIN), and any local declared
     * inside the range sits one slot above its counted position, so later
     * floor resets clobber IT instead (silent wrong values).  Declaring
     * the slot makes every urbi_emit_fs_temp_floor reset inside the range
     * — present and future emitters alike — land above it.  Same
     * discipline as for-each's \x01iter and switch's \x01sw hidden
     * locals.  Guard: adopt only when the closure landed exactly at the
     * local-zone top, so the count formula stays exact; otherwise fall
     * through to the historical raw-temp behavior. */
    bool fork_slot_adopted = false;
    if (e->vm != NULL &&
        closure_reg == urbi_emit_fs_temp_floor(e->current_fs) &&
        e->current_fs->nactvar < UFS_MAX_LOCALS) {
        const char *fork_name = ustr_intern(e->vm, "\x01fork", 5);
        if (fork_name == NULL) { e->error = EMIT_OOM; return 0U; }
        /* Write actvars directly instead of uemit_declare_local: that
         * helper allocates a FRESH slot at freereg and bumps freereg,
         * but the closure already occupies the floor-top register, so we
         * adopt closure_reg as the slot and leave freereg untouched.  The
         * \x01 sentinel name can't collide with any user identifier, so
         * the redeclare scan is skipped too; the adoption is temporary
         * (undone by nactvar-- below), touching no scope-block
         * bookkeeping. */
        ULocalVar *lv = &e->current_fs->actvars[e->current_fs->nactvar];
        lv->name        = fork_name;
        lv->name_len    = 5;
        lv->slot        = closure_reg;
        lv->is_captured = false;
        lv->is_lazy     = false;
        e->current_fs->nactvar++;
        fork_slot_adopted = true;
    }

    /* Step 2: compile everything before the last child inline; release
     * its register after. */
    uint8_t lhs_save = e->next_reg;
    uint8_t lhs_r = emit_seq_amp(e, children, range_count - 1, line);
    if (e->error != EMIT_OK) {
        if (fork_slot_adopted) e->current_fs->nactvar--;
        return 0U;
    }
    (void)lhs_r;
    /* Restore next_reg to above freereg after the inline range (keep
     * closure_reg alive). */
    if (e->next_reg > e->current_fs->freereg &&
        e->next_reg > lhs_save)
        e->next_reg = lhs_save;
    (void)lhs_save;

    /* Un-adopt: the remaining instructions of this level are emitted
     * right here with no floor resets, so the protection window ends
     * with the inline range.  All its blocks have closed, so the
     * adopted entry is back on top of actvars. */
    if (fork_slot_adopted) e->current_fs->nactvar--;

    /* Step 3: OP_FORK_JOIN A=closure_reg B=child_reg. */
    uint8_t child_reg = e->next_reg;
    if (child_reg >= (uint8_t)(UFS_MAX_REGS - 1)) {
        e->error = EMIT_REG_EXHAUSTED;
        return 0U;
    }
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;
    urbi_emit_instr(e, uinstr_enc_abc(OP_FORK_JOIN, closure_reg, child_reg, 0U), line);
    if (e->error != EMIT_OK) return 0U;

    /* Step 4: OP_JOIN_WAIT A=child_reg. */
    urbi_emit_instr(e, uinstr_enc_abc(OP_JOIN_WAIT, child_reg, 0U, 0U), line);
    if (e->error != EMIT_OK) return 0U;

    /* Step 5: OP_LOADVOID into result_reg (`&` result is void). */
    uint8_t result_reg = e->current_fs->freereg;
    if (result_reg >= (uint8_t)(UFS_MAX_REGS - 1)) {
        e->error = EMIT_REG_EXHAUSTED;
        return 0U;
    }
    e->current_fs->freereg++;
    if (e->current_fs->freereg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->current_fs->freereg;
    e->next_reg = e->current_fs->freereg;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADVOID, result_reg, 0U, 0U), line);
    return result_reg;
}

uint8_t urbi_emit_seq_arm(UEmitter *e, UAstNode *n) {
    if (n->u.seq.separator == SEP_PIPE) {
        return emit_seq_pipe(e, n->u.seq.children, n->u.seq.count);
    }
    if (n->u.seq.separator == SEP_AMP) {
        return emit_seq_amp(e, n->u.seq.children, n->u.seq.count, (uint32_t)n->line);
    }
    if (n->u.seq.separator == SEP_COMMA) {
        /* `,` parallel semantics (closure-spawn).
         *
         * Spec §3 row 3: each child runs in parallel — last child's value
         * is the expression's value.  closure-spawn: children 0..count-2
         * are compiled to closures (capturing surrounding locals via upvalue
         * cascade) and spawned as detached strands via OP_FORK_DETACH.
         * The last child runs inline as the parent's continuation.
         */
        if (e->current_fs == NULL) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0U;
        }
        int i;
        for (i = 0; i < n->u.seq.count - 1; i++) {
            /* Compile child[i] as a zero-arg closure (thunk). */
            uint8_t closure_reg = urbi_emit_lazy_thunk(e, n->u.seq.children[i]);
            if (e->error != EMIT_OK) return 0U;
            /* OP_FORK_DETACH A=closure_reg: spawn detached strand. */
            urbi_emit_instr(e, uinstr_enc_abc(OP_FORK_DETACH, closure_reg, 0U, 0U),
                       (uint32_t)n->u.seq.children[i]->line);
            if (e->error != EMIT_OK) return 0U;
            /* Release the closure register (temp). */
            if (e->next_reg > e->current_fs->freereg)
                e->next_reg = e->current_fs->freereg;
        }
        /* Last child runs inline; its result is the SEQ's value. */
        uint8_t r = urbi_emit_expr(e, n->u.seq.children[n->u.seq.count - 1]);
        if (e->error != EMIT_OK) return 0U;
        return r;
    }
    /* SEP_SEMI: compile each child; OP_YIELD between children (not
       before first, not after last); release temp regs between.
       Last child's result register is the SEQ's value.

       Between children, reset next_reg to freereg (first slot above
       all declared locals) rather than blindly decrementing.  The
       decrement-by-one pattern is wrong when a var-decl child has
       promoted a temp into a permanent local (advancing freereg), or
       when a non-var child needed more than one temp: both cases leave
       freereg ahead of where a simple decrement would land. */
    uint8_t r = 0U;
    for (int i = 0; i < n->u.seq.count; i++) {
        if (i > 0) {
            /* Release all temps allocated by the previous child.
             *
             * Bug fix 2026-05-16 (urbiscript-scan stress test, eye_demo
             * BlobScan.scan): the previous reset was just
             *     e->next_reg = e->current_fs->freereg;
             * which kept whatever freereg the previous child left behind.
             * But urbi_emit_call_arm for a discarded-result bare call (e.g.
             * `c_scan_begin();`) leaves freereg ABOVE the local-zone
             * floor (one past the call result), so subsequent var-decl
             * children get a slot at the drifted next_reg.  urbi_emit_var_decl_arm
             * increments nactvar by 1 but assigns lv->slot = drifted-slot;
             * urbi_emit_fs_temp_floor (count-based: nactvar + r_global_slot) then
             * UNDERESTIMATES the true local-zone top by the drift amount.
             * On the next urbi_emit_while_arm body open, freereg gets reset to
             * urbi_emit_fs_temp_floor → lands BELOW already-declared locals → the
             * loop body's `var x = 0` aliases the outer `iters` register,
             * and runtime x/y iterate in lockstep through the diagonal.
             *
             * Mirror urbi_emit_block_arm's reset pattern: drop freereg back to
             * urbi_emit_fs_temp_floor so the next child sees a clean local-zone
             * boundary, regardless of what kind of expression the
             * previous child was. */
            e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
            e->next_reg = e->current_fs->freereg;
            /* A cleanup body is ordinary code, so `;` yields there like
             * anywhere else.  The old core suppressed this because a
             * finally ran as a nested VM invocation and a yield inside one
             * was read as body completion; the re-founded walker runs a
             * finally as a continuation of the same frame, holding the
             * suspended unwind in a cleanup marker that OP_RESUME hands
             * back, so a yield mid-body is just a yield. */
            urbi_emit_instr(e, uinstr_enc_abc(OP_YIELD, 0U, 0U, 0U),
                       e->prev_line);
            if (e->error != EMIT_OK) return 0U;
        }
        r = urbi_emit_expr(e, n->u.seq.children[i]);
        if (e->error != EMIT_OK) return 0U;
    }
    return r;
}

/* --- AST_BLOCK --- */

uint8_t urbi_emit_block_arm(UEmitter *e, UAstNode *n) {
    /* Scoped sequence of statements inside `{ }`.
       Opens a block scope so locals declared inside don't outlive the
       block.  The block's "value" is the last statement's result reg
       (or nil if empty).  Temps are reset between statements. */
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    if (!uemit_open_block(e, false)) return 0U;

    /* Adopt every register already live above the local-zone floor as a
     * hidden declared local for the duration of this block.  A block
     * used anywhere but as a bare top-level statement (the parser's
     * subscript/list-literal desugars can put one in arbitrary
     * expression position) can be entered with an ENCLOSING
     * expression's pending temporaries still above the floor — a
     * call's callee, a binary's LHS, an earlier element of a list
     * literal under construction.  urbi_emit_fs_temp_floor() only knows
     * about declared locals (it is nactvar-based), so without this the
     * between-statement floor reset below would hand those live
     * registers to this block's OWN temporaries and silently corrupt
     * them.  Same trick as emit_seq_amp's closure-register adoption
     * above; uemit_close_block restores nactvar from the snapshot it
     * took at open, above, regardless of how this function returns, so
     * no matching decrement is needed here. */
    if (e->vm != NULL) {
        uint8_t floor_before = urbi_emit_fs_temp_floor(e->current_fs);
        for (uint8_t reg = floor_before; reg < e->next_reg; reg++) {
            if (e->current_fs->nactvar >= UFS_MAX_LOCALS) {
                e->error = EMIT_REG_EXHAUSTED;
                uemit_close_block(e);
                return 0U;
            }
            const char *hidden_name = ustr_intern(e->vm, "\x01blk", 4);
            if (hidden_name == NULL) {
                e->error = EMIT_OOM;
                uemit_close_block(e);
                return 0U;
            }
            ULocalVar *lv = &e->current_fs->actvars[e->current_fs->nactvar];
            lv->name        = hidden_name;
            lv->name_len    = 4;
            lv->slot        = reg;
            lv->is_captured = false;
            lv->is_lazy     = false;
            e->current_fs->nactvar++;
        }
    }

    uint8_t r = 0U;
    for (int i = 0; i < n->u.block.count; i++) {
        r = urbi_emit_expr(e, n->u.block.stmts[i]);
        if (e->error != EMIT_OK) {
            uemit_close_block(e);
            return 0U;
        }
        if (i < n->u.block.count - 1) {
            /* Release temps between statements; locals stay. */
            e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
            e->next_reg = e->current_fs->freereg;
        }
    }

    if (!uemit_close_block(e)) return 0U;
    return r;
}
