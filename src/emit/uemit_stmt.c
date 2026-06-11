/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_stmt.c — statement and function bytecode emitters.
 * Extracted from uemit.c during v0.5.4-decompose (EMIT-045 #4).
 *
 * Contains emit_expr arm helpers for:
 *   AST_IF       — conditional expression (if/else)
 *   AST_WHILE    — while loop
 *   AST_CALL     — function call with lazy-arg support
 *   AST_RETURN   — return statement
 *   AST_FUNCTION — function literal (thin caller for emit_function_literal)
 *   AST_ASSERT   — assert(expr) / assert { block } (W3/v0.10.5, legacy F9)
 *
 * Also contains the shared function-building primitives:
 *   emit_function_literal — compile a function literal into UProto + OP_CLOSURE
 *   emit_lazy_thunk       — wrap an expression as a zero-arg closure thunk
 *
 * NOTE: The AST_CALL arm deliberately preserves the EMIT-014 uint8_t
 * wraparound at 256+ args.  Do NOT fix EMIT-014 here (wave-5-fixes). */

#include "emit/uemit_internal.h"  /* uemit_internal.h pulls in umacros.h (urbi_zero) */
#include "value/uintern.h"        /* ustr_intern */
#include "value/uarena.h"         /* uarena_alloc (W3: assert message building) */
#include "emit/uemit.h"
#include "chunk/uchunk.h"
#include "parse/uast.h"
#include "runtime/umacros.h"
#include <stddef.h>
#include <stdint.h>

/* T16: Compile `expr` as a zero-arg closure (lazy thunk).
 * Builds a synthetic AST_FUNCTION wrapping `expr` in a single-statement
 * body, then recurses into the AST_FUNCTION emit arm.  The result register
 * holds a UVAL_CLOSURE.  Upvalue capture falls out naturally from the
 * recursive AST_FUNCTION emit (the upvalue cascade walks parent FuncStates
 * to find any names `expr` references).
 *
 * Pass-through optimization: if `expr` is AST_IDENT that resolves to a
 * lazy local in the current scope, skip re-wrapping and emit a plain
 * OP_MOVE of that slot.  This keeps thunk identity stable across
 * pass-through layers (avoiding thunk-of-a-thunk double-forcing). */
uint8_t emit_lazy_thunk(UEmitter *e, UAstNode *expr) {
    /* Pass-through shortcut: lazy local used as lazy arg — pass the closure
     * register directly without re-wrapping.  Check BEFORE setting
     * lazy_arg_context (which would suppress the is_lazy check in
     * emit_expr/AST_IDENT). */
    if (expr->kind == AST_IDENT && e->vm != NULL && e->current_fs != NULL) {
        const char *canonical = ustr_intern(e->vm, expr->u.ident.start,
                                            (size_t)expr->u.ident.len);
        if (canonical != NULL) {
            UFuncState *fs = e->current_fs;
            for (int i = fs->nactvar - 1; i >= 0; i--) {
                if (fs->actvars[i].name == canonical && fs->actvars[i].is_lazy) {
                    /* Found a lazy local — emit OP_MOVE of its slot directly.
                     * No force, no re-wrap. */
                    uint8_t dst = e->next_reg;
                    if (dst >= (uint8_t)(UFS_MAX_REGS - 1)) {
                        e->error = EMIT_REG_EXHAUSTED;
                        return 0U;
                    }
                    emit_instr(e, uinstr_enc_abc(OP_MOVE, dst,
                                                 fs->actvars[i].slot, 0U),
                               (uint32_t)expr->line);
                    e->next_reg++;
                    if (e->next_reg > e->max_reg_seen)
                        e->max_reg_seen = e->next_reg;
                    if (e->next_reg > fs->max_reg_seen)
                        fs->max_reg_seen = e->next_reg;
                    /* EMIT-013 fix (Wave 5): also raise freereg to
                     * next_reg so a subsequent emit_function_literal
                     * call (which pulls dst from freereg) does not
                     * alias the thunk-pass-through slot at dst. */
                    if (fs->freereg < e->next_reg)
                        fs->freereg = e->next_reg;
                    return dst;
                }
                /* Stop if we found the name but it's not lazy (normal local). */
                if (fs->actvars[i].name == canonical) break;
            }
        }
    }

    /* General case: build a synthetic AST_FUNCTION wrapping `expr`.
     * Stack-allocate the synthetic nodes — they are used only within this
     * call frame and must NOT be allocated from e->arena because the arena
     * may have been reset between statements (the UFuncState and other
     * persistent emitter state also live in the arena; overwriting them
     * would corrupt the compiler state). */
    UAstNode *stmts_arr[1];
    stmts_arr[0] = expr;

    UAstNode body_node;
    urbi_zero(&body_node, sizeof(body_node));
    body_node.kind             = AST_BLOCK;
    body_node.line             = expr->line;
    body_node.col              = expr->col;
    body_node.u.block.stmts   = stmts_arr;
    body_node.u.block.count   = 1;

    UAstNode fn_node;
    urbi_zero(&fn_node, sizeof(fn_node));
    fn_node.kind              = AST_FUNCTION;
    fn_node.line              = expr->line;
    fn_node.col               = expr->col;
    fn_node.u.func.params     = NULL;
    fn_node.u.func.param_count = 0;
    fn_node.u.func.body       = &body_node;

    /* Recurse into AST_FUNCTION emit.  lazy_arg_context must be clear
     * during this call so that any lazy-local reads *inside* the thunk
     * body emit implicit force (they are reads, not arg passes). */
    bool saved_ctx = e->lazy_arg_context;
    e->lazy_arg_context = false;
    uint8_t dst = emit_expr(e, &fn_node);
    e->lazy_arg_context = saved_ctx;
    return dst;
}

/* T30: emit_function_literal — shared helper for AST_FUNCTION and (T33+)
 * watcher/waituntil cond/body/onleave closures.
 * params/nparams describe the formal parameter list (AST_PARAM or
 * AST_LAZY_PARAM nodes).  body must be an AST_BLOCK.  When as_expression
 * is true, the child proto returns its last expression's register value
 * (cond-closure semantics); when false, the child proto returns nil
 * regardless of its last statement (body/onleave closure semantics).
 * Returns the parent register holding the resulting UVAL_CLOSURE, or 0
 * with e->error set on failure.
 * Requires e->current_fs != NULL and e->vm != NULL. */
uint8_t emit_function_literal(UEmitter *e,
                              UAstNode **params, int nparams,
                              UAstNode  *body,
                              bool       as_expression) {
    UFuncState *parent_fs = e->current_fs;

    /* T21 (EMIT-004): intern all parameter names BEFORE allocating the
     * child UProto.  Pre-fix, child_proto was pushed to module->nested[]
     * first and a mid-loop ustr_intern OOM left a half-initialised proto
     * stuck in the array (nested_count incremented, name slots not yet
     * declared, body never compiled).  By interning into a stack-local
     * cache up front, an intern OOM short-circuits with no module-state
     * mutation.  UFS_MAX_LOCALS bounds nparams (the parser caps the
     * formal-list length at 16 today; the bound here is conservative). */
    const char *param_names[UFS_MAX_LOCALS];
    if (nparams > UFS_MAX_LOCALS) {
        e->error = EMIT_REG_EXHAUSTED;
        return 0U;
    }
    for (int pi = 0; pi < nparams; pi++) {
        const UAstNode *pn = params[pi];
        const char *cname = ustr_intern(e->vm, pn->u.param.name_start,
                                        (size_t)pn->u.param.name_len);
        if (cname == NULL) {
            e->error = EMIT_OOM;
            return 0U;
        }
        param_names[pi] = cname;
    }

    /* 1. Allocate a new UProto under the module's nested[] list.  All
     * parameter interns have already succeeded; from here on, any failure
     * leaves child_proto in nested[] but at least it is consistently a
     * fully-allocated empty proto (uchunk_destroy walks NULL slots
     * cleanly). */
    /* v0.8.5 Task 5: allocate child_proto under the ENCLOSING parent's
     * nested[] (parent_fs->target_proto), not flat under root_proto.  For
     * top-level function literals parent_fs->target_proto == root_proto so
     * the on-disk shape is byte-identical to pre-v0.8.5; for nested
     * function literals (function-inside-function) the child UProto becomes
     * a true child of the enclosing function's UProto and OP_CLOSURE Bx is
     * a per-parent index into that scope's nested[].
     *
     * All parameter interns have already succeeded; from here on, any
     * failure leaves child_proto in parent_proto->nested[] but at least
     * it is consistently a fully-allocated empty proto (uchunk_destroy
     * walks NULL slots cleanly). */
    UProto *parent_proto = parent_fs->target_proto;
    if (parent_proto == NULL) parent_proto = e->module;  /* v0.9.2: e->module IS root */
    UProto *child_proto = uproto_alloc_nested(e->module, parent_proto);
    if (child_proto == NULL) { e->error = EMIT_OOM; return 0U; }
    int proto_idx = (int)(parent_proto->nested_count - 1);

    /* 2. Open a nested FuncState targeting child_proto. */
    UFuncState *child_fs = uemit_open_function(e, parent_fs);
    if (child_fs == NULL) return 0U;
    child_fs->target_proto = child_proto;

    /* 3. Declare parameters as locals in child_fs using the pre-interned
     * names.  uemit_declare_local can still fail (EMIT_REG_EXHAUSTED /
     * EMIT_LOCAL_REDECLARE) but no longer competes with intern OOM. */
    {
        int pi;
        for (pi = 0; pi < nparams; pi++) {
            const UAstNode *pn = params[pi];
            int slot = uemit_declare_local(e, param_names[pi],
                                           pn->u.param.name_len);
            if (slot < 0) { uemit_close_function(e); return 0U; }
            if (pn->kind == AST_LAZY_PARAM) {
                child_fs->actvars[slot].is_lazy = true;
            }
        }
    }
    child_proto->nparams = (uint8_t)nparams;

    /* Pre-reserve the realm-global slot register at the current freereg
     * (right above the last param).  This must happen BEFORE body compilation
     * so that if/while temp-resets (which use fs_temp_floor) never clobber
     * the register, even when a global reference first appears inside a
     * branch arm (where the reset has already moved next_reg below freereg).
     *
     * global_slot_reserved = true signals fs_temp_floor to include this
     * register in the floor, whether or not references_global is set yet.
     * OP_LOAD_REALM_GLOBAL is still emitted lazily (only if the body actually
     * reads or writes a global); if the function turns out not to use any
     * globals, the reserved register is simply unused. */
    if (child_fs->freereg < (uint8_t)(UFS_MAX_REGS - 1)) {
        child_fs->r_global_slot = child_fs->freereg;
        child_fs->global_slot_reserved = true;
        child_fs->freereg++;
        if (child_fs->freereg > child_fs->max_reg_seen)
            child_fs->max_reg_seen = child_fs->freereg;
    }

    /* Save the parent's flat-register cursor before clobbering it with
     * the child's.  After the child compile, the parent's `next_reg` is
     * indispensable for finding a closure-destination slot that does not
     * alias any still-live temp in the parent — e.g. the `Realm` GETSLOT
     * result returned by emit_ident_arm's realm-global fallback when
     * compiling `Realm.fn = function () {...}`.  freereg tracks only the
     * local-zone floor (locals + params + r_global_slot); live temps live
     * ABOVE the floor and are tracked by next_reg, so freereg alone is
     * the wrong source.  Task #22, surfaced 2026-05-16. */
    uint8_t parent_next_reg_before = e->next_reg;

    /* Sync the flat register cursor to the child's freereg so temps
     * inside the function body are allocated above all param slots. */
    e->next_reg = child_fs->freereg;

    /* 4. Compile body (AST_BLOCK); emit_instr routes to child_proto.
     *
     * refactor-3 VM-02/B4: clear in_cleanup_body across the nested body —
     * a function literal (or lazy thunk / watcher closure) defined inside a
     * finally body runs LATER as ordinary code, not as part of the cleanup,
     * so its `;` separators keep normal OP_YIELD semantics.  Mirrors the
     * lazy_arg_context save/clear/restore in emit_lazy_thunk. */
    uint8_t saved_icb = e->in_cleanup_body;
    e->in_cleanup_body = 0U;
    uint8_t body_reg = emit_expr(e, body);
    e->in_cleanup_body = saved_icb;
    if (e->error != EMIT_OK) {
        uemit_close_function(e);
        return 0U;
    }

    /* 5. Final OP_RET.  as_expression=true: return body's last result.
     *    as_expression=false: return nil (body runs for side-effects). */
    if (as_expression) {
        emit_instr(e, uinstr_enc_abc(OP_RET, body_reg, 0U, 0U),
                   (uint32_t)body->line);
    } else {
        uint8_t nil_reg = e->next_reg;
        if (nil_reg < child_fs->freereg) nil_reg = child_fs->freereg;
        emit_instr(e, uinstr_enc_abc(OP_LOADNIL, nil_reg, 0U, 0U),
                   (uint32_t)body->line);
        emit_instr(e, uinstr_enc_abc(OP_RET, nil_reg, 0U, 0U),
                   (uint32_t)body->line);
    }

    /* 6. Capture upvalue descriptors before closing child_fs. */
    int nup = child_fs->nupvalues;
    UUpvalDesc upvals_copy[UFS_MAX_UPVALUES];
    {
        int ui;
        for (ui = 0; ui < nup; ui++) {
            upvals_copy[ui] = child_fs->upvalues[ui];
        }
    }

    uemit_close_function(e);   /* pops back to parent_fs */

    /* 7. In parent, emit OP_CLOSURE + nup pseudo-instructions. */
    {
        /* Choose dst above any live parent temp.  Pre-fix this was
         * `current_fs->freereg`, which only covers the local-zone floor
         * — see `parent_next_reg_before` capture above for the full
         * rationale (task #22). */
        uint8_t dst = e->current_fs->freereg;
        if (parent_next_reg_before > dst) dst = parent_next_reg_before;
        if (dst >= (uint8_t)(UFS_MAX_REGS - 1)) {
            e->error = EMIT_REG_EXHAUSTED;
            return 0U;
        }
        e->current_fs->freereg = (uint8_t)(dst + 1U);
        if (e->current_fs->freereg > e->current_fs->max_reg_seen)
            e->current_fs->max_reg_seen = e->current_fs->freereg;
        e->next_reg = e->current_fs->freereg;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;

        emit_instr(e, uinstr_enc_abx(OP_CLOSURE, dst, (uint16_t)proto_idx),
                   (uint32_t)body->line);
        {
            int ui;
            for (ui = 0; ui < nup; ui++) {
                UUpvalDesc *ud = &upvals_copy[ui];
                emit_instr(e,
                    uinstr_enc_abc(OP_MOVE, 0U,
                                   ud->in_stack ? 1U : 0U,
                                   (uint8_t)ud->idx),
                    (uint32_t)body->line);
            }
        }
        return dst;
    }
}

/* emit_if_arm — AST_IF: if (cond) then-block [else else-block]
 *
 *   With else:
 *     TEST  rx, 0, 1       ; skip JMP if cond is truthy
 *     JMP   else_target    ; jump here when falsy
 *     <then-block>         ; result in rd
 *     JMP   end_target     ; skip else
 *     else_target:
 *     <else-block>         ; result in rd
 *     end_target:
 *
 *   Without else:
 *     TEST  rx, 0, 1       ; skip JMP if cond is truthy
 *     JMP   nil_target     ; jump here when falsy
 *     <then-block>         ; result in rd
 *     JMP   end_target     ; skip nil-load
 *     nil_target:
 *     LOADNIL rd
 *     end_target:
 *
 *   Both arms are compiled with next_reg = rd so they write
 *   their result into rd.  The if-expr returns rd.
 *
 *   OP_TEST polarity: "if (truthy(R[A]) == C) pc++" so C=1 skips
 *   (falls into then-block) when truthy; C=0 would skip when falsy.
 *   C=1 is correct for if-then. */
uint8_t emit_if_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    /* 1. rd is the result register; cond is compiled into rx >= rd.
          Since cond may use multiple regs (e.g. a comparison), compile
          it first (at current next_reg), record the base as rd, then
          reset next_reg back to rd before each arm. */
    uint8_t rd = e->next_reg;

    /* Compile cond starting at rd. */
    uint8_t rx = rd;
    uint8_t cond_reg = emit_expr(e, n->u.if_stmt.cond);
    if (e->error != EMIT_OK) return 0U;
    (void)cond_reg;  /* rx == cond_reg */

    /* 2. TEST rx, 0, 1 — skip next instr (JMP) when cond is truthy. */
    emit_instr(e, uinstr_enc_abc(OP_TEST, rx, 0U, 1U), (uint32_t)n->line);

    /* 3. JMP placeholder to else/nil target (patched later). */
    int jmp_to_else = (int)emit_instr_count(e);
    emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);

    /* 4. Reset cursor to rd so then-block allocates starting at rd. */
    e->next_reg = rd;
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;

    /* 5. Compile then-block. */
    uint8_t then_r = emit_expr(e, n->u.if_stmt.then_block);
    if (e->error != EMIT_OK) return 0U;
    if (then_r != rd) {
        emit_instr(e, uinstr_enc_abc(OP_MOVE, rd, then_r, 0U),
                   (uint32_t)n->line);
    }

    /* 6. JMP past else/nil-load to end (patched later). */
    int jmp_to_end = (int)emit_instr_count(e);
    emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);

    /* 7. Patch jmp_to_else → current pc (start of else/nil arm). */
    {
        int alt_target = (int)emit_instr_count(e);
        emit_patch_instr(e, jmp_to_else,
            uinstr_enc_abx(OP_JMP, 0U,
                           uemit_jmp_offset(jmp_to_else, alt_target)));
    }

    /* 8. Reset cursor to rd for else/nil arm. */
    e->next_reg = rd;
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;

    /* 9. Compile else-block or emit LOADNIL. */
    if (n->u.if_stmt.else_block != NULL) {
        uint8_t else_r = emit_expr(e, n->u.if_stmt.else_block);
        if (e->error != EMIT_OK) return 0U;
        if (else_r != rd) {
            emit_instr(e, uinstr_enc_abc(OP_MOVE, rd, else_r, 0U),
                       (uint32_t)n->line);
        }
    } else {
        emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U),
                   (uint32_t)n->line);
    }

    /* 10. Patch jmp_to_end → current pc. */
    {
        int end_target = (int)emit_instr_count(e);
        emit_patch_instr(e, jmp_to_end,
            uinstr_enc_abx(OP_JMP, 0U,
                           uemit_jmp_offset(jmp_to_end, end_target)));
    }

    /* Advance next_reg past rd so callers can allocate above the result
     * via alloc_reg.  Match emit_compare_arm's protocol: rd is a TEMP
     * (the if-expr's value), not a local.  Do NOT bump fs->freereg —
     * forcing freereg = rd + 1 leaks slot rd into the local-zone floor
     * for siblings that route through fs->freereg (e.g., subsequent
     * uemit_declare_local under SEP_SEMI between-stmt handling, which
     * uses fs->freereg as the next local's slot index).
     *
     * EMIT-016 fix (Wave 5, v0.5.7): pre-fix the trailing
     * `fs->freereg = next_reg` line forced a `var b = init` after
     * `if (cond) { var x = init; x };` to land at slot rd+1 (e.g., 3)
     * instead of the actually-free slot rd (e.g., 2), wasting a register
     * across the function's lifetime — the leak compounds across nested
     * conditionals, inflating proto.max_reg unnecessarily.
     *
     * The if-expr's caller is responsible for the rd register: the
     * NARY/BLOCK between-stmt reset releases rd via fs_temp_floor; the
     * assign-arm and similar consumers read rd before allocating new
     * temps. */
    e->next_reg = rd + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs != NULL) {
        if (e->next_reg > e->current_fs->max_reg_seen)
            e->current_fs->max_reg_seen = e->next_reg;
    }

    return rd;
}

