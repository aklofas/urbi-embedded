/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_unwind.c — unwind / control-transfer bytecode emitters.
 * Extracted from uemit.c during v0.5.4-decompose (EMIT-045 #6).
 *
 * Contains:
 *   - Public encoder helpers for unwind opcodes (uemit_throw, uemit_try_begin,
 *     uemit_try_end, uemit_tag_stop, uemit_push_tag, uemit_pop_tag,
 *     uemit_push_frame_guard, uemit_resume, uemit_load_catch_value).
 *   - emit_expr arm helpers for AST_THROW, AST_TRY, AST_TAG_PREFIX. */

#include "emit/uemit_internal.h"
#include "runtime/ucleanup.h"   /* FLAG_HAS_CATCH, FLAG_HAS_FINALLY */
#include "value/uintern.h"      /* ustr_intern — catch variable interning */
#include "emit/uemit.h"
#include "chunk/uchunk.h"
#include "parse/uast.h"
#include <stddef.h>
#include <stdint.h>

/* =========================================================================
 * M3 row 7 control-transfer opcode encoder helpers.
 * Each function encodes exactly one instruction word and calls emit_instr.
 * See umodule.h §M3 row 7 for the field layout of each opcode.
 * ========================================================================= */

/* OP_THROW ABx: A = reg_value, Bx = 0 (unused). */
void uemit_throw(UEmitter *e, uint8_t reg_value, uint32_t line) {
    emit_instr(e, uinstr_enc_abx(OP_THROW, reg_value, 0U), line);
}

/* OP_TAG_STOP ABC: A = reg_tag, B = reg_value, C = 0. */
void uemit_tag_stop(UEmitter *e, uint8_t reg_tag, uint8_t reg_value, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_TAG_STOP, reg_tag, reg_value, 0U), line);
}

/* OP_TRY_BEGIN ABx: A = flags byte, Bx = handler PC (16-bit, range 0-65535).
 * flags bits: bit 0 = has_catch, bit 1 = has_finally (defined by T9/T10). */
void uemit_try_begin(UEmitter *e, uint8_t flags, uint16_t handler_pc, uint32_t line) {
    emit_instr(e, uinstr_enc_abx(OP_TRY_BEGIN, flags, handler_pc), line);
}

/* OP_TRY_END ABC: no operands (all zero). Pops top cleanup entry. */
void uemit_try_end(UEmitter *e, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_TRY_END, 0U, 0U, 0U), line);
}

/* OP_PUSH_TAG ABx: A packs flags nibble and tag_reg nibble.
 *   A[7:4] = flags (4 bits, values 0-15)
 *   A[3:0] = tag_reg (4 bits, values 0-15)
 *   Bx     = onleave PC (16-bit, range 0-65535)
 * tag_reg must be in [0,15]; flags must be in [0,15]. T30 revisits
 * if wider operand ranges become necessary. */
void uemit_push_tag(UEmitter *e, uint8_t reg_tag, uint8_t flags,
                    uint16_t onleave_pc, uint32_t line) {
    uint8_t a = (uint8_t)(((flags & 0xFU) << 4) | (reg_tag & 0xFU));
    emit_instr(e, uinstr_enc_abx(OP_PUSH_TAG, a, onleave_pc), line);
}

/* OP_POP_TAG ABC: A = reg_tag, B = C = 0. */
void uemit_pop_tag(UEmitter *e, uint8_t reg_tag, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_POP_TAG, reg_tag, 0U, 0U), line);
}

/* OP_PUSH_FRAME_GUARD ABC: A = register_base, B = register_count, C = 0. */
void uemit_push_frame_guard(UEmitter *e, uint8_t register_base,
                             uint8_t register_count, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_PUSH_FRAME_GUARD, register_base,
                                  register_count, 0U), line);
}

/* OP_RESUME ABC: A = reg_state, B = C = 0. */
void uemit_resume(UEmitter *e, uint8_t reg_state, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_RESUME, reg_state, 0U, 0U), line);
}

/* OP_LOAD_CATCH_VALUE ABC: A = destination register, B = C = 0.
 * T10 empirical addition — loads s->catch_value into R[A] at handler entry. */
