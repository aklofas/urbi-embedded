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

    /* 1. Allocate a new UProto under the module's nested[] list. */
    UProto *child_proto = umodule_alloc_nested_proto(e->module);
    if (child_proto == NULL) { e->error = EMIT_OOM; return 0U; }
    int proto_idx = (int)(e->module->nested_count - 1);

    /* 2. Open a nested FuncState targeting child_proto. */
    UFuncState *child_fs = uemit_open_function(e, parent_fs);
    if (child_fs == NULL) return 0U;
    child_fs->target_proto = child_proto;

    /* 3. Declare parameters as locals in child_fs. */
    {
        int pi;
        for (pi = 0; pi < nparams; pi++) {
            UAstNode *pn = params[pi];
            const char *cname = ustr_intern(e->vm, pn->u.param.name_start,
                                            (size_t)pn->u.param.name_len);
            if (cname == NULL) { e->error = EMIT_OOM; uemit_close_function(e); return 0U; }
            int slot = uemit_declare_local(e, cname, pn->u.param.name_len);
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
        uint8_t dst = e->current_fs->freereg;
        if (dst >= (uint8_t)(UFS_MAX_REGS - 1)) {
            e->error = EMIT_REG_EXHAUSTED;
            return 0U;
        }
        e->current_fs->freereg++;
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
    emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, 32768U), (uint32_t)n->line);

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
    emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, 32768U), (uint32_t)n->line);

    /* 7. Patch jmp_to_else → current pc (start of else/nil arm). */
    {
        int alt_target = (int)emit_instr_count(e);
        int alt_offset = alt_target - (jmp_to_else + 1);
        emit_patch_instr(e, jmp_to_else,
            uinstr_enc_abx(OP_JMP, 0U, (uint16_t)(32768 + alt_offset)));
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
        int end_offset = end_target - (jmp_to_end + 1);
        emit_patch_instr(e, jmp_to_end,
            uinstr_enc_abx(OP_JMP, 0U, (uint16_t)(32768 + end_offset)));
    }

    /* Advance past rd so callers can free it as a temp if needed. */
    e->next_reg = rd + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs != NULL) {
        if (e->next_reg > e->current_fs->max_reg_seen)
            e->current_fs->max_reg_seen = e->next_reg;
        e->current_fs->freereg = e->next_reg;
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
    emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, 32768U), (uint32_t)n->line);

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
            int back_offset = loop_start - ((int)emit_instr_count(e) + 1);
            emit_instr(e, uinstr_enc_abx(OP_JMP, 0U,
                                         (uint16_t)(32768 + back_offset)),
                       (uint32_t)n->line);
        }

        /* 7. Close the loop block (emits OP_CLOSE if has_captured, then pops
              actvars back). */
        if (!uemit_close_block(e)) return 0U;
    }

    /* 8. Patch the exit JMP to current pc. */
    {
        int exit_target = (int)emit_instr_count(e);
        int exit_offset = exit_target - (jmp_to_exit + 1);
        emit_patch_instr(e, jmp_to_exit,
            uinstr_enc_abx(OP_JMP, 0U, (uint16_t)(32768 + exit_offset)));
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
 * Emits callee into dst register, then args into consecutive registers.
 * OP_CALL A, B, C: R[A] = callee, args at R[A+1..A+B-1], B = nargs+1.
 * Result written to R[A] by OP_RET. */
uint8_t emit_call_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    /* T16: Look up callee's function signature when the callee is a
     * statically-visible local declared with a function literal.
     * Used below to decide whether to wrap each arg as a lazy thunk.
     * T72 extension: also check global_var_sigs for chunk-top globals. */
    UFuncSig *call_sig = NULL;
    if (n->u.call.callee->kind == AST_IDENT && e->vm != NULL) {
        const char *cn = ustr_intern(e->vm,
                                     n->u.call.callee->u.ident.start,
                                     (size_t)n->u.call.callee->u.ident.len);
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

    uint8_t callee_reg = e->next_reg;
    uint8_t callee_r   = emit_expr(e, n->u.call.callee);
    if (e->error != EMIT_OK) return 0U;
    /* Move callee into callee_reg if emit_expr put it elsewhere
     * (shouldn't happen since next_reg == callee_reg on entry, but be safe). */
    if (callee_r != callee_reg) {
        emit_instr(e, uinstr_enc_abc(OP_MOVE, callee_reg, callee_r, 0U),
                   (uint32_t)n->line);
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
            if (param_lazy) {
                /* Sync freereg to next_reg before the thunk emit so
                 * AST_FUNCTION's OP_CLOSURE destination (taken from
                 * freereg) doesn't clobber an already-allocated arg. */
                if (e->current_fs->freereg < e->next_reg)
                    e->current_fs->freereg = e->next_reg;
                /* Lazy position: compile arg as zero-arg thunk. */
                arg_r = emit_lazy_thunk(e, n->u.call.args[ai]);
            } else {
                arg_r = emit_expr(e, n->u.call.args[ai]);
            }
            e->lazy_arg_context = saved_ctx;
            if (e->error != EMIT_OK) return 0U;
            uint8_t expected = callee_reg + 1U + (uint8_t)ai;
            if (arg_r != expected) {
                emit_instr(e, uinstr_enc_abc(OP_MOVE, expected, arg_r, 0U),
                           (uint32_t)n->line);
            }
        }
    }

    /* OP_CALL callee_reg, nargs+1, 2 (1 result expected). */
    uint8_t b = (uint8_t)(n->u.call.arg_count + 1);
    emit_instr(e, uinstr_enc_abc(OP_CALL, callee_reg, b, 2U),
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
        /* Bare `return`: return nil. */
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
