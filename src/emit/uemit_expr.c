/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_expr.c — leaf-expression bytecode emitters.
 * Extracted from uemit.c during v0.5.4-decompose (EMIT-045 #3).
 *
 * Contains emit_expr arm helpers for the 13 leaf-expression AST kinds:
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
 *   AST_NARY      — separator list (SEP_SEMI `;` / SEP_COMMA `,`)
 *   AST_BIN_SEP   — binary separator (`|` pipe / `&` fork-join)
 *   AST_BLOCK     — braced block scope `{ ... }`
 *
 * NOTE: AST_BIN_SEP SEP_PIPE carries EMIT-009 (raw next_reg-- pattern).
 * AST_IDENT global-slot fallback carries the freereg-sync that AST_AT_EVENT
 * depends on.  AST_VAR_DECL / AST_ASSIGN carry EMIT-020/021/027.
 * Do NOT fix any of these here — they are wave-5/wave-6 dispositions. */

#include "emit/uemit_internal.h"  /* uemit_internal.h pulls in umacros.h (urbi_zero) */
#include "value/uintern.h"        /* ustr_intern */
#include "emit/uemit.h"
#include "chunk/uchunk.h"
#include "parse/uast.h"
#include "runtime/umacros.h"
#include <stddef.h>
#include <stdint.h>

/* --- AST_INT --- */

uint8_t emit_int_arm(UEmitter *e, const UAstNode *n) {
    const uint8_t r = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    const uint16_t k = add_const_int(e, n->u.i);
    if (e->error != EMIT_OK) return 0U;
    emit_instr(e, uinstr_enc_abx(OP_LOADK, r, k), (uint32_t)n->line);
    return r;
}

/* --- AST_FLOAT_LIT --- */

uint8_t emit_float_arm(UEmitter *e, const UAstNode *n) {
    const uint16_t k = add_const_float(e, n->u.f);
    if (e->error != EMIT_OK) return 0U;
    const uint8_t r = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    emit_instr(e, uinstr_enc_abx(OP_LOADK, r, k), (uint32_t)n->line);
    return r;
}

/* --- AST_THIS (v0.6.2 Phase 2 — Gap #3) ---
 *
 * `this` resolves to the receiver object, which the OP_CALL convention
 * places in register R0 of the callee's frame.  Emits OP_MOVE dst, R0.
 *
 * Top-level `this` (fs->parent == NULL) is a v1.x feature (lobby alias);
 * raise EMIT_NO_THIS_OUTSIDE_METHOD for now. */

uint8_t emit_this_arm(UEmitter *e, const UAstNode *n) {
    const UFuncState *fs = e->current_fs;
    if (fs == NULL || fs->parent == NULL) {
        e->error = EMIT_NO_THIS_OUTSIDE_METHOD;
        urbi_emit_diag_error(e, n, "this used outside a method or nested closure");
        return 0U;
    }
    const uint8_t dst = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    /* OP_LOAD_RECV loads the receiver saved in the call frame at dispatch
     * time (UCallFrame.recv ← R[A+1] of the calling OP_CALL when its C
     * carries the method flag, v1.6 S42).  Stable across any GETSLOT /
     * SELF / CALL inside the method body since the value is held in the
     * frame record, not a global. */
    emit_instr(e, uinstr_enc_abc(OP_LOAD_RECV, dst, 0U, 0U), (uint32_t)n->line);
    return dst;
}

/* --- AST_BOOL --- */

uint8_t emit_bool_arm(UEmitter *e, const UAstNode *n) {
    uint8_t r = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    emit_instr(e, uinstr_enc_abc(OP_LOADBOOL, r, n->u.b ? 1U : 0U, 0U),
               (uint32_t)n->line);
    return r;
}

/* --- AST_NIL --- */

uint8_t emit_nil_arm(UEmitter *e, const UAstNode *n) {
    uint8_t r = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U),
               (uint32_t)n->line);
    return r;
}

/* --- AST_STR ---
 *
 * Phase-1 routing: parser fed us escape-resolved + concat-folded bytes in
 * the arena; we intern those bytes against the VM's per-VM intern table
 * (pointer-equal for byte-equal content) and add a UVAL_STR slot to the
 * current constant pool.  OP_LOADK loads the slot into a fresh register.
 *
 * Closes the v0.5.6 MOD-008 reservation of the UVAL_STR constant-pool kind
 * (see src/chunk/uchunk_io.c constant-pool decoder for the symmetric load
 * arm). */

uint8_t emit_string_arm(UEmitter *e, const UAstNode *n) {
    /* Intern the escape-resolved bytes; ustr_intern returns a pointer-stable
     * canonical address per (vm, content) pair.  vm is non-NULL by emitter
     * contract (uemit_init wires it). */
    const char *interned = ustr_intern(e->vm, n->u.str_lit.bytes,
                                       (size_t)n->u.str_lit.len);
    if (interned == NULL) { e->error = EMIT_OOM; return 0U; }

    const uint16_t k = add_const_str(e, interned);
    if (e->error != EMIT_OK) return 0U;

    const uint8_t r = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    emit_instr(e, uinstr_enc_abx(OP_LOADK, r, k), (uint32_t)n->line);
    return r;
}

/* --- AST_NOOP --- */

uint8_t emit_noop_arm(UEmitter *e, const UAstNode *n) {
    /* No-op: load nil as the value. */
    uint8_t r = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U),
               (uint32_t)n->line);
    return r;
}