/* emit_while_arm — AST_WHILE: while (cond) { body }
 *
 *   Loop structure:
 *     loop_start:
 *       <cond>               ; result in rx
 *       TEST rx, 0, 1        ; skip JMP-to-exit when cond is truthy
 *       JMP <exit>           ; exit when falsy
 *       <body stmts>         ; body block opened with is_loop=true
 *       emit_loop_back_close ; OP_CLOSE if any local captured
 *       JMP loop_start       ; back-edge
 *     exit:
 *
 * W1/v0.10.5: pushes a ULoopCtx so break/continue inside the body are
 * lowered to OP_JMP with the targets patched here at exit. */
uint8_t emit_while_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    /* W1/v0.10.5: open loop context for break/continue. */
    if (!uemit_loop_push(e, ULOOP_FRAME_LOOP)) return 0U;

    int loop_start = (int)emit_instr_count(e);

    /* 1. Compile cond into rx. */
    uint8_t rx = e->next_reg;
    emit_expr(e, n->u.while_stmt.cond);
    if (e->error != EMIT_OK) { uemit_loop_pop(e); return 0U; }

    /* 2. TEST rx, 0, 1 — skip JMP-to-exit when cond is truthy. */
    emit_instr(e, uinstr_enc_abc(OP_TEST, rx, 0U, 1U), (uint32_t)n->line);

    /* 3. JMP placeholder to exit (patched later). */
    int jmp_to_exit = (int)emit_instr_count(e);
    emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);

    /* Free cond temp; locals beneath rx stay. */
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* 4. Body — open block as is_loop=true (different from AST_BLOCK
          which opens with is_loop=false). */
    if (n->u.while_stmt.body->kind != AST_BLOCK) {
        e->error = EMIT_UNSUPPORTED_AST;
        uemit_loop_pop(e);
        return 0U;
    }
    int exit_target;
    {
        UAstNode *body = n->u.while_stmt.body;
        if (!uemit_open_block(e, /*is_loop=*/true)) { uemit_loop_pop(e); return 0U; }

        int i;
        for (i = 0; i < body->u.block.count; i++) {
            emit_expr(e, body->u.block.stmts[i]);
            if (e->error != EMIT_OK) {
                uemit_close_block(e);
                uemit_loop_pop(e);
                return 0U;
            }
            /* Release temps between body statements; locals stay. */
            e->current_fs->freereg = fs_temp_floor(e->current_fs);
            e->next_reg = e->current_fs->freereg;
        }

        /* W1/v0.10.5: continue PCs land here — BEFORE the back-edge
         * OP_CLOSE, so `continue` closes the iteration's captured cells
         * instead of jumping straight to the back-edge JMP and reusing
         * the still-open cell next iteration (refactor-3 FE-04
         * follow-on). */
        {
            int cont_target = (int)emit_instr_count(e);
            uemit_loop_patch_continues(e, cont_target);
        }

        /* 5. OP_CLOSE-on-back-edge if any local in the loop block was captured. */
        uemit_emit_loop_back_close(e);

        /* 6. Back-edge JMP to loop_start. */
        {
            int from_pc = (int)emit_instr_count(e);
            emit_instr(e, uinstr_enc_abx(OP_JMP, 0U,
                                         uemit_jmp_offset_backward(from_pc, loop_start)),
                       (uint32_t)n->line);
        }

        /* 7. Close the loop block.  exit_target is captured FIRST so the
              cond-false exit JMP and every break land ON the block-exit
              OP_CLOSE that uemit_close_block emits here (after the
              back-edge JMP) — previously they landed past it, leaving the
              instruction dead and the breaking iteration's cells open into
              recycled registers (refactor-3 FE-04 follow-on).  On the
              cond-false path the close is a no-op (the back-edge close
              already ran).  With no captured local no OP_CLOSE is emitted
              and exit_target degenerates to the position after the
              back-edge JMP, exactly as before.  The compile-time
              actvar/freereg pop inside uemit_close_block still happens
              exactly once; patching below uses instruction positions
              only. */
        exit_target = (int)emit_instr_count(e);
        if (!uemit_close_block(e)) { uemit_loop_pop(e); return 0U; }
    }

    /* 8. Patch the exit JMP and break PCs to exit_target. */
    {
        emit_patch_instr(e, jmp_to_exit,
            uinstr_enc_abx(OP_JMP, 0U,
                           uemit_jmp_offset(jmp_to_exit, exit_target)));
        /* W1/v0.10.5: patch break PCs to exit_target. */
        uemit_loop_patch_breaks(e, exit_target);
    }

    uemit_loop_pop(e);

    /* while-loop is a statement; it doesn't produce a value.
       Return a register that holds nil to give callers a valid reg. */
    {
        uint8_t r = e->next_reg;
        emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U),
                   (uint32_t)n->line);
        e->next_reg++;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        if (e->current_fs->freereg < e->next_reg)
            e->current_fs->freereg = e->next_reg;
        return r;
    }
}