void uemit_load_catch_value(UEmitter *e, uint8_t reg, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_LOAD_CATCH_VALUE, reg, 0U, 0U), line);
}

/* =========================================================================
 * emit_expr arm helpers for AST_THROW / AST_TRY / AST_TAG_PREFIX.
 * Moved from the monolithic emit_expr switch (EMIT-045 #6).
 * emit_try_arm contains the EMIT-033 collapse (emit_try_frame +
 * emit_catch_handler_section).
 * ========================================================================= */

/* ---------------------------------------------------------------------------
 * try/catch/finally helpers (EMIT-033).
 * -------------------------------------------------------------------------- */

/* Emit the catch-handler section: reset temps, declare the catch variable
 * (if named), emit OP_LOAD_CATCH_VALUE, optionally emit a guard check that
 * re-throws if the guard expression is falsy, emit catch_body in a new block,
 * then un-declare the catch var.
 *
 * Called once from the catch+finally path and once from the catch-only path.
 * Both callers have already patched the TRY_BEGIN Bx to the current PC.
 *
 * Wave 6 W5: if catch_guard != NULL, emit:
 *   OP_LOAD_CATCH_VALUE → e_reg
 *   <guard expr> → guard_reg
 *   OP_TEST guard_reg, 0, 1   ; skip JMP if guard is truthy (guard passes)
 *   OP_JMP  throw_pc          ; guard failed — re-throw
 *   <catch body>
 *   OP_JMP  past_throw_pc
 *   [throw_pc]: OP_THROW e_reg */
static void emit_catch_handler_section(UEmitter *e, UAstNode *n) {
    const char *cv_name = NULL;
    uint8_t     e_reg   = 0U;  /* register holding the caught exception value */
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    if (n->u.try_stmt.catch_var_start != NULL && e->vm != NULL) {
        cv_name = ustr_intern(e->vm,
                              n->u.try_stmt.catch_var_start,
                              (size_t)n->u.try_stmt.catch_var_len);
        if (cv_name == NULL) { e->error = EMIT_OOM; return; }
        int slot = uemit_declare_local(e, cv_name,
                                       n->u.try_stmt.catch_var_len);
        if (slot < 0) return;
        e_reg = (uint8_t)slot;
        uemit_load_catch_value(e, e_reg, (uint32_t)n->line);
        if (e->error != EMIT_OK) return;
    } else {
        e_reg = e->next_reg++;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        uemit_load_catch_value(e, e_reg, (uint32_t)n->line);
        if (e->error != EMIT_OK) return;
    }

    /* Optional guard: `catch (var e if cond)` — re-throw if guard is falsy.
     * Layout:
     *   <guard expr> → guard_reg
     *   OP_TEST guard_reg, 0, 1   ; skip JMP if guard is truthy (pass)
     *   OP_JMP  rethrow_pc        ; guard failed — re-throw
     *   <catch body>
     *   OP_JMP  past_rethrow_pc
     *   [rethrow_pc]: OP_THROW e_reg */
    int jmp_past_throw_pc = -1;
    if (n->u.try_stmt.catch_guard != NULL) {
        uint8_t guard_reg = emit_expr(e, n->u.try_stmt.catch_guard);
        if (e->error != EMIT_OK) return;

        /* TEST guard_reg, 0, 1 — skip the JMP when guard is truthy (pass) */
        emit_instr(e, uinstr_enc_abc(OP_TEST, guard_reg, 0U, 1U), (uint32_t)n->line);
        if (e->error != EMIT_OK) return;

        /* JMP to re-throw when guard is falsy (patched after catch body) */
        jmp_past_throw_pc = (int)emit_instr_count(e);
        emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);
        if (e->error != EMIT_OK) return;
    }

    if (!uemit_open_block(e, false)) return;
    emit_expr(e, n->u.try_stmt.catch_body);
    if (e->error != EMIT_OK) { uemit_close_block(e); return; }
    if (!uemit_close_block(e)) return;

    if (n->u.try_stmt.catch_guard != NULL) {
        /* JMP past the re-throw (catch body has finished, guard passed) */
        int jmp_past_rethrow_pc = (int)emit_instr_count(e);
        emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);
        if (e->error != EMIT_OK) return;

        /* Patch jmp_past_throw → here: guard failed, re-throw */
        {
            int rethrow_target = (int)emit_instr_count(e);
            emit_patch_instr(e, jmp_past_throw_pc,
                uinstr_enc_abx(OP_JMP, 0U,
                               uemit_jmp_offset(jmp_past_throw_pc,
                                                rethrow_target)));
        }

        /* OP_THROW e_reg — re-throw the original exception value */
        uemit_throw(e, e_reg, (uint32_t)n->line);
        if (e->error != EMIT_OK) return;

        /* Patch jmp_past_rethrow → here */
        {
            int past_rethrow_target = (int)emit_instr_count(e);
            emit_patch_instr(e, jmp_past_rethrow_pc,
                uinstr_enc_abx(OP_JMP, 0U,
                               uemit_jmp_offset(jmp_past_rethrow_pc,
                                                past_rethrow_target)));
        }
    }

    if (cv_name != NULL && e->current_fs->nactvar > 0) {
        /* refactor-3 FE-05: the catch body may have captured the catch
         * variable; close the upvalue before recycling the register.
         * Mirrors uemit_close_block's has_captured handling. */
        UFuncState *fs = e->current_fs;
        if (fs->actvars[fs->nactvar - 1].is_captured) {
            emit_instr(e, uinstr_enc_abc(OP_CLOSE,
                       (uint8_t)fs->actvars[fs->nactvar - 1].slot, 0U, 0U),
                       (uint32_t)n->line);
        }
        fs->nactvar--;
        fs->freereg = fs_temp_floor(fs);
        e->next_reg = fs->freereg;
    }
}

