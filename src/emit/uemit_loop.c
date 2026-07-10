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
 *   AST_FOR_EACH — urbi_emit_for_each_arm  (while-loop index lowering)
 *   AST_SWITCH   — urbi_emit_switch_arm    (if-else chain lowering)
 */

#include "emit/uemit_internal.h"  /* uemit_internal.h pulls in umacros.h (urbi_zero) */
#include "value/uintern.h"        /* ustr_intern */
#include "value/uarena.h"         /* uarena_alloc (assert message building) */
#include "emit/uemit.h"
#include "chunk/uchunk.h"
#include "parse/uast.h"
#include "runtime/umacros.h"
#include <stddef.h>
#include <stdint.h>

/* === v0.10.5: control-flow emit arms ===
 *
 * urbi_emit_break_arm — AST_BREAK: `break`
 *   Emits a placeholder OP_JMP and records the PC in the innermost loop
 *   context so the enclosing loop can patch it to the exit address.
 *
 * urbi_emit_continue_arm — AST_CONTINUE: `continue`
 *   Emits a placeholder OP_JMP and records the PC in the innermost loop
 *   context so the enclosing loop can patch it to the continue address.
 *
 * urbi_emit_for_each_arm — AST_FOR_EACH: `for (var x : iter) body`
 *   Lowered to a while-loop index pattern:
 *     _iter = iter_expr       ; evaluate iterable once
 *     _n    = _iter.length()  ; number of elements
 *     _i    = 0               ; current index (integer)
 *     loop_start:
 *       TEST (_i < _n)        ; exit if done
 *       JMP <exit>
 *       x = _iter.get(_i)     ; bind loop variable
 *       body                  ; execute body
 *       continue_target:
 *       _i = _i + 1           ; advance
 *       JMP loop_start
 *     exit:
 *   No new opcodes.  The var `x` is a proper local in the body's block scope.
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

/* Helper: emit a method call `recv_reg.method_name()` with no args.
 * Returns the result register (next_reg before the call).
 * Uses stack-allocated synthetic AST nodes (same pattern as urbi_emit_lazy_thunk). */
static uint8_t emit_method_call_0arg(UEmitter *e, uint8_t recv_reg,
                                      const char *method, int method_len,
                                      uint32_t line) {
    /* Build synthetic: recv_reg.method() */
    UAstNode recv_node;
    urbi_zero(&recv_node, sizeof(recv_node));
    recv_node.kind         = AST_IDENT;   /* placeholder; urbi_emit_call_arm uses recv_reg directly */
    recv_node.line         = (int)line;
    recv_node.col          = 1;

    UAstNode member_node;
    urbi_zero(&member_node, sizeof(member_node));
    member_node.kind                = AST_MEMBER_GET;
    member_node.line                = (int)line;
    member_node.col                 = 1;
    member_node.u.member.recv       = &recv_node;
    member_node.u.member.name_start = method;
    member_node.u.member.name_len   = method_len;
    member_node.u.member.value      = NULL;

    UAstNode call_node;
    urbi_zero(&call_node, sizeof(call_node));
    call_node.kind            = AST_CALL;
    call_node.line            = (int)line;
    call_node.col             = 1;
    call_node.u.call.callee   = &member_node;
    call_node.u.call.args     = NULL;
    call_node.u.call.arg_count = 0;

    /* urbi_emit_call_arm with a method-shaped callee will evaluate the receiver.
     * But the receiver here is a synthetic IDENT node that doesn't resolve
     * to a local — we need to emit OP_SELF with the actual recv_reg.
     * Simplest: inline the OP_SELF + OP_CALL pattern directly. */
    (void)call_node;  /* not passed through urbi_emit_call_arm */

    /* Inline OP_SELF + OP_CALL for zero-arg method call. */
    if (e->current_fs == NULL) { e->error = EMIT_UNSUPPORTED_AST; return 0U; }

    /* Intern the method name as a USymbol for IC. */
    const char *interned = ustr_intern(e->vm, method, (size_t)method_len);
    if (interned == NULL) { e->error = EMIT_OOM; return 0U; }
    USymbol *sym = (USymbol *)interned;
    int ic_idx = uemit_assign_ic_index(e, sym);
    if (ic_idx < 0) return 0U;

    uint8_t dst = e->next_reg;
    if (dst >= 254U) { e->error = EMIT_REG_EXHAUSTED; return 0U; }

    /* OP_SELF dst, recv_reg, ic_idx — writes method into R[dst], recv into R[dst+1]. */
    urbi_emit_instr(e, uinstr_enc_abc(OP_SELF, dst, recv_reg, (uint8_t)ic_idx), line);
    e->next_reg += 2U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;

    /* OP_CALL dst, 2 (method + self), 2 | 0x80 (1 result, method flag). */
    urbi_emit_instr(e, uinstr_enc_abc(OP_CALL, dst, 2U, 2U | 0x80U), line);

    /* Result in R[dst]; cursor stays at dst+2, then we reset to dst+1 as the
     * "next available" since the call leaves result in dst. */
    e->next_reg = dst + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return dst;
}