/* emit_call_arm — AST_CALL: function call.
 *
 * Two paths (v1.6 S42):
 *   1. Method call (callee is AST_MEMBER_GET, i.e. obj.m(args)):
 *        eval recv into a temp, emit OP_SELF dst, recv, ic_method (which
 *        writes the method into R[dst] and the receiver into R[dst+1]),
 *        emit args into R[dst+2..], then OP_CALL dst, nargs+2, 2|0x80.
 *        The method-flag bit tells the dispatcher to forward R[dst+1] as
 *        `self` to the callee.
 *   2. Plain call (everything else):
 *        eval callee into dst, emit args into R[dst+1..], then
 *        OP_CALL dst, nargs+1, 2.  Dispatcher passes nil as self.
 *
 * Eliminates the S42 silent-elision bug where intervening OP_GETSLOTs in
 * argument evaluation clobbered vm->last_recv before the outer OP_CALL. */
uint8_t emit_call_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    /* EMIT-014 fix (Wave 5, v0.5.7): the OP_CALL B field is a uint8_t
     * holding (nargs + 1) for plain calls or (nargs + 2) for method calls.
     * Reject calls with >= 253 args before any codegen — method calls
     * need the extra self slot, so 253 args + 2 = 255 (the "all-results"
     * sentinel reserved for tail calls) is the safe upper bound for both
     * paths. */
    if (n->u.call.arg_count >= 253) {
        e->error = EMIT_TOO_MANY_ARGS;
        return 0U;
    }

    UAstNode *callee = n->u.call.callee;
    bool is_method   = (callee->kind == AST_MEMBER_GET);

    /* T16: Look up callee's function signature when the callee is a
     * statically-visible local declared with a function literal.
     * Used below to decide whether to wrap each arg as a lazy thunk.
     * T72 extension: also check global_var_sigs for chunk-top globals.
     * Method calls (AST_MEMBER_GET) skip this — the callee is dispatched
     * via OP_SELF and the slot's lazy-param signature is not visible at
     * the call site (call_sig stays NULL → every arg is eager). */
    UFuncSig *call_sig = NULL;
    if (callee->kind == AST_IDENT && e->vm != NULL) {
        const char *cn = ustr_intern(e->vm,
                                     callee->u.ident.start,
                                     (size_t)callee->u.ident.len);
        if (cn != NULL) {
            UFuncState *fs = e->current_fs;
            /* Local lookup first. */
            for (int i = fs->nactvar - 1; i >= 0; i--) {
                if (fs->actvars[i].name == cn) {
                    if (fs->actvar_sigs[i].resolved) {
                        call_sig = &fs->actvar_sigs[i];
                    }
                    break;
                }
            }
            /* T72: global lookup (chunk-top functions not in actvars). */
            if (call_sig == NULL && fs->parent == NULL) {
                for (int gi = 0; gi < fs->n_global_vars; gi++) {
                    if (fs->global_var_names[gi] == cn) {
                        if (fs->global_var_sigs[gi].resolved) {
                            call_sig = &fs->global_var_sigs[gi];
                        }
                        break;
                    }
                }
            }
        }
    }

    uint8_t callee_reg;
    uint8_t arg_base;      /* register offset where the first explicit arg lands */

    if (is_method) {
        /* Method call path.
         *
         * The result of the whole call must land at the entry e->next_reg
         * (call this reg_before) — emit_var_decl_arm and similar arms
         * assert init_reg == reg_before.  So callee_reg = reg_before, and
         * OP_CALL's result overwrites R[callee_reg].
         *
         * Layout when we're done:
         *   R[callee_reg]    = method (then result after OP_CALL)
         *   R[callee_reg+1]  = self
         *   R[callee_reg+2…] = explicit args
         *
         * To get callee_reg at the bottom, reserve callee+self first,
         * then evaluate the receiver into a scratch slot at callee_reg+2,
         * emit OP_SELF (which copies the snapshot into R[callee_reg+1]
         * and writes the method to R[callee_reg]), and finally drop the
         * scratch so explicit args overwrite it. */
        UAstNode *recv_ast = callee->u.member.recv;

        callee_reg = alloc_reg(e);
        if (e->error != EMIT_OK) return 0U;
        uint8_t self_reg = alloc_reg(e);
        if (e->error != EMIT_OK) return 0U;
        (void)self_reg;  /* implicit — OP_SELF writes callee_reg+1 */
        if (e->current_fs->freereg < e->next_reg)
            e->current_fs->freereg = e->next_reg;

        /* Emit the receiver above callee+self.  It can land anywhere
         * (emit_expr may consume more registers transiently); the slot
         * we care about is whatever emit_expr returns.  OP_SELF snapshots
         * R[recv_r] first, so dst can safely alias recv. */
        uint8_t recv_r = emit_expr(e, recv_ast);
        if (e->error != EMIT_OK) return 0U;

        USymbol *name = (USymbol *)ustr_intern(e->vm,
                                               callee->u.member.name_start,
                                               (size_t)callee->u.member.name_len);
        if (name == NULL) { e->error = EMIT_OOM; return 0U; }
        int ic_idx = uemit_assign_ic_index(e, name);
        if (ic_idx < 0) return 0U;

        emit_instr(e, uinstr_enc_abc(OP_SELF, callee_reg, recv_r,
                                     (uint8_t)ic_idx),
                   (uint32_t)n->line);

        /* Free the receiver scratch — OP_SELF already snapshotted self
         * into callee_reg+1.  Args will overwrite the scratch slot
         * (and any transient temps the recv expression used above it). */
        e->next_reg = (uint8_t)(callee_reg + 2U);
        if (e->current_fs->freereg > e->next_reg)
            e->current_fs->freereg = e->next_reg;

        arg_base = 2U;
    } else {
        /* Plain call path — emit callee into callee_reg, args at +1. */
        callee_reg = e->next_reg;
        uint8_t callee_r = emit_expr(e, callee);
        if (e->error != EMIT_OK) return 0U;
        if (callee_r != callee_reg) {
            emit_instr(e, uinstr_enc_abc(OP_MOVE, callee_reg, callee_r, 0U),
                       (uint32_t)n->line);
        }

        arg_base = 1U;
    }

    /* Sync freereg up to next_reg before the arg loop.  When the callee
     * was a local (OP_MOVE, using next_reg-based allocation), freereg
     * still points at the local zone boundary and is behind next_reg.
     * emit_lazy_thunk → AST_FUNCTION emit uses freereg (not next_reg) as
     * the OP_CLOSURE destination, so without this sync it would clobber
     * the callee's register. */
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;

    /* Emit each argument.  For lazy positions, wrap in a thunk.
     * For all args (lazy or eager), set lazy_arg_context so that
     * any lazy-local reads inside the arg expression use pass-through
     * semantics (spec §4.2). */
    {
        int ai;
        bool saved_ctx = e->lazy_arg_context;
        for (ai = 0; ai < n->u.call.arg_count; ai++) {
            bool param_lazy = (call_sig != NULL &&
                               ai < call_sig->nparams &&
                               call_sig->param_is_lazy[ai]);
            /* Pass-through context: suppress implicit force on lazy-local
             * reads appearing directly as call arguments. */
            e->lazy_arg_context = true;
            uint8_t arg_r;
            /* Sync freereg to next_reg BEFORE every arg emit (not just
             * before lazy-thunk arms).  Plain leaf arms (AST_INT /
             * AST_STR / AST_BOOL / AST_NIL) bump only next_reg via
             * alloc_reg; freereg can lag.  When a subsequent arg is an
             * AST_FUNCTION literal, emit_function_literal pulls its
             * OP_CLOSURE destination from freereg — without this sync
             * the closure lands on an already-allocated arg slot and
             * clobbers it.  Pre-T41 this was unexercised; T41 (M6 Wave 2)
             * surfaced it via the synthetic
             * `recv.setProperty("name", "oget", function() body)` arg
             * sequence. */
            if (e->current_fs->freereg < e->next_reg)
                e->current_fs->freereg = e->next_reg;
            if (param_lazy) {
                /* Lazy position: compile arg as zero-arg thunk. */
                arg_r = emit_lazy_thunk(e, n->u.call.args[ai]);
            } else {
                arg_r = emit_expr(e, n->u.call.args[ai]);
            }
            e->lazy_arg_context = saved_ctx;
            if (e->error != EMIT_OK) return 0U;
            uint8_t expected = callee_reg + arg_base + (uint8_t)ai;
            if (arg_r != expected) {
                emit_instr(e, uinstr_enc_abc(OP_MOVE, expected, arg_r, 0U),
                           (uint32_t)n->line);
            }
        }
    }

    /* OP_CALL: B = nargs + arg_base (plain → +1 = callee; method → +2 =
     * callee + self).  C low bits = nresults+1 = 2 (1 result); bit 7
     * (0x80) set for method calls to instruct dispatch to forward
     * R[A+1] as self. */
    uint8_t b = (uint8_t)(n->u.call.arg_count + arg_base);
    uint8_t c = is_method ? 0x82U : 2U;
    emit_instr(e, uinstr_enc_abc(OP_CALL, callee_reg, b, c),
               (uint32_t)n->line);
    /* Result is written to R[callee_reg] by the called function's OP_RET. */
    e->next_reg = callee_reg + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs != NULL && e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;
    return callee_reg;
}