/* --- AST_UNARY --- */

uint8_t emit_unary_arm(UEmitter *e, UAstNode *n) {
    const uint8_t src_reg = emit_expr(e, n->u.unary.operand);
    if (e->error != EMIT_OK) return 0U;

    if (n->u.unary.op == UOP_NOT) {
        /* Logical NOT — the 4-instruction OP_TEST/OP_LOADBOOL branch idiom
         * (mirrors emit_compare_arm; no new opcode, refactor-3 FE-03):
         *   TEST src, 0, 0     ; skip next when src is falsy
         *   JMP +1             ; truthy -> false arm
         *   LOADBOOL rd, 1, 1  ; rd = true; pc++ (skip false arm)
         *   LOADBOOL rd, 0, 0  ; rd = false */
        const uint8_t rd = src_reg;  /* in-place, same as NEG */
        emit_instr(e, uinstr_enc_abc(OP_TEST, src_reg, 0U, 0U),
                   (uint32_t)n->line);
        emit_instr(e, uinstr_enc_abx(OP_JMP, 0U,
                   (uint16_t)UEMIT_JMP_FALLTHROUGH_BIAS), (uint32_t)n->line);
        emit_instr(e, uinstr_enc_abc(OP_LOADBOOL, rd, 1U, 1U),
                   (uint32_t)n->line);
        emit_instr(e, uinstr_enc_abc(OP_LOADBOOL, rd, 0U, 0U),
                   (uint32_t)n->line);
        return rd;
    }

    /* UOP_NEG: arithmetic negation (parser strips unary '+' at parse time). */
    emit_instr(e, uinstr_enc_abc(OP_NEG, src_reg, src_reg, 0U),
               (uint32_t)n->line);
    return src_reg;   /* dest reuses src; no free_reg */
}

/* --- AST_BINARY --- */

uint8_t emit_binary_arm(UEmitter *e, UAstNode *n) {
    const uint8_t lhs_reg = emit_expr(e, n->u.binary.lhs);
    if (e->error != EMIT_OK) return 0U;
    const uint8_t rhs_reg = emit_expr(e, n->u.binary.rhs);
    if (e->error != EMIT_OK) return 0U;
    emit_instr(e,
               uinstr_enc_abc(binop_to_opcode(n->u.binary.op),
                              lhs_reg, lhs_reg, rhs_reg),
               (uint32_t)n->line);
    free_reg(e);              /* rhs released; lhs holds result in place */
    return lhs_reg;
}

/* --- AST_COMPARE --- */

uint8_t emit_compare_arm(UEmitter *e, UAstNode *n) {
    /* Compile LHS into rb, RHS into the next register. */
    uint8_t rb = e->next_reg;
    uint8_t lhs_reg = emit_expr(e, n->u.cmp.lhs);
    if (e->error != EMIT_OK) return 0U;
    (void)lhs_reg;  /* rb == lhs_reg; named for clarity */
    uint8_t rc_reg = e->next_reg;
    uint8_t rhs_reg = emit_expr(e, n->u.cmp.rhs);
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
    emit_instr(e, uinstr_enc_abc(op, a_bit, b_reg, c_reg), (uint32_t)n->line);
    emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, (uint16_t)UEMIT_JMP_FALLTHROUGH_BIAS), (uint32_t)n->line);
    emit_instr(e, uinstr_enc_abc(OP_LOADBOOL, rb, 1U, 1U), (uint32_t)n->line);
    emit_instr(e, uinstr_enc_abc(OP_LOADBOOL, rb, 0U, 0U), (uint32_t)n->line);

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
 * Register protocol mirrors emit_compare_arm: rd is a TEMP holding the
 * expression value; reset next_reg to rd+1 and bump max_reg_seen so callers
 * can allocate above the result. */
uint8_t emit_logical_arm(UEmitter *e, UAstNode *n) {
    /* Evaluate LHS into rd (the result register). */
    uint8_t rd = e->next_reg;
    uint8_t lhs_reg = emit_expr(e, n->u.logical.lhs);
    if (e->error != EMIT_OK) return 0U;
    (void)lhs_reg;  /* rd == lhs_reg */

    /* TESTSET rd, rd, c — see polarity reasoning above. */
    const uint8_t c = n->u.logical.is_or ? 0U : 1U;
    emit_instr(e, uinstr_enc_abc(OP_TESTSET, rd, rd, c), (uint32_t)n->line);

    /* JMP placeholder — when taken, skips the RHS evaluation (short-circuit). */
    int jmp_skip = (int)emit_instr_count(e);
    emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);

    /* RHS path: evaluate RHS starting at rd (reset cursor so RHS reuses the
     * temp zone above rd), then move the value into rd if it landed elsewhere. */
    e->next_reg = rd;
    uint8_t rhs_reg = emit_expr(e, n->u.logical.rhs);
    if (e->error != EMIT_OK) return 0U;
    if (rhs_reg != rd) {
        emit_instr(e, uinstr_enc_abc(OP_MOVE, rd, rhs_reg, 0U), (uint32_t)n->line);
    }

    /* Patch the short-circuit JMP to land just past the RHS path. */
    {
        int after = (int)emit_instr_count(e);
        emit_patch_instr(e, jmp_skip,
            uinstr_enc_abx(OP_JMP, 0U, uemit_jmp_offset(jmp_skip, after)));
    }

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

