/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_unwind.c — unwind / control-transfer bytecode emitters.
 * Extracted from uemit.c during v0.5.4-decompose (EMIT-045 #6).
 *
 * Contains:
 *   - Public encoder helpers for unwind opcodes (uemit_throw, uemit_try_begin,
 *     uemit_try_end, uemit_push_tag, uemit_pop_tag,
 *     uemit_resume, uemit_load_catch_value).
 *   - urbi_emit_expr arm helpers for AST_THROW, AST_TRY, AST_TAG_PREFIX. */

#include "emit/uemit_internal.h"
#include "runtime/ucleanup.h"   /* FLAG_HAS_CATCH, FLAG_HAS_FINALLY */
#include "value/uintern.h"      /* ustr_intern — catch variable interning */
#include "emit/uemit.h"
#include "chunk/uchunk.h"
#include "parse/uast.h"
#include <stddef.h>
#include <stdint.h>

/* =========================================================================
 * Control-transfer opcode encoder helpers.
 * Each function encodes exactly one instruction word and calls urbi_emit_instr.
 * See chunk/uchunk.h §row-7 for the field layout of each opcode.
 * ========================================================================= */

/* OP_THROW ABx: A = reg_value, Bx = 0 (unused). */
void uemit_throw(UEmitter *e, uint8_t reg_value, uint32_t line) {
    urbi_emit_instr(e, uinstr_enc_abx(OP_THROW, reg_value, 0U), line);
}

/* OP_TRY_BEGIN ABx: A = flags byte, Bx = handler PC (16-bit, range 0-65535).
 * flags bits: bit 0 = has_catch, bit 1 = has_finally. */
void uemit_try_begin(UEmitter *e, uint8_t flags, uint16_t handler_pc, uint32_t line) {
    urbi_emit_instr(e, uinstr_enc_abx(OP_TRY_BEGIN, flags, handler_pc), line);
}

/* OP_TRY_END ABC: no operands (all zero). Pops top cleanup entry. */
void uemit_try_end(UEmitter *e, uint32_t line) {
    urbi_emit_instr(e, uinstr_enc_abc(OP_TRY_END, 0U, 0U, 0U), line);
}

/* OP_PUSH_TAG ABx: A packs flags nibble and tag_reg nibble.
 *   A[7:4] = flags (4 bits, values 0-15)
 *   A[3:0] = tag_reg (4 bits, values 0-15)
 *   Bx     = onleave PC (16-bit, range 0-65535)
 * tag_reg must be in [0,15]; flags must be in [0,15].
 * Widening deferred if wider operand ranges become necessary. */
void uemit_push_tag(UEmitter *e, uint8_t reg_tag, uint8_t flags,
                    uint16_t onleave_pc, uint32_t line) {
    uint8_t a = (uint8_t)(((flags & 0xFU) << 4) | (reg_tag & 0xFU));
    urbi_emit_instr(e, uinstr_enc_abx(OP_PUSH_TAG, a, onleave_pc), line);
}

/* OP_POP_TAG ABC: A = reg_tag, B = C = 0. */
void uemit_pop_tag(UEmitter *e, uint8_t reg_tag, uint32_t line) {
    urbi_emit_instr(e, uinstr_enc_abc(OP_POP_TAG, reg_tag, 0U, 0U), line);
}

/* OP_RESUME ABC: A = reg_state, B = C = 0. */
void uemit_resume(UEmitter *e, uint8_t reg_state, uint32_t line) {
    urbi_emit_instr(e, uinstr_enc_abc(OP_RESUME, reg_state, 0U, 0U), line);
}

/* OP_LOAD_CATCH_VALUE ABC: A = destination register, B = C = 0.
 * Loads s->catch_value into R[A] at handler entry. */
void uemit_load_catch_value(UEmitter *e, uint8_t reg, uint32_t line) {
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOAD_CATCH_VALUE, reg, 0U, 0U), line);
}

/* =========================================================================
 * urbi_emit_expr arm helpers for AST_THROW / AST_TRY / AST_TAG_PREFIX.
 * Moved from the monolithic urbi_emit_expr switch (EMIT-045 #6).
 * urbi_emit_try_arm contains the EMIT-033 collapse (emit_try_frame +
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
 * if catch_guard != NULL, emit:
 *   OP_LOAD_CATCH_VALUE → e_reg
 *   <guard expr> → guard_reg
 *   OP_TEST guard_reg, 0, 1   ; skip JMP if guard is truthy (guard passes)
 *   OP_JMP  throw_pc          ; guard failed — re-throw
 *   <catch body>
 *   OP_JMP  past_throw_pc
 *   [throw_pc]: OP_THROW e_reg */