/* emit_return_arm — AST_RETURN: return [expr].
 * Compile the value (or nil if absent), emit OP_RET.
 * Only valid inside a function body (current_fs must be non-NULL and
 * must have a target_proto — top-level return is not meaningful but
 * is not rejected at emit time; OP_RET at top-level exits urbi_vm_run). */
uint8_t emit_return_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    uint8_t ret_reg;
    if (n->u.ret.value != NULL) {
        ret_reg = emit_expr(e, n->u.ret.value);
        if (e->error != EMIT_OK) return 0U;
    } else {
        /* Bare `return`: return nil.
         *
         * EMIT-017 fix (Wave 5, v0.5.7): force next_reg above the
         * funcstate temp floor before alloc_reg.  alloc_reg uses
         * e->next_reg directly; if a future emit arm transiently drops
         * next_reg below fs_temp_floor (= nactvar +
         * global_slot_reserved), the returned slot would alias a live
         * local and the subsequent OP_LOADNIL would clobber it.
         * Defensive against new arms; current emit-arm contract syncs
         * next_reg to freereg between siblings, so the bug is dormant.
         * Same fix shape as EMIT-018 (AST_THROW). */
        {
            uint8_t floor_val = fs_temp_floor(e->current_fs);
            if (e->next_reg < floor_val) e->next_reg = floor_val;
        }
        ret_reg = alloc_reg(e);
        if (e->error != EMIT_OK) return 0U;
        emit_instr(e, uinstr_enc_abc(OP_LOADNIL, ret_reg, 0U, 0U),
                   (uint32_t)n->line);
    }
    emit_instr(e, uinstr_enc_abc(OP_RET, ret_reg, 0U, 0U),
               (uint32_t)n->line);
    /* Return the register so the block's last-stmt-reg logic works.
     * Any instructions after OP_RET in the same block are unreachable
     * but that is allowed (e.g., the function body auto-appends OP_RET). */
    return ret_reg;
}

