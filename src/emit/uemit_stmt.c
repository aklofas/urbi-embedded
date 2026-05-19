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
 *
 * Also contains the shared function-building primitives:
 *   emit_function_literal — compile a function literal into UProto + OP_CLOSURE
 *   emit_lazy_thunk       — wrap an expression as a zero-arg closure thunk
 *
 * NOTE: The AST_CALL arm deliberately preserves the EMIT-014 uint8_t
 * wraparound at 256+ args.  Do NOT fix EMIT-014 here (wave-5-fixes). */

#include "emit/uemit_internal.h"  /* uemit_internal.h pulls in umacros.h (urbi_zero) */
#include "value/uintern.h"        /* ustr_intern */
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

    /* 4. Compile body (AST_BLOCK); emit_instr routes to child_proto. */
    uint8_t body_reg = emit_expr(e, body);
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
 *     exit: */
uint8_t emit_while_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    int loop_start = (int)emit_instr_count(e);

    /* 1. Compile cond into rx. */
    uint8_t rx = e->next_reg;
    emit_expr(e, n->u.while_stmt.cond);
    if (e->error != EMIT_OK) return 0U;

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
        return 0U;
    }
    {
        UAstNode *body = n->u.while_stmt.body;
        if (!uemit_open_block(e, /*is_loop=*/true)) return 0U;

        for (int i = 0; i < body->u.block.count; i++) {
            emit_expr(e, body->u.block.stmts[i]);
            if (e->error != EMIT_OK) {
                uemit_close_block(e);
                return 0U;
            }
            /* Release temps between body statements; locals stay. */
            e->current_fs->freereg = fs_temp_floor(e->current_fs);
            e->next_reg = e->current_fs->freereg;
        }

        /* 5. OP_CLOSE-on-back-edge if any local in the loop block was captured. */
        uemit_emit_loop_back_close(e);

        /* 6. Back-edge JMP to loop_start. */
        {
            int from_pc = (int)emit_instr_count(e);
            emit_instr(e, uinstr_enc_abx(OP_JMP, 0U,
                                         uemit_jmp_offset(from_pc, loop_start)),
                       (uint32_t)n->line);
        }

        /* 7. Close the loop block (emits OP_CLOSE if has_captured, then pops
              actvars back). */
        if (!uemit_close_block(e)) return 0U;
    }

    /* 8. Patch the exit JMP to current pc. */
    {
        int exit_target = (int)emit_instr_count(e);
        emit_patch_instr(e, jmp_to_exit,
            uinstr_enc_abx(OP_JMP, 0U,
                           uemit_jmp_offset(jmp_to_exit, exit_target)));
    }

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
