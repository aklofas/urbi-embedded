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
#include "module/umodule.h"
#include "parse/uast.h"
#include "runtime/umacros.h"
#include <stddef.h>
#include <stdint.h>

/* --- AST_INT --- */

uint8_t emit_int_arm(UEmitter *e, UAstNode *n) {
    const uint8_t r = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    const uint16_t k = add_const_int(e, n->u.i);
    if (e->error != EMIT_OK) return 0U;
    emit_instr(e, uinstr_enc_abx(OP_LOADK, r, k), (uint32_t)n->line);
    return r;
}

/* --- AST_BOOL --- */

uint8_t emit_bool_arm(UEmitter *e, UAstNode *n) {
    uint8_t r = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    emit_instr(e, uinstr_enc_abc(OP_LOADBOOL, r, n->u.b ? 1U : 0U, 0U),
               (uint32_t)n->line);
    return r;
}

/* --- AST_NIL --- */

uint8_t emit_nil_arm(UEmitter *e, UAstNode *n) {
    uint8_t r = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U),
               (uint32_t)n->line);
    return r;
}

/* --- AST_NOOP --- */

uint8_t emit_noop_arm(UEmitter *e, UAstNode *n) {
    /* No-op: load nil as the value. */
    uint8_t r = alloc_reg(e);
    if (e->error != EMIT_OK) return 0U;
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U),
               (uint32_t)n->line);
    return r;
}

/* --- AST_UNARY --- */

uint8_t emit_unary_arm(UEmitter *e, UAstNode *n) {
    /* M1: parser strips UOP_PLUS at parse time, so AST_UNARY is always
       negation.  Emit the operand into src_reg, then NEG in-place. */
    const uint8_t src_reg = emit_expr(e, n->u.unary.operand);
    if (e->error != EMIT_OK) return 0U;
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

/* --- AST_IDENT --- */

uint8_t emit_ident_arm(UEmitter *e, UAstNode *n) {
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
     * allocates a frame local — the else-branch below handles that. */
    if (fs->parent == NULL) {
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
            return 0U;
        }
    }
    if (fs->nactvar >= UFS_MAX_LOCALS) {
        e->error = EMIT_REG_EXHAUSTED;
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
         * TODO(M5+/design-risks-7): replace closure-spawn with shared-frame
         * spawn to satisfy spec §7.1 (comma-environment.chk semantics). */
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
            /* Release all temps allocated by the previous child, but
             * keep locals (tracked by freereg / nactvar). */
            e->next_reg = e->current_fs->freereg;
            emit_instr(e, uinstr_enc_abc(OP_YIELD, 0U, 0U, 0U),
                       e->prev_line);
            if (e->error != EMIT_OK) return 0U;
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
         * TODO(M5+/design-risks-7): shared-frame spawn for spec §7.1 compliance. */
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
       LHS value is discarded; result is rhs. */
    uint8_t lhs_r = emit_expr(e, n->u.bin_sep.lhs);
    if (e->error != EMIT_OK) return 0U;
    /* Release lhs register before rhs so rhs may reuse the slot. */
    (void)lhs_r;
    if (e->next_reg > 0U) e->next_reg--;
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