/* emit_function_arm — AST_FUNCTION: function literal.
 * T30: thin caller — all logic lives in emit_function_literal.
 * as_expression=true preserves original semantics: the child proto
 * returns its last statement's result register (existing M2 behaviour). */
uint8_t emit_function_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    return emit_function_literal(e,
                                 n->u.func.params,
                                 n->u.func.param_count,
                                 n->u.func.body,
                                 /*as_expression=*/true);
}

/* === W3/v0.10.5: assert keyword ===
 * emit_assert_arm — AST_ASSERT: assert(expr) / assert { block }.
 *
 * Lowering (no new opcode needed):
 *
 *   cond_reg = emit_expr(n->u.assert_stmt.expr)
 *   TEST  cond_reg, 0, 1     ; skip JMP when cond is truthy (assertion passes)
 *   JMP   <throw_site>       ; falsy → throw
 *   LOADNIL rd               ; truthy path: assertion passed → result is nil
 *   JMP   <end>              ; skip throw site
 *   <throw_site>:
 *   LOADK  msg_reg, <msg_k>  ; load "assertion failed[: <src>]"
 *   THROW  msg_reg           ; raise; doesn't fall through
 *   <end>:                   ; patching target; result in rd
 *
 * Diagnostic message:
 *   Paren form: "assertion failed: <src_text>"  (source text from parser)
 *   Block form: "assertion failed"
 *
 * System.ndebug: not supported at v1.0; assertions always run.
 * Evaluated-result display: deferred to v1.x. */