/* Helper: emit `recv_reg.method_name(arg_reg)` — one-arg method call.
 * Returns the result register. */
static uint8_t emit_method_call_1arg(UEmitter *e, uint8_t recv_reg,
                                      const char *method, int method_len,
                                      uint8_t arg_reg,
                                      uint32_t line) {
    if (e->current_fs == NULL) { e->error = EMIT_UNSUPPORTED_AST; return 0U; }

    const char *interned = ustr_intern(e->vm, method, (size_t)method_len);
    if (interned == NULL) { e->error = EMIT_OOM; return 0U; }
    USymbol *sym = (USymbol *)interned;
    int ic_idx = uemit_assign_ic_index(e, sym);
    if (ic_idx < 0) return 0U;

    uint8_t dst = e->next_reg;
    if (dst >= 253U) { e->error = EMIT_REG_EXHAUSTED; return 0U; }

    /* OP_SELF dst, recv_reg, ic_idx */
    urbi_emit_instr(e, uinstr_enc_abc(OP_SELF, dst, recv_reg, (uint8_t)ic_idx), line);
    e->next_reg += 2U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;

    /* Move arg into R[dst+2]. */
    urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, dst + 2U, arg_reg, 0U), line);
    e->next_reg = dst + 3U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;

    /* OP_CALL dst, 3 (method + self + 1 arg), 2 | 0x80. */
    urbi_emit_instr(e, uinstr_enc_abc(OP_CALL, dst, 3U, 2U | 0x80U), line);

    e->next_reg = dst + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return dst;
}

/* urbi_emit_for_each_arm
 *
 * Lowering of `for (var x : iter) { body }` to an index loop.
 *
 * Register layout (all declared as proper locals so urbi_emit_fs_temp_floor stays
 * above them across the body's temp resets):
 *
 *   outer block:
 *     _iter   slot = freereg_at_entry + 0   (the iterable)
 *     _n      slot = freereg_at_entry + 1   (length, constant through loop)
 *     _i      slot = freereg_at_entry + 2   (current index)
 *
 *   inner block (is_loop=true):
 *     x       slot = freereg_at_outer_end   (loop variable)
 *
 * Bytecode shape:
 *   <eval iter>     → _iter
 *   _n = _iter.length()
 *   _i = 0
 *   loop_start:
 *     LT A=0, _i, _n   ; skip JMP-to-exit when _i < _n
 *     JMP <exit>
 *     x = _iter.get(_i)
 *     <body>
 *     CLOSE (if body captured)
 *     [continue target]
 *     _i = _i + 1
 *     JMP <loop_start>
 *   exit:               ← break PCs and jmp_to_exit all patch here
 */
