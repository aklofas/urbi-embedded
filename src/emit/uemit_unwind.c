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
#include "module/umodule.h"
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
 * (if named), emit OP_LOAD_CATCH_VALUE, emit catch_body in a new block,
 * then un-declare the catch var.
 *
 * Called once from the catch+finally path and once from the catch-only path.
 * Both callers have already patched the TRY_BEGIN Bx to the current PC. */
static void emit_catch_handler_section(UEmitter *e, UAstNode *n) {
    const char *cv_name = NULL;
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
        uemit_load_catch_value(e, (uint8_t)slot, (uint32_t)n->line);
        if (e->error != EMIT_OK) return;
    } else {
        uint8_t tmp = e->next_reg++;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        uemit_load_catch_value(e, tmp, (uint32_t)n->line);
        if (e->error != EMIT_OK) return;
    }

    if (!uemit_open_block(e, false)) return;
    emit_expr(e, n->u.try_stmt.catch_body);
    if (e->error != EMIT_OK) { uemit_close_block(e); return; }
    if (!uemit_close_block(e)) return;

    if (cv_name != NULL && e->current_fs->nactvar > 0) {
        e->current_fs->nactvar--;
        e->current_fs->freereg = fs_temp_floor(e->current_fs);
        e->next_reg = e->current_fs->freereg;
    }
}

/* Collapse the 3 near-duplicate try paths (EMIT-033).
 * has_catch = (n->u.try_stmt.catch_body != NULL)
 * has_finally = (n->u.try_stmt.finally_body != NULL)
 * rd = result register (pre-allocated by emit_try_arm).
 *
 * Returns rd on success or 0 on error (e->error set).
 * Emits identical bytecode to the original three inline branches. */
static uint8_t emit_try_frame(UEmitter *e, UAstNode *n, uint8_t rd) {
    const int has_catch   = (n->u.try_stmt.catch_body   != NULL);
    const int has_finally = (n->u.try_stmt.finally_body != NULL);

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

        /* Finally body */
        e->next_reg = rd;
        e->current_fs->freereg = fs_temp_floor(e->current_fs);
        if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
        if (!uemit_open_block(e, false)) return 0U;
        emit_expr(e, n->u.try_stmt.finally_body);
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

        /* Finally body */
        e->next_reg = rd;
        e->current_fs->freereg = fs_temp_floor(e->current_fs);
        if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
        if (!uemit_open_block(e, false)) return 0U;
        emit_expr(e, n->u.try_stmt.finally_body);
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
 * tag_reg is limited to [0,15] by the 4-bit nibble encoding of
 * OP_PUSH_TAG.A[3:0].
 * -------------------------------------------------------------------------- */
uint8_t emit_tag_prefix_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    /* Evaluate tag_expr to get a register (will be nil at M3). */
    uint8_t tag_reg = emit_expr(e, n->u.tag_prefix.tag_expr);
    if (e->error != EMIT_OK) return 0U;

    /* tag_reg must fit in 4 bits for OP_PUSH_TAG encoding.
     *
     * EMIT-015 fix (Wave 5, v0.5.7): pre-fix the spill branch allocated
     * spill = next_reg++ without verifying spill fit in 4 bits — if
     * next_reg was already >= 16 (e.g., function with 16+ locals), the
     * OP_PUSH_TAG packing `((flags<<4) | (reg & 0xF))` silently masked
     * the high bits, producing bytecode that referenced the wrong
     * register at runtime.  Now we reject explicitly; widening the
     * encoding to a full byte is a v1.x bytecode change (filed as
     * backlog under T129/Phase 22). */
    if (tag_reg > 15U) {
        if (e->next_reg > 15U) {
            e->error = EMIT_TAG_SPILL_OUT_OF_RANGE;
            return 0U;
        }
        uint8_t spill = e->next_reg++;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        if (e->current_fs->freereg < e->next_reg)
            e->current_fs->freereg = e->next_reg;
        emit_instr(e, uinstr_enc_abc(OP_MOVE, spill, tag_reg, 0U),
                   (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;
        tag_reg = spill;
    }

    /* Emit OP_PUSH_TAG with placeholder onleave_pc (will be patched). */
    uint8_t flags_m3 = 0U;  /* no FLAG_HAS_ONLEAVE at M3 */
    int push_tag_pc = (int)emit_instr_count(e);
    uemit_push_tag(e, tag_reg, flags_m3, 0U /* placeholder */, (uint32_t)n->line);
    if (e->error != EMIT_OK) return 0U;

    /* Emit body. */
    uint8_t rd = e->next_reg;
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
    if (!uemit_open_block(e, false)) return 0U;
    uint8_t body_result = emit_expr(e, n->u.tag_prefix.body);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
    (void)body_result;
    if (!uemit_close_block(e)) return 0U;

    /* Emit OP_POP_TAG. */
    uemit_pop_tag(e, tag_reg, (uint32_t)n->line);
    if (e->error != EMIT_OK) return 0U;

    /* Emit OP_JMP past the (empty) onleave handler block. */
    int jmp_past_handler_pc = (int)emit_instr_count(e);
    emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);
    if (e->error != EMIT_OK) return 0U;

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

    /* Return a nil register as the tag-prefix's value. */
    e->next_reg = rd;
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), (uint32_t)n->line);
    e->next_reg = rd + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    e->current_fs->freereg = e->next_reg;
    return rd;
}