uint8_t emit_assert_arm(UEmitter *e, UAstNode *n) {
    static const char kMsgBase[]   = "assertion failed";
    static const char kMsgPrefix[] = "assertion failed: ";
    /* kMsgBaseLen = strlen("assertion failed") = 16 */
#define KASSERT_BASE_LEN   (sizeof(kMsgBase) - 1U)
    /* kMsgPrefixLen = strlen("assertion failed: ") = 18 */
#define KASSERT_PREFIX_LEN (sizeof(kMsgPrefix) - 1U)

    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    /* 1. Build the failure message string.
     *    Paren form: "assertion failed: <src_text>" (arena-allocated).
     *    Block form: "assertion failed"              (static — intern directly). */
    const char *msg_interned;
    if (n->u.assert_stmt.src_text != NULL && n->u.assert_stmt.src_len > 0) {
        size_t msg_len = KASSERT_PREFIX_LEN + (size_t)n->u.assert_stmt.src_len;
        char  *msg_buf = (char *)uarena_alloc(e->arena, msg_len + 1U);
        if (msg_buf == NULL) { e->error = EMIT_OOM; return 0U; }
        emit_memcpy(msg_buf, kMsgPrefix, KASSERT_PREFIX_LEN);
        emit_memcpy(msg_buf + KASSERT_PREFIX_LEN,
                    n->u.assert_stmt.src_text,
                    (size_t)n->u.assert_stmt.src_len);
        msg_buf[msg_len] = '\0';
        msg_interned = ustr_intern(e->vm, msg_buf, msg_len);
    } else {
        msg_interned = ustr_intern(e->vm, kMsgBase, KASSERT_BASE_LEN);
    }
    if (msg_interned == NULL) { e->error = EMIT_OOM; return 0U; }

    /* 2. Add message string to constant pool. */
    const uint16_t msg_k = add_const_str(e, msg_interned);
    if (e->error != EMIT_OK) return 0U;

    /* 3. rd is the result register. */
    uint8_t rd = e->next_reg;

    /* 4. Compile the asserted expression/block into cond_reg. */
    uint8_t cond_reg = emit_expr(e, n->u.assert_stmt.expr);
    if (e->error != EMIT_OK) return 0U;

    /* 5. TEST cond_reg, 0, 1 — skip JMP-to-throw when cond is truthy. */
    emit_instr(e, uinstr_enc_abc(OP_TEST, cond_reg, 0U, 1U), (uint32_t)n->line);

    /* 6. JMP placeholder to throw_site (patched below). */
    int jmp_to_throw = (int)emit_instr_count(e);
    emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);

    /* 7. Truthy path: reset cursor to rd, emit LOADNIL. */
    e->next_reg = rd;
    {
        uint8_t floor_val = fs_temp_floor(e->current_fs);
        if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
        if (e->next_reg < floor_val) e->next_reg = floor_val;
        if (e->next_reg < rd) e->next_reg = rd;
    }
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), (uint32_t)n->line);

    /* 8. JMP placeholder to end (patched below — skip throw site). */
    int jmp_to_end = (int)emit_instr_count(e);
    emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);

    /* 9. Patch jmp_to_throw → here (start of throw_site). */
    {
        int throw_target = (int)emit_instr_count(e);
        emit_patch_instr(e, jmp_to_throw,
            uinstr_enc_abx(OP_JMP, 0U,
                           uemit_jmp_offset(jmp_to_throw, throw_target)));
    }

    /* 10. Throw site: allocate msg_reg fresh, load message string, throw. */
    {
        /* Allocate msg_reg above rd so it doesn't stomp rd. */
        uint8_t msg_reg = rd + 1U;
        if (msg_reg > e->max_reg_seen) e->max_reg_seen = msg_reg;
        if (e->current_fs != NULL && msg_reg > e->current_fs->max_reg_seen)
            e->current_fs->max_reg_seen = msg_reg;
        emit_instr(e, uinstr_enc_abx(OP_LOADK, msg_reg, msg_k), (uint32_t)n->line);
        uemit_throw(e, msg_reg, (uint32_t)n->line);
    }

    /* 11. Patch jmp_to_end → here (past throw site). */
    {
        int end_target = (int)emit_instr_count(e);
        emit_patch_instr(e, jmp_to_end,
            uinstr_enc_abx(OP_JMP, 0U,
                           uemit_jmp_offset(jmp_to_end, end_target)));
    }

    /* 12. Advance next_reg past rd.  Match emit_if_arm protocol. */
    e->next_reg = rd + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs != NULL) {
        if (e->next_reg > e->current_fs->max_reg_seen)
            e->current_fs->max_reg_seen = e->next_reg;
    }
    return rd;

#undef KASSERT_BASE_LEN
#undef KASSERT_PREFIX_LEN
}

/* === W1/v0.10.5: control-flow emit arms ===
 *
 * emit_break_arm — AST_BREAK: `break`
 *   Emits a placeholder OP_JMP and records the PC in the innermost loop
 *   context so the enclosing loop can patch it to the exit address.
 *
 * emit_continue_arm — AST_CONTINUE: `continue`
 *   Emits a placeholder OP_JMP and records the PC in the innermost loop
 *   context so the enclosing loop can patch it to the continue address.
 *
 * emit_for_each_arm — AST_FOR_EACH: `for (var x : iter) body`
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
 * emit_switch_arm — AST_SWITCH: `switch (expr) { case v: body ... }`
 *   Lowered to a chain of if-else comparisons:
 *     _sw = expr              ; evaluate discriminant once
 *     if (_sw == v0) { body0 } else
 *     if (_sw == v1) { body1 } else
 *     ...
 *   Equality uses OP_EQ.  break inside a case body exits the switch.
 *   No new opcodes. */

/* emit_break_arm */
uint8_t emit_break_arm(UEmitter *e, const UAstNode *n) {
    if (e->loop_depth == 0) {
        /* Parser should have caught this; defensive. */
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    /* T24: the exit JMP must not cross open try/tag scopes without tearing
     * them down — emit OP_POP_TAG / OP_TRY_END (+ inline finally) for every
     * scope opened since the target frame, innermost-first, BEFORE the JMP.
     * break targets the innermost loop/switch frame (top of loop_stack). */
    if (!uemit_emit_scope_crossings(
            e, e->loop_stack[e->loop_depth - 1].unwind_scope_depth_on_enter,
            (uint32_t)n->line))
        return 0U;
    /* Emit placeholder JMP and record the PC for patching. */
    int jmp_pc = (int)emit_instr_count(e);
    emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);
    uemit_loop_record_break(e, jmp_pc);

    /* break doesn't produce a value; return a nil register. */
    uint8_t r = e->next_reg;
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U), (uint32_t)n->line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs != NULL && e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;
    return r;
}

/* emit_continue_arm */
uint8_t emit_continue_arm(UEmitter *e, const UAstNode *n) {
    if (e->loop_depth == 0) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    /* T24: same scope-crossing teardown as emit_break_arm, but the target
     * is the nearest LOOP frame (switch frames are transparent to
     * `continue` — mirror uemit_loop_record_continue's walk).  If no LOOP
     * frame exists the parser already rejected this; defensive no-op. */
    {
        int d;
        for (d = e->loop_depth; d > 0; d--) {
            if (e->loop_stack[d - 1].kind == ULOOP_FRAME_LOOP) {
                if (!uemit_emit_scope_crossings(
                        e,
                        e->loop_stack[d - 1].unwind_scope_depth_on_enter,
                        (uint32_t)n->line))
                    return 0U;
                break;
            }
        }
    }
    int jmp_pc = (int)emit_instr_count(e);
    emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);
    uemit_loop_record_continue(e, jmp_pc);

    uint8_t r = e->next_reg;
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U), (uint32_t)n->line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs != NULL && e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;
    return r;
}

/* Helper: emit a method call `recv_reg.method_name()` with no args.
 * Returns the result register (next_reg before the call).
 * Uses stack-allocated synthetic AST nodes (same pattern as emit_lazy_thunk). */