/* Collapse the 3 near-duplicate try paths (EMIT-033).
 * has_catch = (n->u.try_stmt.catch_body != NULL)
 * has_finally = (n->u.try_stmt.finally_body != NULL)
 * rd = result register (pre-allocated by emit_try_arm).
 *
 * Returns rd on success or 0 on error (e->error set).
 *
 * Wave 6 W5: else_body is emitted inline on the normal-exit path (after
 * TRY_END, before JMP past_handler) so finally still wraps it correctly
 * in the catch+finally case.  Guard logic lives in emit_catch_handler_section. */

/* v0.11.4-D: emit an inline copy of the finally body for the NORMAL
 * (non-unwind) completion path.  The unwind path reaches the finally via the
 * TRY_BEGIN handler_pc + run_cleanup_with_replace (uunwind.c); the normal
 * fall-through path must ALSO run the body — REVIVAL §S5a: "finally runs on
 * every exit kind (return / throw / tag.stop / cancel) regardless."  Mirrors
 * the unwind-copy register/block setup but omits OP_RESUME: on the normal path
 * control simply falls through to the JMP-past-finally that skips the unwind
 * copy.  Runs exactly once per exit (the body either completes normally and
 * reaches this inline copy, or unwinds and reaches the handler copy — never
 * both).  Returns 1 on success, 0 on error (e->error set). */
static int emit_finally_inline(UEmitter *e, UAstNode *n, uint8_t rd) {
    e->next_reg = rd;
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
    if (!uemit_open_block(e, false)) return 0;
    /* refactor-3 VM-02/B4: cleanup bodies are atomic (`|` semantics) — the
     * `;` separator emits no OP_YIELD inside a finally body.  Applies to
     * this normal-path inline copy too, for consistency with the unwind
     * copy.  Save/restore handles nested try/finally inside a finally. */
    {
        uint8_t saved_icb = e->in_cleanup_body;
        e->in_cleanup_body = 1U;
        emit_expr(e, n->u.try_stmt.finally_body);
        e->in_cleanup_body = saved_icb;
    }
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0; }
    if (!uemit_close_block(e)) return 0;
    return 1;
}

