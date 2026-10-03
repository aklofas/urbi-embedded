/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_loop.c — control-flow statement bytecode emitters.
 *
 * Holds the loop / switch / break / continue emit arms extracted from
 * uemit_stmt.c so the loop-context (ULoopCtx patch-list) machinery sits
 * together.  Each arm reaches the dispatcher via the forwarding decls in
 * uemit_internal.h; no new header surface.
 *
 * Contains:
 *   AST_BREAK    — urbi_emit_break_arm
 *   AST_CONTINUE — urbi_emit_continue_arm
 *   AST_SWITCH   — urbi_emit_switch_arm    (if-else chain lowering)
 *
 * `for (var x : iter) body` is no longer an AST kind this file knows
 * about: the parser lowers it to a plain BLOCK/WHILE (see
 * src/parse/uparse_stmt.c, parse_for), so it reaches this emitter as
 * ordinary AST_WHILE/AST_BLOCK nodes.
 */

#include "emit/uemit_internal.h"  /* uemit_internal.h pulls in umacros.h (urbi_zero) */
#include "emit/uintern.h"        /* ustr_intern */
#include "util/uarena.h"         /* uarena_alloc (assert message building) */
#include "emit/uemit.h"
#include "chunk/uchunk.h"
#include "parse/uast.h"
#include "util/umacros.h"
#include <stddef.h>
#include <stdint.h>

/* urbi_emit_break_arm — AST_BREAK: `break`
 *   Emits a placeholder OP_JMP and records the PC in the innermost loop
 *   context so the enclosing loop can patch it to the exit address.
 *
 * urbi_emit_continue_arm — AST_CONTINUE: `continue`
 *   Emits a placeholder OP_JMP and records the PC in the innermost loop
 *   context so the enclosing loop can patch it to the continue address.
 *
 * urbi_emit_switch_arm — AST_SWITCH: `switch (expr) { case v: body ... }`
 *   Lowered to a chain of if-else comparisons:
 *     _sw = expr              ; evaluate discriminant once
 *     if (_sw == v0) { body0 } else
 *     if (_sw == v1) { body1 } else
 *     ...
 *   Equality uses OP_EQ.  break inside a case body exits the switch.
 *   No new opcodes. */

/* urbi_emit_break_arm */
uint8_t urbi_emit_break_arm(UEmitter *e, const UAstNode *n) {
    if (e->loop_depth == 0) {
        /* Parser should have caught this; defensive. */
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    /* The exit JMP must not cross open try/tag scopes without tearing
     * them down — emit OP_POP_TAG / OP_TRY_END (+ inline finally) for every
     * scope opened since the target frame, innermost-first, BEFORE the JMP.
     * break targets the innermost loop/switch frame (top of loop_stack). */
    if (!urbi_emit_scope_crossings(
            e, e->loop_stack[e->loop_depth - 1].unwind_scope_depth_on_enter,
            (uint32_t)n->line))
        return 0U;
    /* Emit placeholder JMP and record the PC for patching. */
    int jmp_pc = (int)urbi_emit_instr_count(e);
    urbi_emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);
    uemit_loop_record_break(e, jmp_pc);

    /* break doesn't produce a value; return a nil register. */
    uint8_t r = e->next_reg;
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U), (uint32_t)n->line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs != NULL && e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;
    return r;
}

/* urbi_emit_continue_arm */
uint8_t urbi_emit_continue_arm(UEmitter *e, const UAstNode *n) {
    if (e->loop_depth == 0) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    /* Same scope-crossing teardown as urbi_emit_break_arm, but the target
     * is the nearest LOOP frame (switch frames are transparent to
     * `continue` — mirror uemit_loop_record_continue's walk).  If no LOOP
     * frame exists the parser already rejected this; defensive no-op. */
    {
        int d;
        for (d = e->loop_depth; d > 0; d--) {
            if (e->loop_stack[d - 1].kind == ULOOP_FRAME_LOOP) {
                if (!urbi_emit_scope_crossings(
                        e,
                        e->loop_stack[d - 1].unwind_scope_depth_on_enter,
                        (uint32_t)n->line))
                    return 0U;
                break;
            }
        }
    }
    int jmp_pc = (int)urbi_emit_instr_count(e);
    urbi_emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);
    uemit_loop_record_continue(e, jmp_pc);

    uint8_t r = e->next_reg;
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U), (uint32_t)n->line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs != NULL && e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;
    return r;
}