static uint8_t emit_method_call_0arg(UEmitter *e, uint8_t recv_reg,
                                      const char *method, int method_len,
                                      uint32_t line) {
    /* Build synthetic: recv_reg.method() */
    UAstNode recv_node;
    urbi_zero(&recv_node, sizeof(recv_node));
    recv_node.kind         = AST_IDENT;   /* placeholder; emit_call_arm uses recv_reg directly */
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

    /* emit_call_arm with a method-shaped callee will evaluate the receiver.
     * But the receiver here is a synthetic IDENT node that doesn't resolve
     * to a local — we need to emit OP_SELF with the actual recv_reg.
     * Simplest: inline the OP_SELF + OP_CALL pattern directly. */
    (void)call_node;  /* not passed through emit_call_arm */

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
    emit_instr(e, uinstr_enc_abc(OP_SELF, dst, recv_reg, (uint8_t)ic_idx), line);
    e->next_reg += 2U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;

    /* OP_CALL dst, 2 (method + self), 2 | 0x80 (1 result, method flag). */
    emit_instr(e, uinstr_enc_abc(OP_CALL, dst, 2U, 2U | 0x80U), line);

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
    emit_instr(e, uinstr_enc_abc(OP_SELF, dst, recv_reg, (uint8_t)ic_idx), line);
    e->next_reg += 2U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;

    /* Move arg into R[dst+2]. */
    emit_instr(e, uinstr_enc_abc(OP_MOVE, dst + 2U, arg_reg, 0U), line);
    e->next_reg = dst + 3U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;

    /* OP_CALL dst, 3 (method + self + 1 arg), 2 | 0x80. */
    emit_instr(e, uinstr_enc_abc(OP_CALL, dst, 3U, 2U | 0x80U), line);

    e->next_reg = dst + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return dst;
}

/* emit_for_each_arm
 *
 * Lowering of `for (var x : iter) { body }` to an index loop.
 *
 * Register layout (all declared as proper locals so fs_temp_floor stays
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
uint8_t emit_for_each_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    if (n->u.for_each.body->kind != AST_BLOCK) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    uint32_t line = (uint32_t)n->line;
    UFuncState *fs = e->current_fs;

    /* Pre-reserve the global object slot before declaring any synthetic
     * loop-state locals, so r_global_slot is pinned at the current freereg
     * (e.g. R0 at chunk-top) BEFORE we declare _iter/_n/_i at
     * freereg+1/+2/+3 (see uemit_reserve_global_slot). */
    if (fs->parent == NULL && !uemit_reserve_global_slot(e)) return 0U;

    /* Open outer block scope: _iter, _n, _i live here as proper locals.
     * This ensures fs_temp_floor = nactvar + 3 throughout the loop body,
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
    emit_expr(e, n->u.for_each.iter_expr);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
    if (iter_tmp != (uint8_t)iter_slot) {
        emit_instr(e, uinstr_enc_abc(OP_MOVE, (uint8_t)iter_slot, iter_tmp, 0U), line);
    }
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* 2. Declare _n, then compute length(). */
    const char *n_name = ustr_intern(e->vm, "\x01n", 2);
    if (n_name == NULL) { uemit_close_block(e); e->error = EMIT_OOM; return 0U; }
    int n_slot = uemit_declare_local(e, n_name, 2);
    if (n_slot < 0) { uemit_close_block(e); return 0U; }

    uint8_t len_tmp = emit_method_call_0arg(e, (uint8_t)iter_slot, "length", 6, line);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
    if (len_tmp != (uint8_t)n_slot) {
        emit_instr(e, uinstr_enc_abc(OP_MOVE, (uint8_t)n_slot, len_tmp, 0U), line);
    }
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* 3. Declare _i, then load 0. */
    const char *i_name = ustr_intern(e->vm, "\x01i", 2);
    if (i_name == NULL) { uemit_close_block(e); e->error = EMIT_OOM; return 0U; }
    int i_slot = uemit_declare_local(e, i_name, 2);
    if (i_slot < 0) { uemit_close_block(e); return 0U; }

    uint8_t i_reg = (uint8_t)i_slot;   /* alias for clarity */
    uint8_t n_reg = (uint8_t)n_slot;
    {
        uint16_t k0 = add_const_int(e, 0LL);
        if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
        emit_instr(e, uinstr_enc_abx(OP_LOADK, i_reg, k0), line);
    }
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* Open loop context for break/continue. */
    if (!uemit_loop_push(e, ULOOP_FRAME_LOOP)) { uemit_close_block(e); return 0U; }

    /* 4. loop_start: check _i < _n.
     * OP_LT A=0, B=i_reg, C=n_reg:
     *   if (i < n) != 0 → skip next (skip JMP-to-exit) when i < n
     *   fall through to JMP-to-exit when i >= n */
    int loop_start = (int)emit_instr_count(e);
    emit_instr(e, uinstr_enc_abc(OP_LT, 0U, i_reg, n_reg), line);
    int jmp_to_exit = (int)emit_instr_count(e);
    emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), line);

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
        emit_instr(e, uinstr_enc_abc(OP_MOVE, (uint8_t)var_slot, get_result, 0U), line);
    }
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* 6. Execute body. */
    UAstNode *body = n->u.for_each.body;
    int bi;
    for (bi = 0; bi < body->u.block.count; bi++) {
        emit_expr(e, body->u.block.stmts[bi]);
        if (e->error != EMIT_OK) {
            uemit_close_block(e); uemit_loop_pop(e); uemit_close_block(e);
            return 0U;
        }
        e->current_fs->freereg = fs_temp_floor(e->current_fs);
        e->next_reg = e->current_fs->freereg;
    }

    /* continue PCs land here — BEFORE the back-edge OP_CLOSE, so
     * `continue` closes the iteration's captured cells and then falls
     * through the inner block close into the _i++ increment
     * (refactor-3 FE-04 follow-on; previously cont_target sat between
     * steps 7 and 8 and only worked because step 8's OP_CLOSE happened
     * to be emitted exactly there). */
    {
        int cont_target = (int)emit_instr_count(e);
        uemit_loop_patch_continues(e, cont_target);
    }

    /* 7. OP_CLOSE on back-edge (if inner body captured any vars). */
    uemit_emit_loop_back_close(e);

    /* 8. Close inner body block before _i++.  Capture the block's close
     * threshold first: breaks jump straight to the exit patch point
     * (step 11), past this close and the increment, so the exit path
     * needs its own OP_CLOSE for the breaking iteration's still-open
     * cells (refactor-3 FE-04 follow-on; same guard rationale as
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
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* 9. _i = _i + 1. */
    {
        uint16_t k1 = add_const_int(e, 1LL);
        if (e->error != EMIT_OK) { uemit_loop_pop(e); uemit_close_block(e); return 0U; }
        uint8_t tmp = e->next_reg;
        emit_instr(e, uinstr_enc_abx(OP_LOADK, tmp, k1), line);
        e->next_reg++;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        if (e->next_reg > e->current_fs->max_reg_seen)
            e->current_fs->max_reg_seen = e->next_reg;
        emit_instr(e, uinstr_enc_abc(OP_ADD, i_reg, i_reg, tmp), line);
        e->current_fs->freereg = fs_temp_floor(e->current_fs);
        e->next_reg = e->current_fs->freereg;
    }

    /* 10. Back-edge JMP to loop_start (backward encoder — refactor-3 FE-01;
     *      replaces the local `loop_start + 1` compensation hack). */
    {
        int from_pc = (int)emit_instr_count(e);
        emit_instr(e, uinstr_enc_abx(OP_JMP, 0U,
                                     uemit_jmp_offset_backward(from_pc, loop_start)), line);
    }

    /* 11. Patch exit JMP and break PCs.  When the body captured, the
     * exit target lands ON an exit-path OP_CLOSE: a no-op on normal
     * exit (the per-iteration close at step 8 already ran) but required
     * on break paths, which jump here past steps 7-10
     * (refactor-3 FE-04 follow-on). */
    {
        int exit_target = (int)emit_instr_count(e);
        if (body_captured) {
            emit_instr(e, uinstr_enc_abc(OP_CLOSE, body_first_slot, 0U, 0U),
                       line);
        }
        emit_patch_instr(e, jmp_to_exit,
            uinstr_enc_abx(OP_JMP, 0U, uemit_jmp_offset(jmp_to_exit, exit_target)));
        uemit_loop_patch_breaks(e, exit_target);
    }

    uemit_loop_pop(e);

    /* 12. Close outer block (removes _iter, _n, _i from scope). */
    if (!uemit_close_block(e)) return 0U;
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* for-each is a statement; return a nil register. */
    uint8_t r = e->next_reg;
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U), line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return r;
}