uint8_t emit_ident_arm(UEmitter *e, const UAstNode *n) {
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
        emit_instr(e, uinstr_enc_abc(OP_MOVE, dst, (uint8_t)slot, 0U),
                   (uint32_t)n->line);
        e->next_reg++;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        if (e->next_reg > fs->max_reg_seen) fs->max_reg_seen = e->next_reg;

        /* T16: implicit force for lazy locals (spec §4.1).
         * Skip when lazy_arg_context is set — caller is passing this
         * value as a call argument (pass-through, spec §4.2). */
        if (is_lazy_local && !e->lazy_arg_context) {
            /* dst currently holds the thunk closure.  Force it:
             * OP_CALL dst, 1, 2 — zero args, 1 result, result in dst. */
            emit_instr(e, uinstr_enc_abc(OP_CALL, dst, 1U, 2U),
                       (uint32_t)n->line);
        }
        return dst;
    }

    /* Upvalue cascade. */
    int up = find_or_install_upvalue(e, fs, canonical, n->u.ident.len);
    if (up >= 0) {
        uint8_t dst = e->next_reg;
        if (dst >= (uint8_t)(UFS_MAX_REGS - 1)) {
            e->error = EMIT_REG_EXHAUSTED;
            return 0U;
        }
        emit_instr(e, uinstr_enc_abc(OP_GETUPVAL, dst, (uint8_t)up, 0U),
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
         * frame finalizer (uemit_close_function, T73), not inline here.
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
        emit_instr(e, uinstr_enc_abc(OP_GETSLOT, dst, fs->r_global_slot,
                                     (uint8_t)ic_idx),
                   (uint32_t)n->line);
        e->next_reg++;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        if (e->next_reg > fs->max_reg_seen) fs->max_reg_seen = e->next_reg;
        return dst;
    }
}

/* --- AST_VAR_DECL --- */

uint8_t emit_var_decl_arm(UEmitter *e, UAstNode *n) {
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
     * The T73 prologue fills r_global_slot with realm->global_object at
     * function entry; the OP_SETSLOT then writes `init_value` into
     * the correct slot on the global object.
     *
     * NOTE: `var` inside a function body (fs->parent != NULL) still
     * allocates a frame local — the else-branch below handles that.
     * v0.13.5 (LANG4-06): only a BARE chunk-top var (nblocks == 0)
     * installs a realm global; a block-nested chunk-top var falls
     * through to the frame-local branch so it is scoped to its block
     * and cannot clobber an outer binding of the same name (SDK 2.0
     * ch. 17 block-scoped var semantics).  Bare-var REPL persistence
     * is unaffected. */
    if (fs->parent == NULL && fs->nblocks == 0) {
        /* Reserve r_global_slot on first global use (same as T71).
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
             * uemit_close_function (T73) — not emitted inline here. */
        }

        /* Emit init expression into a temp register. */
        uint8_t init_reg = emit_expr(e, n->u.var_decl.init);
        if (e->error != EMIT_OK) return 0U;

        /* Intern slot name and assign IC index. */
        int ic_idx = uemit_assign_ic_index(e, (USymbol *)canonical);
        if (ic_idx < 0) return 0U;

        /* Write value into the global slot. */
        emit_instr(e, uinstr_enc_abc(OP_SETSLOT, init_reg, fs->r_global_slot,
                                     (uint8_t)ic_idx),
                   (uint32_t)n->line);

        /* Track the declared global name so that subsequent AST_ASSIGN
         * nodes (e.g. `n = n + 1` after `var n = 0` at chunk-top) can
         * route to the global slot rather than raising EMIT_UNRESOLVED_NAME.
         * Also record the function signature (global_var_sigs) so that
         * T16 lazy-arg wrapping works at call sites that reference globals. */
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
                            (fn->u.func.params[pi]->kind == AST_LAZY_PARAM);
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
    uint8_t init_reg = emit_expr(e, n->u.var_decl.init);
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

    /* T16: if init is a literal function, record its lazy-param signature
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
                    (fn->u.func.params[pi]->kind == AST_LAZY_PARAM);
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

uint8_t emit_assign_arm(UEmitter *e, UAstNode *n) {
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
            /* T16: reject assignment to lazy parameter (spec §4.5). */
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
        upvalue_idx = find_or_install_upvalue(e, fs, canonical,
                                              n->u.assign.name_len);
        if (upvalue_idx < 0) {
            /* T72: at chunk-top, check whether this name was declared via
             * `var` (stored in global_var_names).  If so, route to the
             * global slot via OP_SETSLOT rather than raising an error.
             * Names that were never declared still produce EMIT_UNRESOLVED_NAME. */
            if (fs->parent == NULL && fs->references_global) {
                for (int gi = 0; gi < fs->n_global_vars; gi++) {
                    if (fs->global_var_names[gi] == canonical) {
                        is_global_assign = true;
                        break;
                    }
                }
            }
            if (!is_global_assign) {
                e->error = EMIT_UNRESOLVED_NAME;
                urbi_emit_diag_error(e, n, "undefined name '%.*s'",
                                n->u.assign.name_len, n->u.assign.name_start);
                return 0U;
            }
        }
    }

    /* Emit RHS into top temp. */
    uint8_t reg_before = e->next_reg;
    uint8_t rhs_reg = emit_expr(e, n->u.assign.value);
    if (e->error != EMIT_OK) return 0U;

    /* Move into the target slot. */
    if (local_slot >= 0) {
        emit_instr(e, uinstr_enc_abc(OP_MOVE, (uint8_t)local_slot,
                                     rhs_reg, 0U),
                   (uint32_t)n->line);
    } else if (is_global_assign) {
        /* T72: write to the global slot on the realm object. */
        int ic_idx = uemit_assign_ic_index(e, (USymbol *)canonical);
        if (ic_idx < 0) return 0U;
        emit_instr(e, uinstr_enc_abc(OP_SETSLOT, rhs_reg, fs->r_global_slot,
                                     (uint8_t)ic_idx),
                   (uint32_t)n->line);
        /* T16: if RHS is a literal function, update global_var_sigs so
         * subsequent calls to this global get correct lazy-arg wrapping. */
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
                                (fn->u.func.params[pi]->kind == AST_LAZY_PARAM);
                        }
                    }
                }
                break;
            }
        }
    } else {
        emit_instr(e, uinstr_enc_abc(OP_SETUPVAL, rhs_reg,
                                     (uint8_t)upvalue_idx, 0U),
                   (uint32_t)n->line);
    }
    /* Free the temp — it was only needed for the RHS. */
    e->next_reg = reg_before;
    /* Assignment expression value is the target slot's value; return it. */
    return (local_slot >= 0) ? (uint8_t)local_slot : reg_before;
}