/* urbi_emit_switch_arm — AST_SWITCH: switch (expr) { case v1: body1 ... }
 * Lowered to a chain of if/else-if comparisons.
 * break inside any case body exits the switch.
 *
 * Register discipline: the subject is held
 * for the whole statement, BELOW any case-body `var` declarations.  A raw
 * temp there breaks urbi_emit_fs_temp_floor's count-based math (nactvar +
 * global_slot_reserved assumes locals are contiguous from the floor): a
 * case-body local lands one slot ABOVE its counted position and every
 * later temp reset clobbers it.  So the subject is a DECLARED hidden
 * local (`\x01sw`) in an outer block, and each case body opens its own
 * block so its locals pop at case end and captured ones get an OP_CLOSE. */
uint8_t urbi_emit_switch_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    uint32_t line = (uint32_t)n->line;
    const UFuncState *fs = e->current_fs;

    /* Pre-reserve the global object slot before declaring the hidden
     * subject local (see urbi_emit_reserve_global_slot). */
    if (fs->parent == NULL && !urbi_emit_reserve_global_slot(e)) return 0U;

    /* Open outer block scope: \x01sw lives here as a proper local, so
     * urbi_emit_fs_temp_floor stays above it across case-body temp resets. */
    if (!uemit_open_block(e, /*is_loop=*/false)) return 0U;

    const char *sw_name = ustr_intern(e->vm, "\x01sw", 3);
    if (sw_name == NULL) { uemit_close_block(e); e->error = EMIT_OOM; return 0U; }
    int sw_slot = uemit_declare_local(e, sw_name, 3);
    if (sw_slot < 0) { uemit_close_block(e); return 0U; }

    /* 1. Evaluate the switch expression into a temp, MOVE into \x01sw. */
    uint8_t sw_tmp = e->next_reg;
    urbi_emit_expr(e, n->u.switch_stmt.expr);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
    if (sw_tmp != (uint8_t)sw_slot) {
        urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, (uint8_t)sw_slot, sw_tmp, 0U), line);
    }
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    uint8_t sw_reg = (uint8_t)sw_slot;

    /* Open loop context so break exits the switch. */
    if (!uemit_loop_push(e, ULOOP_FRAME_SWITCH)) {
        uemit_close_block(e);
        return 0U;
    }

    /* 2. Chain: layout per case (no fall-through — legacy urbiscript
     *    switch has none; each body ends with an implicit JMP to exit):
     *
     *      val_reg = case_val
     *      OP_EQ 0, sw, val    ; skip JMP-to-next-case when equal
     *      JMP <next_case>     ; not equal: skip this case's body
     *      <open case block>
     *      <body>
     *      <close case block>  ; OP_CLOSE here if the body captured
     *      JMP <exit>          ; done with body
     *    next_case:
     *      ...
     *    exit:
     *      OP_CLOSE case_base  ; exit-path close for break paths (below)
     */

    /* Base register of the case zone: every local declared anywhere inside
     * a case body — whether in the per-case block opened below or in a
     * deeper user `{ }` block via urbi_emit_block_arm — lands at or above this
     * slot, and every enclosing local (incl. \x01sw) sits below it. */
    uint8_t case_base = e->current_fs->freereg;

    int exit_jmps[64];   /* PCs of JMPs that jump to exit after each case body */
    int n_exit_jmps = 0;

    int i;
    for (i = 0; i < n->u.switch_stmt.case_count; i++) {
        /* Compile case value. */
        uint8_t val_reg = e->next_reg;
        urbi_emit_expr(e, n->u.switch_stmt.case_vals[i]);
        if (e->error != EMIT_OK) { uemit_loop_pop(e); uemit_close_block(e); return 0U; }
        e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
        e->next_reg = e->current_fs->freereg;

        /* OP_EQ A=0: if ((B==C) != 0) pc++ → skip JMP-to-next-case when
         * equal; fall through to the JMP when NOT equal. */
        urbi_emit_instr(e, uinstr_enc_abc(OP_EQ, 0U, sw_reg, val_reg), line);

        int jmp_to_next = emit_fwd_jmp(e, line);

        /* Body — in its own block scope so case-body `var` declarations
         * are counted locals (floor math stays consistent) and pop when
         * the case ends. */
        if (!uemit_open_block(e, /*is_loop=*/false)) {
            uemit_loop_pop(e);
            uemit_close_block(e);
            return 0U;
        }

        UAstNode *body = n->u.switch_stmt.case_bodies[i];
        if (body->kind == AST_BLOCK) {
            int j;
            for (j = 0; j < body->u.block.count; j++) {
                urbi_emit_expr(e, body->u.block.stmts[j]);
                if (e->error != EMIT_OK) {
                    uemit_close_block(e);
                    uemit_loop_pop(e);
                    uemit_close_block(e);
                    return 0U;
                }
                e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
                e->next_reg = e->current_fs->freereg;
            }
        } else {
            urbi_emit_expr(e, body);
            if (e->error != EMIT_OK) {
                uemit_close_block(e);
                uemit_loop_pop(e);
                uemit_close_block(e);
                return 0U;
            }
            e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
            e->next_reg = e->current_fs->freereg;
        }

        if (!uemit_close_block(e)) {
            uemit_loop_pop(e);
            uemit_close_block(e);
            return 0U;
        }
        e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
        e->next_reg = e->current_fs->freereg;

        /* Implicit JMP to exit after body (no fall-through). */
        if (n_exit_jmps < 64) {
            exit_jmps[n_exit_jmps] = (int)urbi_emit_instr_count(e);
            n_exit_jmps++;
        } else {
            /* A 65th exit JMP would keep its placeholder
             * offset and fall into the next case's dispatch.  Latch and
             * unwind: the per-case block is already closed here; the loop
             * ctx and the outer \x01sw block are still pending (same
             * cleanup shape as the case-value urbi_emit_expr failure above). */
            e->error = EMIT_PATCH_LIST_FULL;
            urbi_emit_diag_error(e, n, "switch has too many cases (max 64)");
            uemit_loop_pop(e);
            uemit_close_block(e);
            return 0U;
        }
        urbi_emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), line);

        /* Patch jmp_to_next → here (next case). */
        patch_fwd_jmp_here(e, jmp_to_next);
    }

    /* default arm — emitted in the no-match fall-through position so the
     * last case's jmp_to_next lands here when no case matched.  Treated
     * like a regular case body: ends with an implicit JMP to exit. */
    if (n->u.switch_stmt.default_body != NULL) {
        if (!uemit_open_block(e, /*is_loop=*/false)) {
            uemit_loop_pop(e);
            uemit_close_block(e);
            return 0U;
        }

        UAstNode *dbody = n->u.switch_stmt.default_body;
        if (dbody->kind == AST_BLOCK) {
            int j;
            for (j = 0; j < dbody->u.block.count; j++) {
                urbi_emit_expr(e, dbody->u.block.stmts[j]);
                if (e->error != EMIT_OK) {
                    uemit_close_block(e);
                    uemit_loop_pop(e);
                    uemit_close_block(e);
                    return 0U;
                }
                e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
                e->next_reg = e->current_fs->freereg;
            }
        } else {
            urbi_emit_expr(e, dbody);
            if (e->error != EMIT_OK) {
                uemit_close_block(e);
                uemit_loop_pop(e);
                uemit_close_block(e);
                return 0U;
            }
            e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
            e->next_reg = e->current_fs->freereg;
        }

        if (!uemit_close_block(e)) {
            uemit_loop_pop(e);
            uemit_close_block(e);
            return 0U;
        }
        e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
        e->next_reg = e->current_fs->freereg;

        if (n_exit_jmps < 64) {
            exit_jmps[n_exit_jmps] = (int)urbi_emit_instr_count(e);
            n_exit_jmps++;
        } else {
            e->error = EMIT_PATCH_LIST_FULL;
            urbi_emit_diag_error(e, n, "switch has too many cases (max 64)");
            uemit_loop_pop(e);
            uemit_close_block(e);
            return 0U;
        }
        urbi_emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), line);
    }

    /* exit: patch all exit JMPs and break PCs.  The exit target lands ON
     * an exit-path OP_CLOSE at case_base: a no-op for normal completion
     * and the no-match path (every block close already ran; closed cells
     * leave the open-upval list) but required on break paths, which jump
     * here past every pending block close (same exit-path-close shape as
     * urbi_emit_while_arm step 7).  It is emitted
     * UNCONDITIONALLY (when any case or default arm exists) rather than
     * gated on the case block's has_captured: the common `case v: { ... }`
     * body emits through urbi_emit_block_arm, so its captures mark that DEEPER
     * block — invisible here once it closes — while OP_CLOSE's register-
     * address threshold at case_base covers cells from any nesting depth. */
    {
        int exit_target = (int)urbi_emit_instr_count(e);
        if (n->u.switch_stmt.case_count > 0 ||
                n->u.switch_stmt.default_body != NULL) {
            urbi_emit_instr(e, uinstr_enc_abc(OP_CLOSE, case_base, 0U, 0U),
                       line);
        }
        int j;
        for (j = 0; j < n_exit_jmps; j++) {
            urbi_emit_patch_instr(e, exit_jmps[j],
                uinstr_enc_abx(OP_JMP, 0U,
                               uemit_jmp_offset(exit_jmps[j], exit_target)));
        }
        uemit_loop_patch_breaks(e, exit_target);
    }

    uemit_loop_pop(e);

    /* Close outer block (removes \x01sw from scope). */
    if (!uemit_close_block(e)) return 0U;
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* switch is a statement; return a nil register. */
    uint8_t r = e->next_reg;
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U), line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return r;
}