/* emit_switch_arm — AST_SWITCH: switch (expr) { case v1: body1 ... }
 * Lowered to a chain of if/else-if comparisons.
 * break inside any case body exits the switch.
 *
 * Register discipline (refactor-3 FE-02 follow-on): the subject is held
 * for the whole statement, BELOW any case-body `var` declarations.  A raw
 * temp there breaks fs_temp_floor's count-based math (nactvar +
 * global_slot_reserved assumes locals are contiguous from the floor): a
 * case-body local lands one slot ABOVE its counted position and every
 * later temp reset clobbers it.  So the subject is a DECLARED hidden
 * local (`\x01sw`, for-each's `\x01iter` machinery pattern) in an outer
 * block, and each case body opens its own block so its locals pop at
 * case end and captured ones get an OP_CLOSE. */
uint8_t emit_switch_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    uint32_t line = (uint32_t)n->line;
    UFuncState *fs = e->current_fs;

    /* Pre-reserve the global object slot before declaring the hidden
     * subject local — same rationale as emit_for_each_arm (see
     * uemit_reserve_global_slot). */
    if (fs->parent == NULL && !uemit_reserve_global_slot(e)) return 0U;

    /* Open outer block scope: \x01sw lives here as a proper local, so
     * fs_temp_floor stays above it across case-body temp resets. */
    if (!uemit_open_block(e, /*is_loop=*/false)) return 0U;

    const char *sw_name = ustr_intern(e->vm, "\x01sw", 3);
    if (sw_name == NULL) { uemit_close_block(e); e->error = EMIT_OOM; return 0U; }
    int sw_slot = uemit_declare_local(e, sw_name, 3);
    if (sw_slot < 0) { uemit_close_block(e); return 0U; }

    /* 1. Evaluate the switch expression into a temp, MOVE into \x01sw. */
    uint8_t sw_tmp = e->next_reg;
    emit_expr(e, n->u.switch_stmt.expr);
    if (e->error != EMIT_OK) { uemit_close_block(e); return 0U; }
    if (sw_tmp != (uint8_t)sw_slot) {
        emit_instr(e, uinstr_enc_abc(OP_MOVE, (uint8_t)sw_slot, sw_tmp, 0U), line);
    }
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
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
     * deeper user `{ }` block via emit_block_arm — lands at or above this
     * slot, and every enclosing local (incl. \x01sw) sits below it. */
    uint8_t case_base = e->current_fs->freereg;

    int exit_jmps[64];   /* PCs of JMPs that jump to exit after each case body */
    int n_exit_jmps = 0;

    int i;
    for (i = 0; i < n->u.switch_stmt.case_count; i++) {
        /* Compile case value. */
        uint8_t val_reg = e->next_reg;
        emit_expr(e, n->u.switch_stmt.case_vals[i]);
        if (e->error != EMIT_OK) { uemit_loop_pop(e); uemit_close_block(e); return 0U; }
        e->current_fs->freereg = fs_temp_floor(e->current_fs);
        e->next_reg = e->current_fs->freereg;

        /* OP_EQ A=0: if ((B==C) != 0) pc++ → skip JMP-to-next-case when
         * equal; fall through to the JMP when NOT equal. */
        emit_instr(e, uinstr_enc_abc(OP_EQ, 0U, sw_reg, val_reg), line);

        int jmp_to_next = (int)emit_instr_count(e);
        emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), line);

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
                emit_expr(e, body->u.block.stmts[j]);
                if (e->error != EMIT_OK) {
                    uemit_close_block(e);
                    uemit_loop_pop(e);
                    uemit_close_block(e);
                    return 0U;
                }
                e->current_fs->freereg = fs_temp_floor(e->current_fs);
                e->next_reg = e->current_fs->freereg;
            }
        } else {
            emit_expr(e, body);
            if (e->error != EMIT_OK) {
                uemit_close_block(e);
                uemit_loop_pop(e);
                uemit_close_block(e);
                return 0U;
            }
            e->current_fs->freereg = fs_temp_floor(e->current_fs);
            e->next_reg = e->current_fs->freereg;
        }

        if (!uemit_close_block(e)) {
            uemit_loop_pop(e);
            uemit_close_block(e);
            return 0U;
        }
        e->current_fs->freereg = fs_temp_floor(e->current_fs);
        e->next_reg = e->current_fs->freereg;

        /* Implicit JMP to exit after body (no fall-through). */
        if (n_exit_jmps < 64) {
            exit_jmps[n_exit_jmps] = (int)emit_instr_count(e);
            n_exit_jmps++;
        } else {
            /* refactor-3 FE-06: a 65th exit JMP would keep its placeholder
             * offset and fall into the next case's dispatch.  Latch and
             * unwind: the per-case block is already closed here; the loop
             * ctx and the outer \x01sw block are still pending (same
             * cleanup shape as the case-value emit_expr failure above). */
            e->error = EMIT_PATCH_LIST_FULL;
            uemit_loop_pop(e);
            uemit_close_block(e);
            return 0U;
        }
        emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), line);

        /* Patch jmp_to_next → here (next case). */
        {
            int next_case = (int)emit_instr_count(e);
            emit_patch_instr(e, jmp_to_next,
                uinstr_enc_abx(OP_JMP, 0U,
                               uemit_jmp_offset(jmp_to_next, next_case)));
        }
    }

    /* exit: patch all exit JMPs and break PCs.  The exit target lands ON
     * an exit-path OP_CLOSE at case_base: a no-op for normal completion
     * and the no-match path (every block close already ran; closed cells
     * leave the open-upval list) but required on break paths, which jump
     * here past every pending block close (same exit-path-close shape as
     * emit_for_each_arm step 11 / emit_while_arm step 7).  It is emitted
     * UNCONDITIONALLY (when any case exists) rather than gated on the
     * case block's has_captured: the common `case v: { ... }` body emits
     * through emit_block_arm, so its captures mark that DEEPER block —
     * invisible here once it closes — while OP_CLOSE's register-address
     * threshold at case_base covers cells from any nesting depth. */
    {
        int exit_target = (int)emit_instr_count(e);
        if (n->u.switch_stmt.case_count > 0) {
            emit_instr(e, uinstr_enc_abc(OP_CLOSE, case_base, 0U, 0U),
                       line);
        }
        int j;
        for (j = 0; j < n_exit_jmps; j++) {
            emit_patch_instr(e, exit_jmps[j],
                uinstr_enc_abx(OP_JMP, 0U,
                               uemit_jmp_offset(exit_jmps[j], exit_target)));
        }
        uemit_loop_patch_breaks(e, exit_target);
    }

    uemit_loop_pop(e);

    /* Close outer block (removes \x01sw from scope). */
    if (!uemit_close_block(e)) return 0U;
    e->current_fs->freereg = fs_temp_floor(e->current_fs);
    e->next_reg = e->current_fs->freereg;

    /* switch is a statement; return a nil register. */
    uint8_t r = e->next_reg;
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U), line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return r;
}
/* === end W1/v0.10.5: control-flow emit arms === */