/* Returns the register holding the catch body's result (0 on error, check
 * e->error).  The caller must emit OP_MOVE rd, r_catch immediately after
 * (before any further register allocation) to thread the value through. */
static uint8_t emit_catch_handler_section(UEmitter *e, UAstNode *n) {
    const char *cv_name = NULL;
    uint8_t     e_reg   = 0U;  /* register holding the caught exception value */
    uint8_t     r_catch = 0U;  /* result register of the catch body */
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    if (n->u.try_stmt.catch_var_start != NULL && e->vm != NULL) {
        cv_name = ustr_intern(e->vm,
                              n->u.try_stmt.catch_var_start,
                              (size_t)n->u.try_stmt.catch_var_len);
        if (cv_name == NULL) { e->error = EMIT_OOM; return 0U; }
        int slot = uemit_declare_local(e, cv_name,
                                       n->u.try_stmt.catch_var_len);
        if (slot < 0) return 0U;
        e_reg = (uint8_t)slot;
        uemit_load_catch_value(e, e_reg, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;
    } else {
        e_reg = e->next_reg++;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        uemit_load_catch_value(e, e_reg, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;
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
        uint8_t guard_reg = urbi_emit_expr(e, n->u.try_stmt.catch_guard);
        if (e->error != EMIT_OK) return 0U;

        /* TEST guard_reg, 0, 1 — skip the JMP when guard is truthy (pass) */
        urbi_emit_instr(e, uinstr_enc_abc(OP_TEST, guard_reg, 0U, 1U), (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* JMP to re-throw when guard is falsy (patched after catch body) */
        jmp_past_throw_pc = emit_fwd_jmp(e, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;
    }

    if (!uemit_open_block(e, false)) return 0U;
    r_catch = urbi_emit_expr(e, n->u.try_stmt.catch_body);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
    if (!uemit_close_block(e)) return 0U;

    if (n->u.try_stmt.catch_guard != NULL) {
        /* JMP past the re-throw (catch body has finished, guard passed) */
        int jmp_past_rethrow_pc = emit_fwd_jmp(e, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Patch jmp_past_throw → here: guard failed, re-throw */
        patch_fwd_jmp_here(e, jmp_past_throw_pc);

        /* OP_THROW e_reg — re-throw the original exception value */
        uemit_throw(e, e_reg, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Patch jmp_past_rethrow → here */
        patch_fwd_jmp_here(e, jmp_past_rethrow_pc);
    }

    if (cv_name != NULL && e->current_fs->nactvar > 0) {
        /* refactor-3 FE-05: the catch body may have captured the catch
         * variable; close the upvalue before recycling the register.
         * Mirrors uemit_close_block's has_captured handling. */
        UFuncState *fs = e->current_fs;
        if (fs->actvars[fs->nactvar - 1].is_captured) {
            urbi_emit_instr(e, uinstr_enc_abc(OP_CLOSE,
                       (uint8_t)fs->actvars[fs->nactvar - 1].slot, 0U, 0U),
                       (uint32_t)n->line);
        }
        fs->nactvar--;
        fs->freereg = urbi_emit_fs_temp_floor(fs);
        e->next_reg = fs->freereg;
    }
    return r_catch;
}

/* Collapse the 3 near-duplicate try paths (EMIT-033).
 * has_catch = (n->u.try_stmt.catch_body != NULL)
 * has_finally = (n->u.try_stmt.finally_body != NULL)
 * rd = result register (pre-allocated by urbi_emit_try_arm).
 *
 * Returns rd on success or 0 on error (e->error set).
 *
 * else_body is emitted inline on the normal-exit path (after
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
static int emit_finally_body_at(UEmitter *e, UAstNode *finally_body,
                                uint8_t reg_floor) {
    e->next_reg = reg_floor;
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
    if (e->current_fs->freereg < reg_floor) e->current_fs->freereg = reg_floor;
    if (!uemit_open_block(e, false)) return 0;
    /* refactor-3 VM-02/B4: cleanup bodies are atomic (`|` semantics) — the
     * `;` separator emits no OP_YIELD inside a finally body.  Applies to
     * every inline copy (normal-path, break/continue-crossing) for
     * consistency with the unwind copy.  Save/restore handles nested
     * try/finally inside a finally. */
    {
        uint8_t saved_icb = e->in_cleanup_body;
        e->in_cleanup_body = 1U;
        urbi_emit_expr(e, finally_body);
        e->in_cleanup_body = saved_icb;
    }
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0; }
    if (!uemit_close_block(e)) return 0;
    return 1;
}

static int emit_finally_inline(UEmitter *e, UAstNode *n, uint8_t rd) {
    return emit_finally_body_at(e, n->u.try_stmt.finally_body, rd);
}

/* Emit the teardown for every unwind scope above down_to_depth,
 * innermost-first, at a break/continue site whose JMP crosses them.
 * Tag scope → OP_POP_TAG (runtime pops + tears down the top TAG_SCOPE
 * entry; the A operand is disasm-fidelity only).  Try scope → OP_TRY_END,
 * then — when the scope carries a finally — an inline copy of the finally
 * body (REVIVAL §S5a: finally runs on every exit kind; same mechanism as
 * the v0.11.4-D normal-path copy).  Code-size cost is one copy per
 * crossing site, mirroring the normal-path/unwind-copy duplication.
 * Does NOT modify e->unwind_scope_depth: the scopes stay open for the
 * (unreachable-after-JMP, but still emitted) fall-through path and for
 * sibling break sites. */
int urbi_emit_scope_crossings(UEmitter *e, int down_to_depth, uint32_t line) {
    int d;
    for (d = e->unwind_scope_depth; d > down_to_depth; d--) {
        UUnwindScope *sc = &e->unwind_scopes[d - 1];
        if (sc->kind == (uint8_t)UEMIT_SCOPE_TAG) {
            uemit_pop_tag(e, sc->tag_reg, line);
            if (e->error != EMIT_OK) return 0;
        } else {
            uemit_try_end(e, line);
            if (e->error != EMIT_OK) return 0;
            if (sc->finally_body != NULL) {
                if (!emit_finally_body_at(e, sc->finally_body,
                                          urbi_emit_fs_temp_floor(e->current_fs)))
                    return 0;
            }
        }
    }
    return 1;
}

static uint8_t emit_try_frame(UEmitter *e, UAstNode *n, uint8_t rd) {
    const int has_catch   = (n->u.try_stmt.catch_body   != NULL);
    const int has_finally = (n->u.try_stmt.finally_body != NULL);
    const int has_else    = (n->u.try_stmt.else_body    != NULL);

    /* Reserve rd at entry and default it to nil.  Bodies emit above rd
     * (next_reg starts at rd+1 in each arm), so temp resets inside the
     * bodies cannot reclaim this slot.  Each completing path MOVEs its
     * result into rd before jumping past the handler section. */
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), (uint32_t)n->line);
    if (e->error != EMIT_OK) return 0U;
    e->next_reg = rd + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;

    if (has_catch && has_finally) {
        /* === OUTER TRY_FRAME: finally wrapper === */
        int outer_try_begin_pc = (int)urbi_emit_instr_count(e);
        uemit_try_begin(e, FLAG_HAS_FINALLY, 0U, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;
        /* Outer scope is open until the outer OP_TRY_END below;
         * break/continue inside body / else / catch handler cross it. */
        if (!uemit_unwind_scope_push(e, UEMIT_SCOPE_TRY, 0U,
                                     n->u.try_stmt.finally_body))
            return 0U;

        /* === INNER TRY_FRAME: catch wrapper === */
        int inner_try_begin_pc = (int)urbi_emit_instr_count(e);
        uemit_try_begin(e, FLAG_HAS_CATCH, 0U, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;
        /* Inner (catch) scope covers only the body — the runtime pops
         * it at the inner OP_TRY_END on the normal path and at catch
         * absorption on the unwind path. */
        if (!uemit_unwind_scope_push(e, UEMIT_SCOPE_TRY, 0U, NULL))
            return 0U;

        /* Body — starts at rd+1 so rd stays reserved below body temps. */
        e->next_reg = rd + 1U;
        e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
        if (e->current_fs->freereg < (uint8_t)(rd + 1U))
            e->current_fs->freereg = (uint8_t)(rd + 1U);
        uint8_t r_body = urbi_emit_expr(e, n->u.try_stmt.body);
        if (e->error != EMIT_OK) return 0U;
        /* Thread body result into rd before inner TRY_END. */
        if (r_body != rd) {
            urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, rd, r_body, 0U), (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0U;
        }

        /* OP_TRY_END (inner) */
        uemit_try_end(e, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;
        uemit_unwind_scope_pop(e);   /* inner catch scope closed */

        /* Optional else body: runs on normal exit, still inside outer finally frame */
        if (has_else) {
            e->next_reg = rd + 1U;
            e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
            if (e->current_fs->freereg < (uint8_t)(rd + 1U))
                e->current_fs->freereg = (uint8_t)(rd + 1U);
            if (!uemit_open_block(e, false)) return 0U;
            urbi_emit_expr(e, n->u.try_stmt.else_body);
            if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
            if (!uemit_close_block(e)) return 0U;
        }

        /* JMP past catch */
        int jmp_past_catch_pc = emit_fwd_jmp(e, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Patch inner_try_begin handler_pc → catch handler */
        {
            int catch_target = (int)urbi_emit_instr_count(e);
            urbi_emit_patch_instr(e, inner_try_begin_pc,
                uinstr_enc_abx(OP_TRY_BEGIN, FLAG_HAS_CATCH,
                               (uint16_t)catch_target));
        }

        /* Catch handler — returns the catch body's result register. */
        uint8_t r_catch_cf = emit_catch_handler_section(e, n);
        if (e->error != EMIT_OK) return 0U;
        /* Thread catch result into rd before JMP past catch. */
        if (r_catch_cf != rd) {
            urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, rd, r_catch_cf, 0U),
                       (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0U;
        }

        /* Patch jmp_past_catch → here (past_catch_pc) */
        patch_fwd_jmp_here(e, jmp_past_catch_pc);

        /* OP_TRY_END (outer) */
        uemit_try_end(e, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;
        uemit_unwind_scope_pop(e);   /* outer finally scope closed */

        /* v0.11.4-D: normal-path finally.  Both the normal-completion path and
         * the post-catch path converge here (after the outer TRY_END), so this
         * single inline copy runs the finally body on every non-unwind exit. */
        if (!emit_finally_inline(e, n, (uint8_t)(rd + 1U))) return 0U;

        /* JMP past finally */
        int jmp_past_finally_pc = emit_fwd_jmp(e, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Patch outer_try_begin handler_pc → finally handler */
        {
            int finally_target = (int)urbi_emit_instr_count(e);
            urbi_emit_patch_instr(e, outer_try_begin_pc,
                uinstr_enc_abx(OP_TRY_BEGIN, FLAG_HAS_FINALLY,
                               (uint16_t)finally_target));
        }

        /* Finally body (unwind copy — atomic per refactor-3 VM-02/B4: a
         * mid-walk OP_YIELD would enqueue the strand while the unwind
         * walker still owns it) */
        e->next_reg = rd + 1U;
        e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
        if (e->current_fs->freereg < (uint8_t)(rd + 1U))
            e->current_fs->freereg = (uint8_t)(rd + 1U);
        if (!uemit_open_block(e, false)) return 0U;
        {
            uint8_t saved_icb = e->in_cleanup_body;
            e->in_cleanup_body = 1U;
            urbi_emit_expr(e, n->u.try_stmt.finally_body);
            e->in_cleanup_body = saved_icb;
        }
        if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
        if (!uemit_close_block(e)) return 0U;
        uemit_resume(e, 0U, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Patch jmp_past_finally → here */
        patch_fwd_jmp_here(e, jmp_past_finally_pc);

    } else if (has_catch) {
        /* === Catch-only TRY_FRAME === */
        int try_begin_pc = (int)urbi_emit_instr_count(e);
        uemit_try_begin(e, FLAG_HAS_CATCH, 0U, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;
        /* Catch scope covers only the body (popped at runtime on the
         * normal path by OP_TRY_END, on the unwind path at absorption). */
        if (!uemit_unwind_scope_push(e, UEMIT_SCOPE_TRY, 0U, NULL))
            return 0U;

        /* Body — starts at rd+1 so rd stays reserved below body temps. */
        e->next_reg = rd + 1U;
        e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
        if (e->current_fs->freereg < (uint8_t)(rd + 1U))
            e->current_fs->freereg = (uint8_t)(rd + 1U);
        uint8_t r_body_co = urbi_emit_expr(e, n->u.try_stmt.body);
        if (e->error != EMIT_OK) return 0U;
        /* Thread body result into rd before TRY_END. */
        if (r_body_co != rd) {
            urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, rd, r_body_co, 0U),
                       (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0U;
        }

        uemit_try_end(e, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;
        uemit_unwind_scope_pop(e);   /* catch scope closed */

        /* Optional else body: runs on normal exit (after TRY_END, before JMP past handler) */
        if (has_else) {
            e->next_reg = rd + 1U;
            e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
            if (e->current_fs->freereg < (uint8_t)(rd + 1U))
                e->current_fs->freereg = (uint8_t)(rd + 1U);
            if (!uemit_open_block(e, false)) return 0U;
            urbi_emit_expr(e, n->u.try_stmt.else_body);
            if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
            if (!uemit_close_block(e)) return 0U;
        }

        /* JMP past handler */
        int jmp_past_handler_pc = emit_fwd_jmp(e, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Patch try_begin handler_pc → catch handler */
        {
            int catch_target = (int)urbi_emit_instr_count(e);
            urbi_emit_patch_instr(e, try_begin_pc,
                uinstr_enc_abx(OP_TRY_BEGIN, FLAG_HAS_CATCH,
                               (uint16_t)catch_target));
        }

        /* Catch handler — returns the catch body's result register. */
        uint8_t r_catch_co = emit_catch_handler_section(e, n);
        if (e->error != EMIT_OK) return 0U;
        /* Thread catch result into rd before JMP past handler. */
        if (r_catch_co != rd) {
            urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, rd, r_catch_co, 0U),
                       (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0U;
        }

        /* Patch jmp_past_handler → here */
        patch_fwd_jmp_here(e, jmp_past_handler_pc);

    } else {
        /* === Finally-only TRY_FRAME === */
        int try_begin_pc = (int)urbi_emit_instr_count(e);
        uemit_try_begin(e, FLAG_HAS_FINALLY, 0U, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;
        /* A break/continue inside the body crosses this scope — the
         * crossing site emits OP_TRY_END + an inline finally copy. */
        if (!uemit_unwind_scope_push(e, UEMIT_SCOPE_TRY, 0U,
                                     n->u.try_stmt.finally_body))
            return 0U;

        /* Body — starts at rd+1 so rd stays reserved below body temps. */
        e->next_reg = rd + 1U;
        e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
        if (e->current_fs->freereg < (uint8_t)(rd + 1U))
            e->current_fs->freereg = (uint8_t)(rd + 1U);
        uint8_t r_body_fo = urbi_emit_expr(e, n->u.try_stmt.body);
        if (e->error != EMIT_OK) return 0U;
        /* Thread body result into rd before TRY_END. */
        if (r_body_fo != rd) {
            urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, rd, r_body_fo, 0U),
                       (uint32_t)n->line);
            if (e->error != EMIT_OK) return 0U;
        }

        uemit_try_end(e, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;
        uemit_unwind_scope_pop(e);   /* finally scope closed */

        /* v0.11.4-D: normal-path finally — run the body on fall-through before
         * jumping past the unwind copy (REVIVAL §S5a). */
        if (!emit_finally_inline(e, n, (uint8_t)(rd + 1U))) return 0U;

        /* JMP past finally (normal exit path) */
        int jmp_past_finally_pc = emit_fwd_jmp(e, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Patch try_begin handler_pc → finally handler */
        {
            int finally_target = (int)urbi_emit_instr_count(e);
            urbi_emit_patch_instr(e, try_begin_pc,
                uinstr_enc_abx(OP_TRY_BEGIN, FLAG_HAS_FINALLY,
                               (uint16_t)finally_target));
        }

        /* Finally body (unwind copy — atomic per refactor-3 VM-02/B4, see
         * the catch+finally arm above) */
        e->next_reg = rd + 1U;
        e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
        if (e->current_fs->freereg < (uint8_t)(rd + 1U))
            e->current_fs->freereg = (uint8_t)(rd + 1U);
        if (!uemit_open_block(e, false)) return 0U;
        {
            uint8_t saved_icb = e->in_cleanup_body;
            e->in_cleanup_body = 1U;
            urbi_emit_expr(e, n->u.try_stmt.finally_body);
            e->in_cleanup_body = saved_icb;
        }
        if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
        if (!uemit_close_block(e)) return 0U;
        uemit_resume(e, 0U, (uint32_t)n->line);
        if (e->error != EMIT_OK) return 0U;

        /* Patch jmp_past_finally → here */
        patch_fwd_jmp_here(e, jmp_past_finally_pc);
    }

    /* rd holds the try expression's value (body result or catch result,
     * defaulting to nil if neither path completed). */
    e->next_reg = rd + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return rd;
}

/* ---------------------------------------------------------------------------
 * urbi_emit_throw_arm — AST_THROW dispatch target.
 * throw expr: eval the expression, emit OP_THROW, set pending_unwind.
 * OP_THROW goes to safepoint; urbi_unwind walks the cleanup stack.
 * -------------------------------------------------------------------------- */
uint8_t urbi_emit_throw_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    uint8_t val_reg = urbi_emit_expr(e, n->u.throw_expr.value);
    if (e->error != EMIT_OK) return 0U;
    uemit_throw(e, val_reg, (uint32_t)n->line);
    /* throw is a statement; return a nil reg for the block's last-stmt logic.
     *
     * EMIT-018 fix (Wave 5, v0.5.7): force next_reg above urbi_emit_fs_temp_floor
     * before claiming rd.  Same root cause as EMIT-017 (AST_RETURN
     * bare-return).  Defensive against future arms; current emit-arm
     * contract syncs next_reg to freereg between siblings, so the bug is
     * dormant.  Same fix shape as EMIT-017. */
    {
        uint8_t floor_val = urbi_emit_fs_temp_floor(e->current_fs);
        if (e->next_reg < floor_val) e->next_reg = floor_val;
    }
    uint8_t rd = e->next_reg;
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), (uint32_t)n->line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return rd;
}

/* ---------------------------------------------------------------------------
 * urbi_emit_try_arm — AST_TRY dispatch target.
 * try { body } [catch (e) { handler }] [finally { cleanup }].
 * Three paths (catch+finally / catch-only / finally-only) are collapsed
 * into emit_try_frame (EMIT-033).
 * -------------------------------------------------------------------------- */
uint8_t urbi_emit_try_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    /* Pre-reserve the global object slot before declaring the hidden
     * local (\x01rd) — same rationale as urbi_emit_for_each_arm /
     * urbi_emit_switch_arm (see urbi_emit_reserve_global_slot). */
    if (e->current_fs->parent == NULL && !urbi_emit_reserve_global_slot(e))
        return 0U;

    /* v0.13.5-B/-E: anchor the try result register in nactvar as a
     * declared hidden local (the urbi_emit_tag_prefix_arm pattern) instead of
     * a raw temp.  A raw rd breaks urbi_emit_fs_temp_floor's count-based math
     * (nactvar + global_slot_reserved assumes declared locals are
     * contiguous from the frame base): every local declared inside the
     * try body landed one register ABOVE the computed floor, so each
     * statement-boundary temp reset handed that local's register out as
     * a temp — switch's \x01sw collided with the case-value temp (EQ
     * Rn,Rn: first arm always ran), for-each's hidden locals collided
     * with body temps (runtime TypeError), and a while loop's var was
     * clobbered by the condition's result temp (raising a TypeError that
     * masked the body's thrown value in the catch). */
    if (!uemit_open_block(e, /*is_loop=*/false)) return 0U;
    const char *rd_name = ustr_intern(e->vm, "\x01rd", 3);
    if (rd_name == NULL) { uemit_close_block(e); e->error = EMIT_OOM; return 0U; }
    int rd_slot = uemit_declare_local(e, rd_name, 3);
    if (rd_slot < 0) { uemit_close_block(e); return 0U; }

    uint8_t rd = emit_try_frame(e, n, (uint8_t)rd_slot);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }

    /* Close the anchor block (pops \x01rd from scope) and keep rd
     * allocated for the caller — same epilogue as urbi_emit_tag_prefix_arm. */
    if (!uemit_close_block(e)) return 0U;
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
    e->next_reg = rd + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return rd;
}

/* ---------------------------------------------------------------------------
 * urbi_emit_tag_prefix_arm — AST_TAG_PREFIX dispatch target.
 *
 * mytag: { body }
 *
 * Bytecode layout:
 *   <tag_expr → \x01tag local>
 *   [push_tag_pc]:
 *     OP_PUSH_TAG packed_A, onleave_pc_placeholder  ; onleave_pc=0 initially
 *     <body opcodes>
 *     OP_POP_TAG  tag_reg
 *     OP_JMP      past_handler_placeholder
 *   [onleave_pc]:   ← OP_PUSH_TAG Bx points here (0 initially)
 *     (empty — onleave body deferred)
 *   [past_handler_pc]:
 *     <continuation>
 *
 * Register discipline (refactor-3 FE-02 follow-on, v0.13.4 fix): the
 * result register (rd) and the tag value (\x01tag) must both sit BELOW any
 * body-declared `var`s and below urbi_emit_fs_temp_floor.  A raw temp for rd breaks
 * urbi_emit_fs_temp_floor's count-based math (nactvar + global_slot_reserved assumes
 * declared locals are contiguous from the floor): \x01tag landed AT the
 * floor rather than below it, so the body's first temp reset clobbered it.
 * Both rd and the tag value are therefore DECLARED hidden locals (`\x01rd`
 * and `\x01tag`, the for-each `\x01iter` / switch `\x01sw` machinery
 * pattern) in an outer block, so nactvar counts both and the temp floor
 * lands one above \x01tag:
 *   ... [outer locals] | \x01rd | \x01tag | <-- urbi_emit_fs_temp_floor / body temps
 * The body keeps its own block so body-declared vars pop at scope end.
 *
 * 4-bit constraint: OP_PUSH_TAG packs tag_reg into A[3:0], so `\x01tag`'s
 * slot must be <= 15.  OP_PUSH_TAG is the ONLY reader of R[tag_reg] (it
 * binds the scope's UTag at push time, v0.10.9-B); OP_POP_TAG ignores its
 * A operand and pops the top cleanup entry — holding the value in the
 * local across the body is for liveness/GC-rooting, not for the pop.
 * When the slot exceeds 15 every lower register is local-occupied, so
 * there is no safe spill target; the EMIT-015 rejection stays:
 * EMIT_TAG_SPILL_OUT_OF_RANGE (widening the encoding to a full byte is a
 * v1.x bytecode change, filed as a deferred backlog item).
 * -------------------------------------------------------------------------- */
uint8_t urbi_emit_tag_prefix_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    uint32_t line = (uint32_t)n->line;
    const UFuncState *fs = e->current_fs;

    /* Pre-reserve the global object slot before declaring the hidden
     * locals (\x01rd, \x01tag) — same rationale as urbi_emit_for_each_arm /
     * urbi_emit_switch_arm (see urbi_emit_reserve_global_slot). */
    if (fs->parent == NULL && !urbi_emit_reserve_global_slot(e)) return 0U;

    /* Open outer block scope: \x01rd and \x01tag live here as proper
     * locals so urbi_emit_fs_temp_floor counts both and body temps start above them.
     * Both are popped when this block closes. */
    if (!uemit_open_block(e, /*is_loop=*/false)) return 0U;

    /* Declare \x01rd as a hidden local to anchor rd in nactvar.  This is
     * the fix for the floor-aliasing bug: without it \x01tag lands AT
     * urbi_emit_fs_temp_floor and the body's first temp write clobbers it.  Emit
     * LOADNIL so rd is nil-initialised (same semantic as emit_try_frame). */
    const char *rd_name = ustr_intern(e->vm, "\x01rd", 3);
    if (rd_name == NULL) { uemit_close_block(e); e->error = EMIT_OOM; return 0U; }
    int rd_slot = uemit_declare_local(e, rd_name, 3);
    if (rd_slot < 0) { uemit_close_block(e); return 0U; }
    uint8_t rd = (uint8_t)rd_slot;
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), line);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }

    const char *tag_name = ustr_intern(e->vm, "\x01tag", 4);
    if (tag_name == NULL) { uemit_close_block(e); e->error = EMIT_OOM; return 0U; }
    int tag_slot = uemit_declare_local(e, tag_name, 4);
    if (tag_slot < 0) { uemit_close_block(e); return 0U; }

    /* Evaluate tag_expr (will be nil initially); MOVE the result into \x01tag. */
    uint8_t tag_tmp = urbi_emit_expr(e, n->u.tag_prefix.tag_expr);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
    if (tag_tmp != (uint8_t)tag_slot) {
        urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, (uint8_t)tag_slot, tag_tmp, 0U),
                   line);
        if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
    }
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* 4-bit nibble check (EMIT-015, see header comment). */
    if (tag_slot > 15) {
        uemit_close_block(e);
        e->error = EMIT_TAG_SPILL_OUT_OF_RANGE;
        urbi_emit_diag_error(e, n, "tag scope register spills beyond nibble (16-tag limit)");
        return 0U;
    }
    uint8_t tag_reg = (uint8_t)tag_slot;

    /* Emit OP_PUSH_TAG with placeholder onleave_pc (will be patched). */
    uint8_t tag_flags = 0U;  /* no FLAG_HAS_ONLEAVE initially */
    int push_tag_pc = (int)urbi_emit_instr_count(e);
    uemit_push_tag(e, tag_reg, tag_flags, 0U /* placeholder */, line);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
    /* A break/continue inside the body crosses this tag scope — the
     * crossing site emits an OP_POP_TAG so the runtime TAG_SCOPE entry
     * (and its tag-member link) is torn down before the JMP. */
    if (!uemit_unwind_scope_push(e, UEMIT_SCOPE_TAG, tag_reg, NULL)) {
        uemit_close_block(e);
        return 0U;
    }

    /* Body — its own block so body-declared locals pop at scope end and
     * captured ones get an OP_CLOSE on the fall-through path.  The tag
     * scope has NO emitted abnormal exits of its own: tag.stop() and
     * throw unwind via the runtime walker (not emitted JMPs), so only
     * this normal close matters at emit level; break/continue against an
     * enclosing loop are covered by that loop's exit-path closes via
     * has_captured propagation (uemit_close_block). */
    if (!uemit_open_block(e, false)) { uemit_close_block(e); return 0U; }
    uint8_t body_result = urbi_emit_expr(e, n->u.tag_prefix.body);
    if (e->error != EMIT_OK) {
        uemit_close_block(e);
        uemit_close_block(e);
        return 0U;
    }
    if (!uemit_close_block(e)) { uemit_close_block(e); return 0U; }
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;
    /* Thread body result into rd before OP_POP_TAG teardown.  The value
     * in body_result's slot is still live: close_block only resets
     * bookkeeping (freereg/nactvar), not the VM's register file. */
    if (body_result != rd) {
        urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, rd, body_result, 0U), line);
        if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
    }
    uemit_unwind_scope_pop(e);   /* body done — normal OP_POP_TAG next */

    /* Emit OP_POP_TAG. */
    uemit_pop_tag(e, tag_reg, line);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }

    /* Emit OP_JMP past the (empty) onleave handler block. */
    int jmp_past_handler_pc = emit_fwd_jmp(e, line);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }

    /* Onleave handler block starts here.
     * onleave is always NULL — emit nothing; just record the PC. */
    int onleave_target = (int)urbi_emit_instr_count(e);

    /* Patch OP_PUSH_TAG Bx to point to onleave handler PC. */
    urbi_emit_patch_instr(e, push_tag_pc,
        uinstr_enc_abx(OP_PUSH_TAG,
                       (uint8_t)(((tag_flags & 0xFU) << 4) | (tag_reg & 0xFU)),
                       (uint16_t)onleave_target));

    /* Past-handler: JMP lands here. */
    patch_fwd_jmp_here(e, jmp_past_handler_pc);

    /* Close outer block (removes \x01rd and \x01tag from scope).  If the
     * body block propagated has_captured up here (uemit_close_block), this
     * emits an OP_CLOSE at \x01rd's slot AFTER the past-handler PC — a no-op on
     * the normal path (the body block already closed its cells) but live
     * on the tag.stop() walker-resume path, which lands at the handler PC
     * past the body block's inline close. */
    if (!uemit_close_block(e)) return 0U;
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);

    /* rd holds the body's value (or nil if the body didn't complete).
     * Restore next_reg/freereg so the caller sees rd as allocated. */
    e->next_reg = rd + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return rd;
}