static uint8_t emit_try_frame(UEmitter *e, UAstNode *n, uint8_t rd) {
    const int has_catch   = (n->u.try_stmt.catch_body   != NULL);
    const int has_finally = (n->u.try_stmt.finally_body != NULL);
    const int has_else    = (n->u.try_stmt.else_body    != NULL);

    if (has_catch && has_finally) {
        /* === OUTER TRY_FRAME: finally wrapper === */
        int outer_try_begin_pc = (int)emit_instr_count(e);
        uemit_try_begin(e, FLAG_HAS_FINALLY, 0U, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* === INNER TRY_FRAME: catch wrapper === */
        int inner_try_begin_pc = (int)emit_instr_count(e);
        uemit_try_begin(e, FLAG_HAS_CATCH, 0U, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Body */
        e->next_reg = rd;
        e->current_fs->freereg = fs_temp_floor(e->current_fs);
        if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
        emit_expr(e, n->u.try_stmt.body);
        if (e->error != EMIT_OK) return 0U;

        /* OP_TRY_END (inner) */
        uemit_try_end(e, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Optional else body: runs on normal exit, still inside outer finally frame */
        if (has_else) {
            e->next_reg = rd;
            e->current_fs->freereg = fs_temp_floor(e->current_fs);
            if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
            if (!uemit_open_block(e, false)) return 0U;
            emit_expr(e, n->u.try_stmt.else_body);
            if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
            if (!uemit_close_block(e)) return 0U;
        }

        /* JMP past catch */
        int jmp_past_catch_pc = (int)emit_instr_count(e);
        emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Patch inner_try_begin handler_pc → catch handler */
        {
            int catch_target = (int)emit_instr_count(e);
            emit_patch_instr(e, inner_try_begin_pc,
                uinstr_enc_abx(OP_TRY_BEGIN, FLAG_HAS_CATCH,
                               (uint16_t)catch_target));
        }

        /* Catch handler */
        emit_catch_handler_section(e, n);
        if (e->error != EMIT_OK) return 0U;

        /* Patch jmp_past_catch → here (past_catch_pc) */
        {
            int past_catch_target = (int)emit_instr_count(e);
            emit_patch_instr(e, jmp_past_catch_pc,
                uinstr_enc_abx(OP_JMP, 0U,
                               uemit_jmp_offset(jmp_past_catch_pc,
                                                past_catch_target)));
        }

        /* OP_TRY_END (outer) */
        uemit_try_end(e, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* v0.11.4-D: normal-path finally.  Both the normal-completion path and
         * the post-catch path converge here (after the outer TRY_END), so this
         * single inline copy runs the finally body on every non-unwind exit. */
        if (!emit_finally_inline(e, n, rd)) return 0U;

        /* JMP past finally */
        int jmp_past_finally_pc = (int)emit_instr_count(e);
        emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Patch outer_try_begin handler_pc → finally handler */
        {
            int finally_target = (int)emit_instr_count(e);
            emit_patch_instr(e, outer_try_begin_pc,
                uinstr_enc_abx(OP_TRY_BEGIN, FLAG_HAS_FINALLY,
                               (uint16_t)finally_target));
        }

        /* Finally body (unwind copy — atomic per refactor-3 VM-02/B4: a
         * mid-walk OP_YIELD would enqueue the strand while the unwind
         * walker still owns it) */
        e->next_reg = rd;
        e->current_fs->freereg = fs_temp_floor(e->current_fs);
        if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
        if (!uemit_open_block(e, false)) return 0U;
        {
            uint8_t saved_icb = e->in_cleanup_body;
            e->in_cleanup_body = 1U;
            emit_expr(e, n->u.try_stmt.finally_body);
            e->in_cleanup_body = saved_icb;
        }
        if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
        if (!uemit_close_block(e)) return 0U;
        uemit_resume(e, 0U, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Patch jmp_past_finally → here */
        {
            int past_finally_target = (int)emit_instr_count(e);
            emit_patch_instr(e, jmp_past_finally_pc,
                uinstr_enc_abx(OP_JMP, 0U,
                               uemit_jmp_offset(jmp_past_finally_pc,
                                                past_finally_target)));
        }

    } else if (has_catch) {
        /* === Catch-only TRY_FRAME === */
        int try_begin_pc = (int)emit_instr_count(e);
        uemit_try_begin(e, FLAG_HAS_CATCH, 0U, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Body */
        e->next_reg = rd;
        e->current_fs->freereg = fs_temp_floor(e->current_fs);
        if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
        emit_expr(e, n->u.try_stmt.body);
        if (e->error != EMIT_OK) return 0U;

        uemit_try_end(e, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Optional else body: runs on normal exit (after TRY_END, before JMP past handler) */
        if (has_else) {
            e->next_reg = rd;
            e->current_fs->freereg = fs_temp_floor(e->current_fs);
            if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
            if (!uemit_open_block(e, false)) return 0U;
            emit_expr(e, n->u.try_stmt.else_body);
            if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
            if (!uemit_close_block(e)) return 0U;
        }

        /* JMP past handler */
        int jmp_past_handler_pc = (int)emit_instr_count(e);
        emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Patch try_begin handler_pc → catch handler */
        {
            int catch_target = (int)emit_instr_count(e);
            emit_patch_instr(e, try_begin_pc,
                uinstr_enc_abx(OP_TRY_BEGIN, FLAG_HAS_CATCH,
                               (uint16_t)catch_target));
        }

        /* Catch handler */
        emit_catch_handler_section(e, n);
        if (e->error != EMIT_OK) return 0U;

        /* Patch jmp_past_handler → here */
        {
            int past_target = (int)emit_instr_count(e);
            emit_patch_instr(e, jmp_past_handler_pc,
                uinstr_enc_abx(OP_JMP, 0U,
                               uemit_jmp_offset(jmp_past_handler_pc,
                                                past_target)));
        }

    } else {
        /* === Finally-only TRY_FRAME === */
        int try_begin_pc = (int)emit_instr_count(e);
        uemit_try_begin(e, FLAG_HAS_FINALLY, 0U, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Body */
        e->next_reg = rd;
        e->current_fs->freereg = fs_temp_floor(e->current_fs);
        if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
        emit_expr(e, n->u.try_stmt.body);
        if (e->error != EMIT_OK) return 0U;

        uemit_try_end(e, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* v0.11.4-D: normal-path finally — run the body on fall-through before
         * jumping past the unwind copy (REVIVAL §S5a). */
        if (!emit_finally_inline(e, n, rd)) return 0U;

        /* JMP past finally (normal exit path) */
        int jmp_past_finally_pc = (int)emit_instr_count(e);
        emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Patch try_begin handler_pc → finally handler */
        {
            int finally_target = (int)emit_instr_count(e);
            emit_patch_instr(e, try_begin_pc,
                uinstr_enc_abx(OP_TRY_BEGIN, FLAG_HAS_FINALLY,
                               (uint16_t)finally_target));
        }

        /* Finally body (unwind copy — atomic per refactor-3 VM-02/B4, see
         * the catch+finally arm above) */
        e->next_reg = rd;
        e->current_fs->freereg = fs_temp_floor(e->current_fs);
        if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
        if (!uemit_open_block(e, false)) return 0U;
        {
            uint8_t saved_icb = e->in_cleanup_body;
            e->in_cleanup_body = 1U;
            emit_expr(e, n->u.try_stmt.finally_body);
            e->in_cleanup_body = saved_icb;
        }
        if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
        if (!uemit_close_block(e)) return 0U;
        uemit_resume(e, 0U, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Patch jmp_past_finally → here */
        {
            int past_target = (int)emit_instr_count(e);
            emit_patch_instr(e, jmp_past_finally_pc,
                uinstr_enc_abx(OP_JMP, 0U,
                               uemit_jmp_offset(jmp_past_finally_pc,
                                                past_target)));
        }
    }

    /* Emit nil into rd for the "value" of the try expression. */
    e->next_reg = rd;
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), (uint32_t)n->line);
    e->next_reg = rd + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    e->current_fs->freereg = e->next_reg;
    return rd;
}

/* ---------------------------------------------------------------------------
 * emit_throw_arm — AST_THROW dispatch target.
 * throw expr: eval the expression, emit OP_THROW, set pending_unwind.
 * OP_THROW goes to safepoint; urbi_unwind walks the cleanup stack.
 * -------------------------------------------------------------------------- */
uint8_t emit_throw_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    uint8_t val_reg = emit_expr(e, n->u.throw_expr.value);
    if (e->error != EMIT_OK) return 0U;
    uemit_throw(e, val_reg, (uint32_t)n->line);
    /* throw is a statement; return a nil reg for the block's last-stmt logic.
     *
     * EMIT-018 fix (Wave 5, v0.5.7): force next_reg above fs_temp_floor
     * before claiming rd.  Same root cause as EMIT-017 (AST_RETURN
     * bare-return).  Defensive against future arms; current emit-arm
     * contract syncs next_reg to freereg between siblings, so the bug is
     * dormant.  Same fix shape as EMIT-017. */
    {
        uint8_t floor_val = fs_temp_floor(e->current_fs);
        if (e->next_reg < floor_val) e->next_reg = floor_val;
    }
    uint8_t rd = e->next_reg;
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), (uint32_t)n->line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return rd;
}

/* ---------------------------------------------------------------------------
 * emit_try_arm — AST_TRY dispatch target.
 * try { body } [catch (e) { handler }] [finally { cleanup }].
 * Three paths (catch+finally / catch-only / finally-only) are collapsed
 * into emit_try_frame (EMIT-033).
 * -------------------------------------------------------------------------- */
uint8_t emit_try_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    uint8_t rd = e->next_reg;
    return emit_try_frame(e, n, rd);
}

/* ---------------------------------------------------------------------------
 * emit_tag_prefix_arm — AST_TAG_PREFIX dispatch target.
 *
 * mytag: { body }
 *
 * Bytecode layout:
 *   <tag_expr → \x01tag local>
 *   [push_tag_pc]:
 *     OP_PUSH_TAG packed_A, onleave_pc_placeholder  ; onleave_pc=0 at M3
 *     <body opcodes>
 *     OP_POP_TAG  tag_reg
 *     OP_JMP      past_handler_placeholder
 *   [onleave_pc]:   ← OP_PUSH_TAG Bx points here (0 at M3)
 *     (empty — onleave body deferred to M5)
 *   [past_handler_pc]:
 *     <continuation>
 *
 * Register discipline (refactor-3 FE-02 follow-on): the tag value is held
 * for the whole scope, BELOW any body-declared `var`s.  A raw temp there
 * breaks fs_temp_floor's count-based math (nactvar + global_slot_reserved
 * assumes locals are contiguous from the floor): a body local landed one
 * slot ABOVE its counted position and every later temp reset clobbered it.
 * So the tag value is a DECLARED hidden local (`\x01tag`, the for-each
 * `\x01iter` / switch `\x01sw` machinery pattern) in an outer block, and
 * the body keeps its own block so body locals pop at scope end.
 *
 * 4-bit constraint: OP_PUSH_TAG packs tag_reg into A[3:0], so `\x01tag`'s
 * slot must be <= 15.  OP_PUSH_TAG is the ONLY reader of R[tag_reg] (it
 * binds the scope's UTag at push time, v0.10.9-B); OP_POP_TAG ignores its
 * A operand and pops the top cleanup entry — holding the value in the
 * local across the body is for liveness/GC-rooting, not for the pop.
 * When the slot exceeds 15 every lower register is local-occupied, so
 * there is no safe spill target; the EMIT-015 rejection stays:
 * EMIT_TAG_SPILL_OUT_OF_RANGE (widening the encoding to a full byte is a
 * v1.x bytecode change, filed as backlog under T129/Phase 22).
 * -------------------------------------------------------------------------- */
uint8_t emit_tag_prefix_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    uint32_t line = (uint32_t)n->line;
    UFuncState *fs = e->current_fs;

    /* Pre-reserve the global object slot before declaring the hidden tag
     * local — same rationale as emit_for_each_arm / emit_switch_arm (see
     * uemit_reserve_global_slot). */
    if (fs->parent == NULL && !uemit_reserve_global_slot(e)) return 0U;

    /* Open outer block scope: \x01tag lives here as a proper local, so
     * fs_temp_floor stays above it across body temp resets. */
    if (!uemit_open_block(e, /*is_loop=*/false)) return 0U;

    const char *tag_name = ustr_intern(e->vm, "\x01tag", 4);
    if (tag_name == NULL) { uemit_close_block(e); e->error = EMIT_OOM; return 0U; }
    int tag_slot = uemit_declare_local(e, tag_name, 4);
    if (tag_slot < 0) { uemit_close_block(e); return 0U; }

    /* Evaluate tag_expr (will be nil at M3); MOVE the result into \x01tag. */
    uint8_t tag_tmp = emit_expr(e, n->u.tag_prefix.tag_expr);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
    if (tag_tmp != (uint8_t)tag_slot) {
        emit_instr(e, uinstr_enc_abc(OP_MOVE, (uint8_t)tag_slot, tag_tmp, 0U),
                   line);
        if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
    }
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* 4-bit nibble check (EMIT-015, see header comment). */
    if (tag_slot > 15) {
        uemit_close_block(e);
        e->error = EMIT_TAG_SPILL_OUT_OF_RANGE;
        return 0U;
    }
    uint8_t tag_reg = (uint8_t)tag_slot;

    /* Emit OP_PUSH_TAG with placeholder onleave_pc (will be patched). */
    uint8_t flags_m3 = 0U;  /* no FLAG_HAS_ONLEAVE at M3 */
    int push_tag_pc = (int)emit_instr_count(e);
    uemit_push_tag(e, tag_reg, flags_m3, 0U /* placeholder */, line);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }

    /* Body — its own block so body-declared locals pop at scope end and
     * captured ones get an OP_CLOSE on the fall-through path.  The tag
     * scope has NO emitted abnormal exits of its own: tag.stop() and
     * throw unwind via the runtime walker (not emitted JMPs), so only
     * this normal close matters at emit level; break/continue against an
     * enclosing loop are covered by that loop's exit-path closes via
     * has_captured propagation (uemit_close_block). */
    if (!uemit_open_block(e, false)) { uemit_close_block(e); return 0U; }
    uint8_t body_result = emit_expr(e, n->u.tag_prefix.body);
    if (e->error != EMIT_OK) {
        uemit_close_block(e);
        uemit_close_block(e);
        return 0U;
    }
    (void)body_result;
    if (!uemit_close_block(e)) { uemit_close_block(e); return 0U; }
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* Emit OP_POP_TAG. */
    uemit_pop_tag(e, tag_reg, line);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }

    /* Emit OP_JMP past the (empty) onleave handler block. */
    int jmp_past_handler_pc = (int)emit_instr_count(e);
    emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), line);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }

    /* Onleave handler block starts here.
     * At M3, onleave is always NULL — emit nothing; just record the PC. */
    int onleave_target = (int)emit_instr_count(e);

    /* Patch OP_PUSH_TAG Bx to point to onleave handler PC. */
    emit_patch_instr(e, push_tag_pc,
        uinstr_enc_abx(OP_PUSH_TAG,
                       (uint8_t)(((flags_m3 & 0xFU) << 4) | (tag_reg & 0xFU)),
                       (uint16_t)onleave_target));

    /* Past-handler: JMP lands here. */
    {
        int past_handler_target = (int)emit_instr_count(e);
        emit_patch_instr(e, jmp_past_handler_pc,
            uinstr_enc_abx(OP_JMP, 0U,
                           uemit_jmp_offset(jmp_past_handler_pc,
                                            past_handler_target)));
    }

    /* Close outer block (removes \x01tag from scope).  If the body block
     * propagated has_captured up here (uemit_close_block), this emits an
     * OP_CLOSE at \x01tag's slot AFTER the past-handler PC — a no-op on
     * the normal path (the body block already closed its cells) but live
     * on the tag.stop() walker-resume path, which lands at the handler PC
     * past the body block's inline close. */
    if (!uemit_close_block(e)) return 0U;
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* Return a nil register as the tag-prefix's value. */
    uint8_t rd = e->next_reg;
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return rd;
}