/* --- AST_NARY --- */

uint8_t emit_nary_arm(UEmitter *e, UAstNode *n) {
    if (n->u.nary.separator == SEP_COMMA) {
        /* `,` parallel semantics (M3 closure-spawn).
         *
         * Spec §3 row 3: each child runs in parallel — last child's value
         * is the expression's value.  M3 closure-spawn: children 0..count-2
         * are compiled to closures (capturing surrounding locals via upvalue
         * cascade) and spawned as detached strands via OP_FORK_DETACH.
         * The last child runs inline as the parent's continuation.
         *
         * Known limitation: fork-thunks cannot write to chunk-top variables
         * (they fail with EMIT_UNRESOLVED_NAME). Chunk-top declarations live
         * in global_var_names of the parent scope, while the thunk's assign
         * path searches only its own actvars. The global-assign fallback in
         * emit_assign_arm is gated on parent == NULL, preventing thunks from
         * reaching it. Reads of chunk-top vars work via the realm-slot
         * fallback. The fix would be teaching emit_assign_arm to emit
         * OP_SETSLOT when the thunk assigns to a chunk-top name. */
        if (e->current_fs == NULL) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0U;
        }
        int i;
        for (i = 0; i < n->u.nary.count - 1; i++) {
            /* Compile child[i] as a zero-arg closure (thunk). */
            uint8_t closure_reg = emit_lazy_thunk(e, n->u.nary.children[i]);
            if (e->error != EMIT_OK) return 0U;
            /* OP_FORK_DETACH A=closure_reg: spawn detached strand. */
            emit_instr(e, uinstr_enc_abc(OP_FORK_DETACH, closure_reg, 0U, 0U),
                       (uint32_t)n->u.nary.children[i]->line);
            if (e->error != EMIT_OK) return 0U;
            /* Release the closure register (temp). */
            if (e->next_reg > e->current_fs->freereg)
                e->next_reg = e->current_fs->freereg;
        }
        /* Last child runs inline; its result is the NARY's value. */
        uint8_t r = emit_expr(e, n->u.nary.children[n->u.nary.count - 1]);
        if (e->error != EMIT_OK) return 0U;
        return r;
    }
    /* SEP_SEMI: compile each child; OP_YIELD between children (not
       before first, not after last); release temp regs between.
       Last child's result register is the Nary's value.

       Between children, reset next_reg to freereg (first slot above
       all declared locals) rather than blindly decrementing.  The
       decrement-by-one pattern is wrong when a var-decl child has
       promoted a temp into a permanent local (advancing freereg), or
       when a non-var child needed more than one temp: both cases leave
       freereg ahead of where a simple decrement would land. */
    uint8_t r = 0U;
    for (int i = 0; i < n->u.nary.count; i++) {
        if (i > 0) {
            /* Release all temps allocated by the previous child.
             *
             * Bug fix 2026-05-16 (urbiscript-scan stress test, eye_demo
             * BlobScan.scan): the previous reset was just
             *     e->next_reg = e->current_fs->freereg;
             * which kept whatever freereg the previous child left behind.
             * But emit_call_arm for a discarded-result bare call (e.g.
             * `c_scan_begin();`) leaves freereg ABOVE the local-zone
             * floor (one past the call result), so subsequent var-decl
             * children get a slot at the drifted next_reg.  emit_var_decl_arm
             * increments nactvar by 1 but assigns lv->slot = drifted-slot;
             * fs_temp_floor (count-based: nactvar + r_global_slot) then
             * UNDERESTIMATES the true local-zone top by the drift amount.
             * On the next emit_while_arm body open, freereg gets reset to
             * fs_temp_floor → lands BELOW already-declared locals → the
             * loop body's `var x = 0` aliases the outer `iters` register,
             * and runtime x/y iterate in lockstep through the diagonal.
             *
             * Mirror emit_block_arm's reset pattern: drop freereg back to
             * fs_temp_floor so the next child sees a clean local-zone
             * boundary, regardless of what kind of expression the
             * previous child was. */
            e->current_fs->freereg = fs_temp_floor(e->current_fs);
            e->next_reg = e->current_fs->freereg;
            /* refactor-3 VM-02/B4: cleanup bodies (finally / onleave) are
             * atomic — `;` separates statements but yields nothing there.
             * An OP_YIELD inside the unwind-copy finally would enqueue the
             * strand mid-walk (run_cleanup_with_replace treats the yield as
             * body completion): scheduler assert / corruption. */
            if (!e->in_cleanup_body) {
                emit_instr(e, uinstr_enc_abc(OP_YIELD, 0U, 0U, 0U),
                           e->prev_line);
                if (e->error != EMIT_OK) return 0U;
            }
        }
        r = emit_expr(e, n->u.nary.children[i]);
        if (e->error != EMIT_OK) return 0U;
    }
    return r;
}