uint8_t urbi_emit_for_each_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    if (n->u.for_each.body->kind != AST_BLOCK) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    uint32_t line = (uint32_t)n->line;
    const UFuncState *fs = e->current_fs;

    /* Pre-reserve the global object slot before declaring any synthetic
     * loop-state locals, so r_global_slot is pinned at the current freereg
     * (e.g. R0 at chunk-top) BEFORE we declare _iter/_n/_i at
     * freereg+1/+2/+3 (see urbi_emit_reserve_global_slot). */
    if (fs->parent == NULL && !urbi_emit_reserve_global_slot(e)) return 0U;

    /* Open outer block scope: _iter, _n, _i live here as proper locals.
     * This ensures urbi_emit_fs_temp_floor = nactvar + 3 throughout the loop body,
     * preventing any temp-register reset from clobbering loop state. */
    if (!uemit_open_block(e, /*is_loop=*/false)) return 0U;

    /* 1. Declare _iter, evaluate iterable, MOVE result into _iter slot.
     *
     * Declaration order: _iter FIRST so its slot is pinned before iter
     * expression is compiled (iter_tmp = next_reg after declaration). */
    const char *iter_name = ustr_intern(e->vm, "\x01iter", 5);
    if (iter_name == NULL) { uemit_close_block(e); e->error = EMIT_OOM; return 0U; }

    int iter_slot = uemit_declare_local(e, iter_name, 5);
    if (iter_slot < 0) { uemit_close_block(e); return 0U; }

    /* Emit the iterable expression into a temp above iter_slot. */
    uint8_t iter_tmp = e->next_reg;
    urbi_emit_expr(e, n->u.for_each.iter_expr);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
    if (iter_tmp != (uint8_t)iter_slot) {
        urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, (uint8_t)iter_slot, iter_tmp, 0U), line);
    }
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* 2. Declare _n, then compute length(). */
    const char *n_name = ustr_intern(e->vm, "\x01n", 2);
    if (n_name == NULL) { uemit_close_block(e); e->error = EMIT_OOM; return 0U; }
    int n_slot = uemit_declare_local(e, n_name, 2);
    if (n_slot < 0) { uemit_close_block(e); return 0U; }

    uint8_t len_tmp = emit_method_call_0arg(e, (uint8_t)iter_slot, "length", 6, line);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
    if (len_tmp != (uint8_t)n_slot) {
        urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, (uint8_t)n_slot, len_tmp, 0U), line);
    }
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* 3. Declare _i, then load 0. */
    const char *i_name = ustr_intern(e->vm, "\x01i", 2);
    if (i_name == NULL) { uemit_close_block(e); e->error = EMIT_OOM; return 0U; }
    int i_slot = uemit_declare_local(e, i_name, 2);
    if (i_slot < 0) { uemit_close_block(e); return 0U; }

    uint8_t i_reg = (uint8_t)i_slot;   /* alias for clarity */
    uint8_t n_reg = (uint8_t)n_slot;
    {
        uint16_t k0 = urbi_emit_add_const_int(e, 0LL);
        if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
        urbi_emit_instr(e, uinstr_enc_abx(OP_LOADK, i_reg, k0), line);
    }
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* Open loop context for break/continue. */
    if (!uemit_loop_push(e, ULOOP_FRAME_LOOP)) { uemit_close_block(e); return 0U; }

    /* 4. loop_start: check _i < _n.
     * OP_LT A=0, B=i_reg, C=n_reg:
     *   if (i < n) != 0 → skip next (skip JMP-to-exit) when i < n
     *   fall through to JMP-to-exit when i >= n */
    int loop_start = (int)urbi_emit_instr_count(e);
    urbi_emit_instr(e, uinstr_enc_abc(OP_LT, 0U, i_reg, n_reg), line);
    int jmp_to_exit = (int)urbi_emit_instr_count(e);
    urbi_emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), line);

    /* 5. Open inner body block and declare loop variable x. */
    if (!uemit_open_block(e, /*is_loop=*/true)) {
        uemit_loop_pop(e);
        uemit_close_block(e);
        return 0U;
    }

    const char *var_canonical = ustr_intern(e->vm,
                                             n->u.for_each.var_name_start,
                                             (size_t)n->u.for_each.var_name_len);
    if (var_canonical == NULL) {
        uemit_close_block(e); uemit_loop_pop(e); uemit_close_block(e);
        e->error = EMIT_OOM;
        return 0U;
    }
    int var_slot = uemit_declare_local(e, var_canonical, n->u.for_each.var_name_len);
    if (var_slot < 0) {
        uemit_close_block(e); uemit_loop_pop(e); uemit_close_block(e);
        return 0U;
    }

    /* Emit x = _iter.get(_i). */
    uint8_t get_result = emit_method_call_1arg(e, (uint8_t)iter_slot, "get", 3, i_reg, line);
    if (e->error != EMIT_OK) {
        uemit_close_block(e); uemit_loop_pop(e); uemit_close_block(e);
        return 0U;
    }
    if (get_result != (uint8_t)var_slot) {
        urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, (uint8_t)var_slot, get_result, 0U), line);
    }
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* 6. Execute body. */
    UAstNode *body = n->u.for_each.body;
    int bi;
    for (bi = 0; bi < body->u.block.count; bi++) {
        urbi_emit_expr(e, body->u.block.stmts[bi]);
        if (e->error != EMIT_OK) {
            uemit_close_block(e); uemit_loop_pop(e); uemit_close_block(e);
            return 0U;
        }
        e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
        e->next_reg = e->current_fs->freereg;
    }

    /* continue PCs land here — BEFORE the back-edge OP_CLOSE, so
     * `continue` closes the iteration's captured cells and then falls
     * through the inner block close into the _i++ increment
     * (previously cont_target sat between
     * steps 7 and 8 and only worked because step 8's OP_CLOSE happened
     * to be emitted exactly there). */
    {
        int cont_target = (int)urbi_emit_instr_count(e);
        uemit_loop_patch_continues(e, cont_target);
    }

    /* 7. OP_CLOSE on back-edge (if inner body captured any vars). */
    uemit_emit_loop_back_close(e);

    /* 8. Close inner body block before _i++.  Capture the block's close
     * threshold first: breaks jump straight to the exit patch point
     * (step 11), past this close and the increment, so the exit path
     * needs its own OP_CLOSE for the breaking iteration's still-open
     * cells (same guard rationale as
     * uemit_close_block). */
    bool body_captured = false;
    uint8_t body_first_slot = 0U;
    {
        UFuncState *cfs = e->current_fs;
        const UBlockCtx *iblk = &cfs->blocks[cfs->nblocks - 1];
        if (iblk->has_captured && iblk->first_local_idx < cfs->nactvar) {
            body_captured = true;
            body_first_slot = cfs->actvars[iblk->first_local_idx].slot;
        }
    }
    if (!uemit_close_block(e)) {
        uemit_loop_pop(e); uemit_close_block(e);
        return 0U;
    }
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* 9. _i = _i + 1. */
    {
        uint16_t k1 = urbi_emit_add_const_int(e, 1LL);
        if (e->error != EMIT_OK) { uemit_loop_pop(e); uemit_close_block(e); return 0U; }
        uint8_t tmp = e->next_reg;
        urbi_emit_instr(e, uinstr_enc_abx(OP_LOADK, tmp, k1), line);
        e->next_reg++;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        if (e->next_reg > e->current_fs->max_reg_seen)
            e->current_fs->max_reg_seen = e->next_reg;
        urbi_emit_instr(e, uinstr_enc_abc(OP_ADD, i_reg, i_reg, tmp), line);
        e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
        e->next_reg = e->current_fs->freereg;
    }

    /* 10. Back-edge JMP to loop_start (backward encoder —
     *      replaces the local `loop_start + 1` compensation hack). */
    {
        int from_pc = (int)urbi_emit_instr_count(e);
        urbi_emit_instr(e, uinstr_enc_abx(OP_JMP, 0U,
                                     uemit_jmp_offset_backward(from_pc, loop_start)), line);
    }

    /* 11. Patch exit JMP and break PCs.  When the body captured, the
     * exit target lands ON an exit-path OP_CLOSE: a no-op on normal
     * exit (the per-iteration close at step 8 already ran) but required
     * on break paths, which jump here past steps 7-10. */
    {
        int exit_target = (int)urbi_emit_instr_count(e);
        if (body_captured) {
            urbi_emit_instr(e, uinstr_enc_abc(OP_CLOSE, body_first_slot, 0U, 0U),
                       line);
        }
        urbi_emit_patch_instr(e, jmp_to_exit,
            uinstr_enc_abx(OP_JMP, 0U, uemit_jmp_offset(jmp_to_exit, exit_target)));
        uemit_loop_patch_breaks(e, exit_target);
    }

    uemit_loop_pop(e);

    /* 12. Close outer block (removes _iter, _n, _i from scope). */
    if (!uemit_close_block(e)) return 0U;
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* for-each is a statement; return a nil register. */
    uint8_t r = e->next_reg;
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U), line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
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
 * local (`\x01sw`, for-each's `\x01iter` machinery pattern) in an outer
 * block, and each case body opens its own block so its locals pop at
 * case end and captured ones get an OP_CLOSE. */
uint8_t urbi_emit_switch_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    uint32_t line = (uint32_t)n->line;
    const UFuncState *fs = e->current_fs;

    /* Pre-reserve the global object slot before declaring the hidden
     * subject local — same rationale as urbi_emit_for_each_arm (see
     * urbi_emit_reserve_global_slot). */
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
     * urbi_emit_for_each_arm step 11 / urbi_emit_while_arm step 7).  It is emitted
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
/* === end v0.10.5: control-flow emit arms === */