/* --- AST_BIN_SEP --- */

uint8_t emit_bin_sep_arm(UEmitter *e, UAstNode *n) {
    if (n->u.bin_sep.separator == SEP_AMP) {
        /* `&` fork-join (M3 closure-spawn).
         *
         * Spec §3 row 4 + §3.2 + §7.2: spawn RHS as a child strand,
         * wait for it to complete, then produce void as the result.
         *
         * Emit sequence:
         *   1. Compile RHS to a closure (thunk) → closure_reg.
         *   2. Compile LHS inline (parent strand continues).
         *   3. OP_FORK_JOIN  A=closure_reg  B=child_reg  → spawns + stores handle.
         *   4. OP_JOIN_WAIT  A=child_reg                 → block until child DEAD.
         *   5. OP_LOADVOID   A=result_reg                → result is void (spec §7.2).
         *
         * Known limitation: fork-thunks cannot write to chunk-top variables
         * (they fail with EMIT_UNRESOLVED_NAME). Chunk-top declarations live
         * in global_var_names of the parent scope, while the thunk's assign
         * path searches only its own actvars. The global-assign fallback in
         * emit_assign_arm is gated on parent == NULL, preventing thunks from
         * reaching it. Reads of chunk-top vars work via the realm-slot
         * fallback. The fix would be teaching emit_assign_arm to emit
         * OP_SETSLOT when the thunk assigns to a chunk-top name. */
        if (e->current_fs == NULL) {
            e->error = EMIT_UNSUPPORTED_AST;
            return 0U;
        }
        /* Step 1: compile RHS to a closure. */
        uint8_t closure_reg = emit_lazy_thunk(e, n->u.bin_sep.rhs);
        if (e->error != EMIT_OK) return 0U;

        /* Step 2: compile LHS inline; release its register after. */
        uint8_t lhs_save = e->next_reg;
        uint8_t lhs_r = emit_expr(e, n->u.bin_sep.lhs);
        if (e->error != EMIT_OK) return 0U;
        (void)lhs_r;
        /* Restore next_reg to above freereg after LHS (keep closure_reg alive). */
        if (e->next_reg > e->current_fs->freereg &&
            e->next_reg > lhs_save)
            e->next_reg = lhs_save;
        (void)lhs_save;

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
        emit_instr(e, uinstr_enc_abc(OP_FORK_JOIN, closure_reg, child_reg, 0U),
                   (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Step 4: OP_JOIN_WAIT A=child_reg. */
        emit_instr(e, uinstr_enc_abc(OP_JOIN_WAIT, child_reg, 0U, 0U),
                   (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Step 5: OP_LOADVOID into result_reg (spec §7.2: `&` result is void). */
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
        emit_instr(e, uinstr_enc_abc(OP_LOADVOID, result_reg, 0U, 0U),
                   (uint32_t)n->line);
        return result_reg;
    }
    /* SEP_PIPE: lhs then rhs in sequence, no yield.
       LHS value is discarded; result is rhs.

       EMIT-009 fix (Wave 5, v0.5.7): sync next_reg to the FuncState
       freereg before emitting RHS, mirroring the v0.5.2 AST_NARY shape
       (commit 882fbb8).  The bare next_reg-- is wrong when LHS leaves
       freereg promoted (e.g., LHS ends in a function literal, which
       lifts freereg via emit_function_literal's `freereg++`); after
       next_reg-- the cursor sits BELOW freereg and RHS allocation
       clobbers a still-live LHS temp.  See
       tests/unit/test_emit_freereg_drift.c::
       emit_sep_pipe_does_not_alias_lhs_temp_with_rhs. */
    uint8_t lhs_r = emit_expr(e, n->u.bin_sep.lhs);
    if (e->error != EMIT_OK) return 0U;
    /* Release lhs register before rhs so rhs may reuse the slot. */
    (void)lhs_r;
    e->next_reg = e->current_fs->freereg;
    uint8_t rhs_r = emit_expr(e, n->u.bin_sep.rhs);
    return rhs_r;
}

/* --- AST_BLOCK --- */

uint8_t emit_block_arm(UEmitter *e, UAstNode *n) {
    /* Scoped sequence of statements inside `{ }`.
       Opens a block scope so locals declared inside don't outlive the
       block.  The block's "value" is the last statement's result reg
       (or nil if empty).  Temps are reset between statements. */
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    if (!uemit_open_block(e, false)) return 0U;

    uint8_t r = 0U;
    for (int i = 0; i < n->u.block.count; i++) {
        r = emit_expr(e, n->u.block.stmts[i]);
        if (e->error != EMIT_OK) {
            uemit_close_block(e);
            return 0U;
        }
        if (i < n->u.block.count - 1) {
            /* Release temps between statements; locals stay. */
            e->current_fs->freereg = fs_temp_floor(e->current_fs);
            e->next_reg = e->current_fs->freereg;
        }
    }

    if (!uemit_close_block(e)) return 0U;
    return r;
}

/* === W10/v0.10.5: list/dict literals + subscript emit =====================
 *
 * Stdlib-call lowering — no new opcodes.  Wire format stays at v1.9.
 *
 *   [e1, e2, e3]      → List.new(e1, e2, e3)
 *   ["k" => v, ...]   → _d = Dict.new(); _d.set("k", v); ... ; _d
 *   l[i]              → l.get(i)
 *   l[i] = v          → l.set(i, v)
 *   l[i] += v         → l.set(i, l.get(i) + v)
 *
 * All lowerings use synthetic AST_IDENT + AST_CALL + AST_MEMBER_GET nodes
 * built on the arena and fed back through emit_expr — this inherits the
 * existing method-call ABI (OP_SELF + OP_CALL), realm-global resolution,
 * and IC index assignment for free.
 * ========================================================================= */

/* Helper: intern a C-string name into a synthetic AST_IDENT on the arena. */
static UAstNode *synth_ident(UEmitter *e, const char *name, int line) {
    UAstNode *n = (UAstNode *)uarena_alloc(e->arena, sizeof(UAstNode));
    if (!n) { e->error = EMIT_OOM; return NULL; }
    urbi_zero(n, sizeof(*n));
    n->kind = AST_IDENT;
    n->line = line;
    n->col  = 0;
    n->u.ident.start = name;  /* static / interned lifetime; safe */
    n->u.ident.len   = 0;
    /* Compute length by scanning (name is a short compile-time literal). */
    {
        int l = 0;
        while (name[l] != '\0') l++;
        n->u.ident.len = l;
    }
    return n;
}

/* Helper: build a synthetic AST_MEMBER_GET (recv.method_name). */
static UAstNode *synth_member_get(UEmitter *e, UAstNode *recv,
                                   const char *method_name, int line) {
    UAstNode *n = (UAstNode *)uarena_alloc(e->arena, sizeof(UAstNode));
    if (!n) { e->error = EMIT_OOM; return NULL; }
    urbi_zero(n, sizeof(*n));
    n->kind = AST_MEMBER_GET;
    n->line = line;
    n->col  = 0;
    n->u.member.recv       = recv;
    n->u.member.name_start = method_name;
    {
        int l = 0;
        while (method_name[l] != '\0') l++;
        n->u.member.name_len = l;
    }
    n->u.member.value = NULL;
    return n;
}

/* Helper: build a synthetic AST_CALL (callee(args[0..nargs-1])). */
static UAstNode *synth_call(UEmitter *e, UAstNode *callee,
                             UAstNode **args, int nargs, int line) {
    UAstNode *n = (UAstNode *)uarena_alloc(e->arena, sizeof(UAstNode));
    if (!n) { e->error = EMIT_OOM; return NULL; }
    urbi_zero(n, sizeof(*n));
    n->kind = AST_CALL;
    n->line = line;
    n->col  = 0;
    n->u.call.callee    = callee;
    n->u.call.args      = args;
    n->u.call.arg_count = nargs;
    return n;
}

/* --- emit_list_lit_arm — AST_LIST_LIT: [e1, e2, ...]
 *
 * Lowers to: List.new(e1, e2, ...)
 *
 * Builds: AST_CALL { callee = AST_MEMBER_GET{List, "new"}, args = [e1...] }
 * then recurses through emit_call_arm for the standard method-call ABI. */

uint8_t emit_list_lit_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    int line = n->line;

    /* Build: List.new — MEMBER_GET on "List" ident. */
    UAstNode *list_ident = synth_ident(e, "List", line);
    if (!list_ident) return 0U;
    UAstNode *callee = synth_member_get(e, list_ident, "new", line);
    if (!callee) return 0U;

    /* Allocate args array from arena. */
    int argc = n->u.list_lit.count;
    UAstNode **args = NULL;
    if (argc > 0) {
        args = (UAstNode **)uarena_alloc(e->arena,
                                          (size_t)argc * sizeof(UAstNode *));
        if (!args) { e->error = EMIT_OOM; return 0U; }
        for (int i = 0; i < argc; i++) args[i] = n->u.list_lit.elems[i];
    }

    UAstNode *call = synth_call(e, callee, args, argc, line);
    if (!call) return 0U;

    return emit_call_arm(e, call);
}

/* --- emit_dict_lit_arm — AST_DICT_LIT: ["k1" => v1, "k2" => v2, ...]
 *
 * Lowers to:
 *   _d = Dict.new()
 *   _d.set("k1", v1)
 *   _d.set("k2", v2)
 *   ...
 *   _d   (result)
 *
 * Implemented by emitting the instructions directly: alloc _d_reg, emit
 * Dict.new() call, then for each pair emit the .set() call.  Result is _d_reg.
 *
 * Note: _d_reg is held across the .set() calls.  Each .set() call is emitted
 * as a synthetic CALL where the callee is MEMBER_GET{_d_ident, "set"}.
 * However, since _d is a local temp (not in the actvar table), we cannot
 * use AST_IDENT to reference it — instead we emit its register directly via a
 * special synthetic pattern.
 *
 * Simpler direct approach: emit Dict.new() call → result in rd; then for each
 * pair emit OP_SELF + OP_CALL for .set(k, v) using rd as receiver. */

uint8_t emit_dict_lit_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    int line = n->line;

    /* Step 1: emit Dict.new() → result register becomes rd. */
    uint8_t rd;
    {
        UAstNode *dict_ident = synth_ident(e, "Dict", line);
        if (!dict_ident) return 0U;
        UAstNode *callee = synth_member_get(e, dict_ident, "new", line);
        if (!callee) return 0U;
        UAstNode *call0 = synth_call(e, callee, NULL, 0, line);
        if (!call0) return 0U;
        /* emit_call_arm returns callee_reg which is where the result lives.
         * After the call, next_reg = r + 1 (per emit_call_arm contract). */
        rd = emit_call_arm(e, call0);
        if (e->error != EMIT_OK) return 0U;
    }

    /* Step 2: for each key-value pair, emit rd.set(key, value).
     * We use direct bytecode emission (OP_SELF + args + OP_CALL) to keep
     * rd stable across iterations without declaring a local. */
    {
        const char *set_name = "set";
        USymbol *set_sym = (USymbol *)ustr_intern(e->vm, set_name, 3U);
        if (!set_sym) { e->error = EMIT_OOM; return 0U; }

        for (int i = 0; i < n->u.dict_lit.count; i++) {
            /* Layout: R[call_base] = method, R[call_base+1] = self (rd),
             *         R[call_base+2] = key,  R[call_base+3] = value. */
            e->next_reg = rd + 1U;  /* reuse temps above rd */
            if (e->current_fs->freereg < e->next_reg)
                e->current_fs->freereg = e->next_reg;

            uint8_t call_base = alloc_reg(e);
            if (e->error != EMIT_OK) return 0U;
            uint8_t self_reg = alloc_reg(e); /* callee_reg+1 = self */
            if (e->error != EMIT_OK) return 0U;
            (void)self_reg;  /* OP_SELF fills it */

            /* OP_SELF: load method + snapshot receiver into call_base/call_base+1. */
            int ic_idx = uemit_assign_ic_index(e, set_sym);
            if (ic_idx < 0) return 0U;
            emit_instr(e, uinstr_enc_abc(OP_SELF, call_base, rd, (uint8_t)ic_idx),
                       (uint32_t)line);

            /* Reset next_reg to args position (call_base+2). */
            e->next_reg = (uint8_t)(call_base + 2U);
            if (e->current_fs->freereg > e->next_reg)
                e->current_fs->freereg = e->next_reg;

            /* Emit key arg. */
            if (e->current_fs->freereg < e->next_reg)
                e->current_fs->freereg = e->next_reg;
            uint8_t key_r = emit_expr(e, n->u.dict_lit.keys[i]);
            if (e->error != EMIT_OK) return 0U;
            uint8_t key_expected = (uint8_t)(call_base + 2U);
            if (key_r != key_expected) {
                emit_instr(e, uinstr_enc_abc(OP_MOVE, key_expected, key_r, 0U),
                           (uint32_t)line);
                e->next_reg = key_expected + 1U;
            }

            /* Emit value arg. */
            if (e->current_fs->freereg < e->next_reg)
                e->current_fs->freereg = e->next_reg;
            uint8_t val_r = emit_expr(e, n->u.dict_lit.vals[i]);
            if (e->error != EMIT_OK) return 0U;
            uint8_t val_expected = (uint8_t)(call_base + 3U);
            if (val_r != val_expected) {
                emit_instr(e, uinstr_enc_abc(OP_MOVE, val_expected, val_r, 0U),
                           (uint32_t)line);
            }

            /* OP_CALL: method call with 2 explicit args (B = 2+2 = 4). */
            emit_instr(e, uinstr_enc_abc(OP_CALL, call_base, 4U, 0x82U),
                       (uint32_t)line);

            /* Result of .set() is discarded; restore next_reg to rd+1. */
            e->next_reg = rd + 1U;
            if (e->current_fs->freereg > e->next_reg)
                e->current_fs->freereg = e->next_reg;
        }
    }

    /* Result is rd (the dict object). */
    e->next_reg = rd + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs != NULL && e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;
    return rd;
}

/* Helper: build a synthetic AST_REG_REF leaf (W2/v0.10.7).
 *
 * Used in emit_subscript_set_arm to pin recv and index into temp registers
 * before building the synthetic .get/.set calls, so each expression is
 * evaluated exactly once even when the caller is a side-effectful expression
 * like makeList()[nextIdx()] += v. */
static UAstNode *synth_reg_ref(UEmitter *e, uint8_t reg, int line) {
    UAstNode *n = (UAstNode *)uarena_alloc(e->arena, sizeof(UAstNode));
    if (!n) { e->error = EMIT_OOM; return NULL; }
    urbi_zero(n, sizeof(*n));
    n->kind = AST_REG_REF;
    n->line = line;
    n->u.reg_ref.reg = reg;
    return n;
}

/* --- emit_subscript_get_arm — AST_SUBSCRIPT_GET: l[i] → l.get(i) */

uint8_t emit_subscript_get_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    int line = n->line;

    /* Build: recv.get(index)
     * Args array must be arena-allocated — synth_call stores the pointer and
     * emit_call_arm may read it after this stack frame returns. */
    UAstNode *mg = synth_member_get(e, n->u.subscript.recv, "get", line);
    if (!mg) return 0U;
    UAstNode **args = (UAstNode **)uarena_alloc(e->arena, sizeof(UAstNode *));
    if (!args) { e->error = EMIT_OOM; return 0U; }
    args[0] = n->u.subscript.index;
    UAstNode *call = synth_call(e, mg, args, 1, line);
    if (!call) return 0U;

    return emit_call_arm(e, call);
}

/* --- emit_subscript_set_arm — AST_SUBSCRIPT_SET:
 *   l[i] = v    → l.set(i, v)
 *   l[i] += v   → l.set(i, l.get(i) + v)   (is_compound_add=true) */

uint8_t emit_subscript_set_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    int line = n->line;

    UAstNode *rhs_val = n->u.subscript.value;

    if (n->u.subscript.is_compound_add) {
        /* W2/v0.10.7: Single-evaluate recv and index.
         *
         * Previous lowering re-emitted n->u.subscript.recv and
         * n->u.subscript.index for both the synthetic .get and .set calls,
         * causing side-effectful expressions like makeList()[nextIdx()] to
         * fire twice.  Fix: emit each expression once into a temp register,
         * then build synth calls against AST_REG_REF leaves so the emitter
         * moves from the already-evaluated register rather than re-evaluating
         * the original AST node.
         *
         * Register pinning: save next_reg before each emit_expr, then bump
         * freereg past the result so subsequent alloc_reg calls don't reuse
         * the slot. */

        /* --- Single-evaluate receiver --- */
        uint8_t recv_reg = e->next_reg;
        (void)emit_expr(e, n->u.subscript.recv);
        if (e->error != EMIT_OK) return 0U;
        /* recv_reg now holds the receiver value (emit_call_arm contract:
         * result at callee_reg, next_reg = callee_reg + 1). */
        if (e->current_fs->freereg <= recv_reg)
            e->current_fs->freereg = recv_reg + 1U;
        e->next_reg = e->current_fs->freereg;

        /* --- Single-evaluate index --- */
        uint8_t idx_reg = e->next_reg;
        (void)emit_expr(e, n->u.subscript.index);
        if (e->error != EMIT_OK) return 0U;
        if (e->current_fs->freereg <= idx_reg)
            e->current_fs->freereg = idx_reg + 1U;
        e->next_reg = e->current_fs->freereg;

        /* --- Build AST_REG_REF wrappers --- */
        UAstNode *recv_ref = synth_reg_ref(e, recv_reg, line);
        UAstNode *idx_ref  = synth_reg_ref(e, idx_reg,  line);
        if (!recv_ref || !idx_ref) return 0U;

        /* --- recv.get(idx_ref) --- */
        UAstNode *get_mg = synth_member_get(e, recv_ref, "get", line);
        if (!get_mg) return 0U;
        UAstNode **get_args = (UAstNode **)uarena_alloc(e->arena, sizeof(UAstNode *));
        if (!get_args) { e->error = EMIT_OOM; return 0U; }
        get_args[0] = idx_ref;
        UAstNode *get_call = synth_call(e, get_mg, get_args, 1, line);
        if (!get_call) return 0U;

        /* --- AST_BINARY: get_call + rhs_val --- */
        UAstNode *add_node = (UAstNode *)uarena_alloc(e->arena, sizeof(UAstNode));
        if (!add_node) { e->error = EMIT_OOM; return 0U; }
        urbi_zero(add_node, sizeof(*add_node));
        add_node->kind = AST_BINARY;
        add_node->line = line;
        add_node->u.binary.op  = BOP_ADD;
        add_node->u.binary.lhs = get_call;
        add_node->u.binary.rhs = rhs_val;
        rhs_val = add_node;

        /* --- recv.set(idx_ref, rhs_val) using the same pinned refs --- */
        UAstNode *set_mg = synth_member_get(e, recv_ref, "set", line);
        if (!set_mg) return 0U;
        UAstNode **set_args = (UAstNode **)uarena_alloc(e->arena,
                                                         2U * sizeof(UAstNode *));
        if (!set_args) { e->error = EMIT_OOM; return 0U; }
        set_args[0] = idx_ref;
        set_args[1] = rhs_val;
        UAstNode *call = synth_call(e, set_mg, set_args, 2, line);
        if (!call) return 0U;

        return emit_call_arm(e, call);
    }

    /* Non-compound: simple recv.set(index, rhs_val) — single use, no change. */
    UAstNode *mg = synth_member_get(e, n->u.subscript.recv, "set", line);
    if (!mg) return 0U;
    UAstNode **set_args = (UAstNode **)uarena_alloc(e->arena,
                                                     2U * sizeof(UAstNode *));
    if (!set_args) { e->error = EMIT_OOM; return 0U; }
    set_args[0] = n->u.subscript.index;
    set_args[1] = rhs_val;
    UAstNode *call = synth_call(e, mg, set_args, 2, line);
    if (!call) return 0U;

    return emit_call_arm(e, call);
}
/* === end W10/v0.10.5 === */
