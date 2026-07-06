/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_stmt.c — statement and function bytecode emitters.
 * Extracted from uemit.c during v0.5.4-decompose (EMIT-045 #4).
 *
 * Contains urbi_emit_expr arm helpers for:
 *   AST_IF       — conditional expression (if/else)
 *   AST_WHILE    — while loop
 *   AST_CALL     — function call with lazy-arg support
 *   AST_RETURN   — return statement
 *   AST_FUNCTION — function literal (thin caller for urbi_emit_function_literal)
 *   AST_ASSERT   — assert(expr) / assert { block } (W3/v0.10.5, legacy F9)
 *
 * Also contains the shared function-building primitives:
 *   urbi_emit_function_literal — compile a function literal into UProto + OP_CLOSURE
 *   urbi_emit_lazy_thunk       — wrap an expression as a zero-arg closure thunk
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
uint8_t urbi_emit_lazy_thunk(UEmitter *e, UAstNode *expr) {
    /* Pass-through shortcut: lazy local used as lazy arg — pass the closure
     * register directly without re-wrapping.  Check BEFORE setting
     * lazy_arg_context (which would suppress the is_lazy check in
     * urbi_emit_expr/AST_IDENT). */
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
                    urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, dst,
                                                 fs->actvars[i].slot, 0U),
                               (uint32_t)expr->line);
                    e->next_reg++;
                    if (e->next_reg > e->max_reg_seen)
                        e->max_reg_seen = e->next_reg;
                    if (e->next_reg > fs->max_reg_seen)
                        fs->max_reg_seen = e->next_reg;
                    /* EMIT-013 fix (Wave 5): also raise freereg to
                     * next_reg so a subsequent urbi_emit_function_literal
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
    uint8_t dst = urbi_emit_expr(e, &fn_node);
    e->lazy_arg_context = saved_ctx;
    return dst;
}

/* T30: urbi_emit_function_literal — shared helper for AST_FUNCTION and (T33+)
 * watcher/waituntil cond/body/onleave closures.
 * params/nparams describe the formal parameter list (AST_PARAM or
 * AST_LAZY_PARAM nodes).  body must be an AST_BLOCK.  When as_expression
 * is true, the child proto returns its last expression's register value
 * (cond-closure semantics); when false, the child proto returns nil
 * regardless of its last statement (body/onleave closure semantics).
 * Returns the parent register holding the resulting UVAL_CLOSURE, or 0
 * with e->error set on failure.
 * Requires e->current_fs != NULL and e->vm != NULL. */
uint8_t urbi_emit_function_literal(UEmitter *e,
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
     * mutation.  UFS_MAX_LOCALS bounds nparams (the parser grows param
     * arrays dynamically up to UFS_MAX_LOCALS = 200; the bound here is
     * conservative relative to UFuncSig.param_is_lazy[16]). */
    const char *param_names[UFS_MAX_LOCALS];
    if (nparams > UFS_MAX_LOCALS) {
        e->error = EMIT_REG_EXHAUSTED;
        urbi_emit_diag_error(e, body, "too many parameters (%d; max %d)", nparams, UFS_MAX_LOCALS);
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
    /* Save the parent's flat-register cursor BEFORE opening the child
     * FuncState.  After the child compile, the parent's `next_reg` is
     * indispensable for finding a closure-destination slot that does not
     * alias any still-live temp in the parent — e.g. the `Realm` GETSLOT
     * result returned by urbi_emit_ident_arm's realm-global fallback when
     * compiling `Realm.fn = function () {...}`.  freereg tracks only the
     * local-zone floor (locals + params + r_global_slot); live temps live
     * ABOVE the floor and are tracked by next_reg, so freereg alone is
     * the wrong source.  Task #22, surfaced 2026-05-16.
     *
     * v0.13.5: capture moved from AFTER the child param declarations to
     * here.  uemit_declare_local's tail sync (`if (e->next_reg <
     * fs->freereg) e->next_reg = fs->freereg`) raises the FLAT cursor to
     * the CHILD's local floor while declaring child params — so when the
     * child's param+synthetic-local count exceeded the parent's floor
     * (e.g. any 1-param literal inside a 0-param watcher body once
     * \x01nargs exists, or a 2-param literal there before it), the old
     * capture point read the child-contaminated cursor and OP_CLOSURE's
     * dst landed one-plus slots above the var-decl's expected register
     * (urbi_emit_var_decl_arm's init_reg != reg_before check → spurious
     * EMIT_UNSUPPORTED_AST). */
    uint8_t parent_next_reg_before = e->next_reg;

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

    /* v0.13.5 arity self-check discipline (default params):
     * every >=1-param function reserves a synthetic local right above the
     * params — slot index == nparams — that the VM (OP_CALL) and the
     * strand-arm paths seed with the ACTUAL passed argument count as a
     * UVAL_INT.  The prologue emitted below (step 3b) reads it to enforce
     * the minimum arity and to fill omitted defaulted params at call time.
     * Synthetic-name pattern per for-each's \x01iter / switch's \x01sw;
     * \x01 is unlexable so user code can never collide.  Declared as a
     * real local so urbi_emit_fs_temp_floor's count-based formula keeps protecting
     * it from if/while temp resets.  ("nargs", not "argc": a name starting
     * with a hex digit would be munched into the \x01 escape.) */
    int argc_slot = -1;
    if (nparams > 0) {
        const char *argc_name = ustr_intern(e->vm, "\x01nargs", 6);
        if (argc_name == NULL) {
            e->error = EMIT_OOM;
            uemit_close_function(e);
            return 0U;
        }
        argc_slot = uemit_declare_local(e, argc_name, 6);
        if (argc_slot < 0) { uemit_close_function(e); return 0U; }
    }
    /* Flag the proto: its module is serialized with header flag bit 0 set
     * and OP_CALL relaxes the arity check to `nargs <= nparams` (the
     * prologue owns the lower bound).  Set on every proto — including
     * 0-param ones, where relaxed and exact checks coincide — so the
     * module-granular wire flag describes every proto uniformly. */
    child_proto->arity_prologue = 1U;

    /* Pre-reserve the realm-global slot register at the current freereg
     * (right above the last param).  This must happen BEFORE body compilation
     * so that if/while temp-resets (which use urbi_emit_fs_temp_floor) never clobber
     * the register, even when a global reference first appears inside a
     * branch arm (where the reset has already moved next_reg below freereg).
     *
     * global_slot_reserved = true signals urbi_emit_fs_temp_floor to include this
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
     * inside the function body are allocated above all param slots.
     * (parent_next_reg_before was captured before uemit_open_function —
     * see the v0.13.5 note there.) */
    e->next_reg = child_fs->freereg;

    /* 3b + 4. Compile the arity prologue, then the body (AST_BLOCK);
     * urbi_emit_instr routes to child_proto.
     *
     * refactor-3 VM-02/B4: clear in_cleanup_body across the nested body —
     * a function literal (or lazy thunk / watcher closure) defined inside a
     * finally body runs LATER as ordinary code, not as part of the cleanup,
     * so its `;` separators keep normal OP_YIELD semantics.  Mirrors the
     * lazy_arg_context save/clear/restore in urbi_emit_lazy_thunk.  The arity
     * prologue (and its default expressions) get the same treatment: they
     * run at call time as ordinary function code. */
    uint8_t saved_icb = e->in_cleanup_body;
    e->in_cleanup_body = 0U;

    /* === 3b: v0.13.5 arity self-check prologue ===
     * Emitted BEFORE the body, using only existing wire-v1.9 opcodes
     * (LOADK / LT / LE / JMP / MOVE / THROW — no wire change).  The
     * synthetic \x01nargs local at argc_slot carries the actual passed
     * count (seeded by OP_CALL / the strand-arm paths).
     *
     *   min_arity = 1 + highest param index WITHOUT a default (0 when all
     *   params carry defaults).  Matches the legacy runtime: formals
     *   desugar to in-order LocalDeclarations (factory.cc formals_to_decs),
     *   so a missing non-defaulted formal raises regardless of defaults on
     *   earlier params — non-trailing defaults are legal but dead.
     *
     *   too-few check (min_arity > 0):
     *     LOADK tmp, K(min)
     *     LT    0, nargs, tmp      ; nargs < min → skip JMP → throw
     *     JMP   ok
     *     LOADK tmp, K("function call: wrong argument count ...")
     *     THROW tmp                ; catchable, same as assert's lowering
     *   ok:
     *
     *   default fill, for each param i in [min_arity, nparams):
     *     LOADK tmp, K(i)
     *     LE    0, nargs, tmp      ; nargs <= i (omitted) → skip JMP → fill
     *     JMP   skip_i
     *     <default expr → r>       ; call-time, callee scope; params 0..i-1
     *     MOVE  R[i], r            ;   already bound, so `b = a + 1` works
     *   skip_i:
     */
    if (nparams > 0) {
        int min_arity = 0;
        {
            int pi;
            for (pi = 0; pi < nparams; pi++) {
                if (params[pi]->u.param.default_expr == NULL) min_arity = pi + 1;
            }
        }
        uint32_t pline = (uint32_t)params[0]->line;

        if (min_arity > 0) {
            uint16_t kmin = urbi_emit_add_const_int(e, (int64_t)min_arity);
            if (e->error != EMIT_OK) {
                e->in_cleanup_body = saved_icb;
                uemit_close_function(e);
                return 0U;
            }

            /* Static diagnostic text: today's VM message + the static
             * expected-count ("at least" when defaults make it a range).
             * The dynamic got-count cannot ride a LOADK constant. */
            char msgbuf[64];
            int  mlen = 0;
            {
                static const char kPrefix[] = "function call: wrong argument count (expected ";
                const char *cp;
                for (cp = kPrefix; *cp != '\0'; cp++) msgbuf[mlen++] = *cp;
                if (min_arity < nparams) {
                    static const char kAtLeast[] = "at least ";
                    for (cp = kAtLeast; *cp != '\0'; cp++) msgbuf[mlen++] = *cp;
                }
                /* Decimal render of min_arity (<= UFS_MAX_LOCALS = 200). */
                {
                    char dig[4];
                    int  nd = 0, v = min_arity;
                    do { dig[nd++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
                    while (nd > 0) msgbuf[mlen++] = dig[--nd];
                }
                msgbuf[mlen++] = ')';
            }
            const char *msg_interned = ustr_intern(e->vm, msgbuf, (size_t)mlen);
            if (msg_interned == NULL) {
                e->error = EMIT_OOM;
                e->in_cleanup_body = saved_icb;
                uemit_close_function(e);
                return 0U;
            }
            uint16_t kmsg = urbi_emit_add_const_str(e, msg_interned);
            if (e->error != EMIT_OK) {
                e->in_cleanup_body = saved_icb;
                uemit_close_function(e);
                return 0U;
            }

            uint8_t tmp = alloc_reg(e);
            if (e->error != EMIT_OK) {
                e->in_cleanup_body = saved_icb;
                uemit_close_function(e);
                return 0U;
            }
            urbi_emit_instr(e, uinstr_enc_abx(OP_LOADK, tmp, kmin), pline);
            urbi_emit_instr(e, uinstr_enc_abc(OP_LT, 0U, (uint8_t)argc_slot, tmp),
                       pline);
            int jmp_ok = emit_fwd_jmp(e, pline);
            urbi_emit_instr(e, uinstr_enc_abx(OP_LOADK, tmp, kmsg), pline);
            uemit_throw(e, tmp, pline);
            /* Bail BEFORE the patch on any emit failure above: a failed
             * urbi_emit_instr doesn't append, so urbi_emit_instr_count could equal
             * jmp_ok and uemit_jmp_offset's forward-jump precondition
             * (target > from) would trip (OOM-injection suites sweep
             * every allocation point through this path). */
            if (e->error != EMIT_OK) {
                e->in_cleanup_body = saved_icb;
                uemit_close_function(e);
                return 0U;
            }
            patch_fwd_jmp_here(e, jmp_ok);
            e->next_reg = child_fs->freereg;  /* release tmp */
        }

        {
            int pi;
            for (pi = min_arity; pi < nparams; pi++) {
                UAstNode *def = params[pi]->u.param.default_expr;
                /* Every param at index >= min_arity carries a default by
                 * construction of min_arity. */
                uint32_t dline = (uint32_t)params[pi]->line;
                uint16_t ki = urbi_emit_add_const_int(e, (int64_t)pi);
                if (e->error != EMIT_OK) {
                    e->in_cleanup_body = saved_icb;
                    uemit_close_function(e);
                    return 0U;
                }
                uint8_t tmp = alloc_reg(e);
                if (e->error != EMIT_OK) {
                    e->in_cleanup_body = saved_icb;
                    uemit_close_function(e);
                    return 0U;
                }
                urbi_emit_instr(e, uinstr_enc_abx(OP_LOADK, tmp, ki), dline);
                urbi_emit_instr(e, uinstr_enc_abc(OP_LE, 0U, (uint8_t)argc_slot, tmp),
                           dline);
                int jmp_skip = emit_fwd_jmp(e, dline);
                uint8_t r = urbi_emit_expr(e, def);
                if (e->error != EMIT_OK) {
                    e->in_cleanup_body = saved_icb;
                    uemit_close_function(e);
                    return 0U;
                }
                if (r != (uint8_t)pi) {
                    urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, (uint8_t)pi, r, 0U),
                               dline);
                }
                /* Same bail-before-patch rule as the too-few block. */
                if (e->error != EMIT_OK) {
                    e->in_cleanup_body = saved_icb;
                    uemit_close_function(e);
                    return 0U;
                }
                patch_fwd_jmp_here(e, jmp_skip);
                /* Reset the temp cursor to the local-zone top for the next
                 * fill / the body (mirrors the if-arm temp-reset idiom). */
                e->next_reg = child_fs->freereg;
            }
        }
    }
    /* === end 3b === */

    uint8_t body_reg = urbi_emit_expr(e, body);
    e->in_cleanup_body = saved_icb;
    if (e->error != EMIT_OK) {
        uemit_close_function(e);
        return 0U;
    }

    /* 5. Final OP_RET.  as_expression=true: return body's last result.
     *    as_expression=false: return nil (body runs for side-effects). */
    if (as_expression) {
        urbi_emit_instr(e, uinstr_enc_abc(OP_RET, body_reg, 0U, 0U),
                   (uint32_t)body->line);
    } else {
        uint8_t nil_reg = e->next_reg;
        if (nil_reg < child_fs->freereg) nil_reg = child_fs->freereg;
        urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, nil_reg, 0U, 0U),
                   (uint32_t)body->line);
        urbi_emit_instr(e, uinstr_enc_abc(OP_RET, nil_reg, 0U, 0U),
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

        urbi_emit_instr(e, uinstr_enc_abx(OP_CLOSURE, dst, (uint16_t)proto_idx),
                   (uint32_t)body->line);
        {
            int ui;
            for (ui = 0; ui < nup; ui++) {
                UUpvalDesc *ud = &upvals_copy[ui];
                urbi_emit_instr(e,
                    uinstr_enc_abc(OP_MOVE, 0U,
                                   ud->in_stack ? 1U : 0U,
                                   (uint8_t)ud->idx),
                    (uint32_t)body->line);
            }
        }
        return dst;
    }
}

/* urbi_emit_if_arm — AST_IF: if (cond) then-block [else else-block]
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
uint8_t urbi_emit_if_arm(UEmitter *e, UAstNode *n) {
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
    uint8_t cond_reg = urbi_emit_expr(e, n->u.if_stmt.cond);
    if (e->error != EMIT_OK) return 0U;
    (void)cond_reg;  /* rx == cond_reg */

    /* 2. TEST rx, 0, 1 — skip next instr (JMP) when cond is truthy. */
    urbi_emit_instr(e, uinstr_enc_abc(OP_TEST, rx, 0U, 1U), (uint32_t)n->line);

    /* 3. JMP placeholder to else/nil target (patched later). */
    int jmp_to_else = emit_fwd_jmp(e, (uint32_t)n->line);

    /* 4. Reset cursor to rd so then-block allocates starting at rd. */
    e->next_reg = rd;
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
    if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;

    /* 5. Compile then-block. */
    uint8_t then_r = urbi_emit_expr(e, n->u.if_stmt.then_block);
    if (e->error != EMIT_OK) return 0U;
    if (then_r != rd) {
        urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, rd, then_r, 0U),
                   (uint32_t)n->line);
    }

    /* 6. JMP past else/nil-load to end (patched later). */
    int jmp_to_end = emit_fwd_jmp(e, (uint32_t)n->line);

    /* 7. Patch jmp_to_else → current pc (start of else/nil arm). */
    patch_fwd_jmp_here(e, jmp_to_else);

    /* 8. Reset cursor to rd for else/nil arm. */
    e->next_reg = rd;
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
    if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;

    /* 9. Compile else-block or emit LOADNIL. */
    if (n->u.if_stmt.else_block != NULL) {
        uint8_t else_r = urbi_emit_expr(e, n->u.if_stmt.else_block);
        if (e->error != EMIT_OK) return 0U;
        if (else_r != rd) {
            urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, rd, else_r, 0U),
                       (uint32_t)n->line);
        }
    } else {
        urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U),
                   (uint32_t)n->line);
    }

    /* 10. Patch jmp_to_end → current pc. */
    patch_fwd_jmp_here(e, jmp_to_end);

    /* Advance next_reg past rd so callers can allocate above the result
     * via alloc_reg.  Match urbi_emit_compare_arm's protocol: rd is a TEMP
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
     * NARY/BLOCK between-stmt reset releases rd via urbi_emit_fs_temp_floor; the
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

/* urbi_emit_while_arm — AST_WHILE: while (cond) { body }
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
uint8_t urbi_emit_while_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    /* rd: the incoming next_reg — anchors the result register and the freereg
     * lower-bound throughout the loop.  Without this pin, a freereg reset to
     * urbi_emit_fs_temp_floor() after the condition can land below a live closure register
     * held by an enclosing & or , emitter (the RHS thunk was compiled first and
     * its register sits between the floor and rd).  The LOADNIL emitted at
     * loop-exit would then overwrite that closure with nil, causing a runtime
     * TypeError from OP_FORK_JOIN.  urbi_emit_if_arm already uses this pattern
     * (`if (fs->freereg < rd) fs->freereg = rd`); align urbi_emit_while_arm. */
    uint8_t rd = e->next_reg;

    /* W1/v0.10.5: open loop context for break/continue. */
    if (!uemit_loop_push(e, ULOOP_FRAME_LOOP)) return 0U;

    int loop_start = (int)urbi_emit_instr_count(e);

    /* 1. Compile cond into rx (= rd — cond starts at the result register). */
    uint8_t rx = rd;
    urbi_emit_expr(e, n->u.while_stmt.cond);
    if (e->error != EMIT_OK) { uemit_loop_pop(e); return 0U; }

    /* 2. TEST rx, 0, 1 — skip JMP-to-exit when cond is truthy. */
    urbi_emit_instr(e, uinstr_enc_abc(OP_TEST, rx, 0U, 1U), (uint32_t)n->line);

    /* 3. JMP placeholder to exit (patched later). */
    int jmp_to_exit = (int)urbi_emit_instr_count(e);
    urbi_emit_instr(e, uinstr_enc_abx(OP_JMP, 0U, UEMIT_JMP_BIAS), (uint32_t)n->line);

    /* Free cond temp; pin freereg to at least rd so the body does not
     * allocate into registers below the result anchor (which may overlap
     * a caller's live closure register). */
    e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
    if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
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
            urbi_emit_expr(e, body->u.block.stmts[i]);
            if (e->error != EMIT_OK) {
                uemit_close_block(e);
                uemit_loop_pop(e);
                return 0U;
            }
            /* Release temps between body statements; pin freereg >= rd. */
            e->current_fs->freereg = urbi_emit_fs_temp_floor(e->current_fs);
            if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
            e->next_reg = e->current_fs->freereg;
        }

        /* W1/v0.10.5: continue PCs land here — BEFORE the back-edge
         * OP_CLOSE, so `continue` closes the iteration's captured cells
         * instead of jumping straight to the back-edge JMP and reusing
         * the still-open cell next iteration (refactor-3 FE-04
         * follow-on). */
        {
            int cont_target = (int)urbi_emit_instr_count(e);
            uemit_loop_patch_continues(e, cont_target);
        }

        /* 5. OP_CLOSE-on-back-edge if any local in the loop block was captured. */
        uemit_emit_loop_back_close(e);

        /* 6. Back-edge JMP to loop_start. */
        {
            int from_pc = (int)urbi_emit_instr_count(e);
            urbi_emit_instr(e, uinstr_enc_abx(OP_JMP, 0U,
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
        exit_target = (int)urbi_emit_instr_count(e);
        if (!uemit_close_block(e)) { uemit_loop_pop(e); return 0U; }
    }

    /* 8. Patch the exit JMP and break PCs to exit_target. */
    {
        urbi_emit_patch_instr(e, jmp_to_exit,
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
        urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U),
                   (uint32_t)n->line);
        e->next_reg++;
        if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
        if (e->current_fs->freereg < e->next_reg)
            e->current_fs->freereg = e->next_reg;
        return r;
    }
}

/* urbi_emit_call_arm — AST_CALL: function call.
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
uint8_t urbi_emit_call_arm(UEmitter *e, UAstNode *n) {
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
        urbi_emit_diag_error(e, n, "too many arguments (%d; max 252)", n->u.call.arg_count);
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
         * (call this reg_before) — urbi_emit_var_decl_arm and similar arms
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
         * (urbi_emit_expr may consume more registers transiently); the slot
         * we care about is whatever urbi_emit_expr returns.  OP_SELF snapshots
         * R[recv_r] first, so dst can safely alias recv. */
        uint8_t recv_r = urbi_emit_expr(e, recv_ast);
        if (e->error != EMIT_OK) return 0U;

        USymbol *name = (USymbol *)ustr_intern(e->vm,
                                               callee->u.member.name_start,
                                               (size_t)callee->u.member.name_len);
        if (name == NULL) { e->error = EMIT_OOM; return 0U; }
        int ic_idx = uemit_assign_ic_index(e, name);
        if (ic_idx < 0) return 0U;

        urbi_emit_instr(e, uinstr_enc_abc(OP_SELF, callee_reg, recv_r,
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
        uint8_t callee_r = urbi_emit_expr(e, callee);
        if (e->error != EMIT_OK) return 0U;
        if (callee_r != callee_reg) {
            urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, callee_reg, callee_r, 0U),
                       (uint32_t)n->line);
        }

        arg_base = 1U;
    }

    /* Sync freereg up to next_reg before the arg loop.  When the callee
     * was a local (OP_MOVE, using next_reg-based allocation), freereg
     * still points at the local zone boundary and is behind next_reg.
     * urbi_emit_lazy_thunk → AST_FUNCTION emit uses freereg (not next_reg) as
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
             * AST_FUNCTION literal, urbi_emit_function_literal pulls its
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
                arg_r = urbi_emit_lazy_thunk(e, n->u.call.args[ai]);
            } else {
                arg_r = urbi_emit_expr(e, n->u.call.args[ai]);
            }
            e->lazy_arg_context = saved_ctx;
            if (e->error != EMIT_OK) return 0U;
            uint8_t expected = callee_reg + arg_base + (uint8_t)ai;
            if (arg_r != expected) {
                urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, expected, arg_r, 0U),
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
    urbi_emit_instr(e, uinstr_enc_abc(OP_CALL, callee_reg, b, c),
               (uint32_t)n->line);
    /* Result is written to R[callee_reg] by the called function's OP_RET. */
    e->next_reg = callee_reg + 1U;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs != NULL && e->next_reg > e->current_fs->max_reg_seen)
        e->current_fs->max_reg_seen = e->next_reg;
    return callee_reg;
}

/* urbi_emit_return_arm — AST_RETURN: return [expr].
 * Compile the value (or nil if absent), emit OP_RET.
 * Only valid inside a function body (current_fs must be non-NULL and
 * must have a target_proto — top-level return is not meaningful but
 * is not rejected at emit time; OP_RET at top-level exits urbi_vm_run). */
uint8_t urbi_emit_return_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    uint8_t ret_reg;
    if (n->u.ret.value != NULL) {
        ret_reg = urbi_emit_expr(e, n->u.ret.value);
        if (e->error != EMIT_OK) return 0U;
    } else {
        /* Bare `return`: return nil.
         *
         * EMIT-017 fix (Wave 5, v0.5.7): force next_reg above the
         * funcstate temp floor before alloc_reg.  alloc_reg uses
         * e->next_reg directly; if a future emit arm transiently drops
         * next_reg below urbi_emit_fs_temp_floor (= nactvar +
         * global_slot_reserved), the returned slot would alias a live
         * local and the subsequent OP_LOADNIL would clobber it.
         * Defensive against new arms; current emit-arm contract syncs
         * next_reg to freereg between siblings, so the bug is dormant.
         * Same fix shape as EMIT-018 (AST_THROW). */
        {
            uint8_t floor_val = urbi_emit_fs_temp_floor(e->current_fs);
            if (e->next_reg < floor_val) e->next_reg = floor_val;
        }
        ret_reg = alloc_reg(e);
        if (e->error != EMIT_OK) return 0U;
        urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, ret_reg, 0U, 0U),
                   (uint32_t)n->line);
    }
    urbi_emit_instr(e, uinstr_enc_abc(OP_RET, ret_reg, 0U, 0U),
               (uint32_t)n->line);
    /* Return the register so the block's last-stmt-reg logic works.
     * Any instructions after OP_RET in the same block are unreachable
     * but that is allowed (e.g., the function body auto-appends OP_RET). */
    return ret_reg;
}

/* urbi_emit_function_arm — AST_FUNCTION: function literal.
 * T30: thin caller — all logic lives in urbi_emit_function_literal.
 * as_expression=true preserves original semantics: the child proto
 * returns its last statement's result register (existing M2 behaviour). */
uint8_t urbi_emit_function_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }
    return urbi_emit_function_literal(e,
                                 n->u.func.params,
                                 n->u.func.param_count,
                                 n->u.func.body,
                                 /*as_expression=*/true);
}

/* === W3/v0.10.5: assert keyword ===
 * urbi_emit_assert_arm — AST_ASSERT: assert(expr) / assert { block }.
 *
 * Lowering (no new opcode needed):
 *
 *   cond_reg = urbi_emit_expr(n->u.assert_stmt.expr)
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
uint8_t urbi_emit_assert_arm(UEmitter *e, UAstNode *n) {
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
    const uint16_t msg_k = urbi_emit_add_const_str(e, msg_interned);
    if (e->error != EMIT_OK) return 0U;

    /* 3. rd is the result register. */
    uint8_t rd = e->next_reg;

    /* 4. Compile the asserted expression/block into cond_reg. */
    uint8_t cond_reg = urbi_emit_expr(e, n->u.assert_stmt.expr);
    if (e->error != EMIT_OK) return 0U;

    /* 5. TEST cond_reg, 0, 1 — skip JMP-to-throw when cond is truthy. */
    urbi_emit_instr(e, uinstr_enc_abc(OP_TEST, cond_reg, 0U, 1U), (uint32_t)n->line);

    /* 6. JMP placeholder to throw_site (patched below). */
    int jmp_to_throw = emit_fwd_jmp(e, (uint32_t)n->line);

    /* 7. Truthy path: reset cursor to rd, emit LOADNIL. */
    e->next_reg = rd;
    {
        uint8_t floor_val = urbi_emit_fs_temp_floor(e->current_fs);
        if (e->current_fs->freereg < rd) e->current_fs->freereg = rd;
        if (e->next_reg < floor_val) e->next_reg = floor_val;
        if (e->next_reg < rd) e->next_reg = rd;
    }
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), (uint32_t)n->line);

    /* 8. JMP placeholder to end (patched below — skip throw site). */
    int jmp_to_end = emit_fwd_jmp(e, (uint32_t)n->line);

    /* 9. Patch jmp_to_throw → here (start of throw_site). */
    patch_fwd_jmp_here(e, jmp_to_throw);

    /* 10. Throw site: allocate msg_reg fresh, load message string, throw. */
    {
        /* Allocate msg_reg above rd so it doesn't stomp rd. */
        uint8_t msg_reg = rd + 1U;
        if (msg_reg > e->max_reg_seen) e->max_reg_seen = msg_reg;
        if (e->current_fs != NULL && msg_reg > e->current_fs->max_reg_seen)
            e->current_fs->max_reg_seen = msg_reg;
        urbi_emit_instr(e, uinstr_enc_abx(OP_LOADK, msg_reg, msg_k), (uint32_t)n->line);
        uemit_throw(e, msg_reg, (uint32_t)n->line);
    }

    /* 11. Patch jmp_to_end → here (past throw site). */
    patch_fwd_jmp_here(e, jmp_to_end);

    /* 12. Advance next_reg past rd.  Match urbi_emit_if_arm protocol. */
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
    /* T24: the exit JMP must not cross open try/tag scopes without tearing
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
    /* T24: same scope-crossing teardown as urbi_emit_break_arm, but the target
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
     * (refactor-3 FE-04 follow-on; previously cont_target sat between
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

    /* 10. Back-edge JMP to loop_start (backward encoder — refactor-3 FE-01;
     *      replaces the local `loop_start + 1` compensation hack). */
    {
        int from_pc = (int)urbi_emit_instr_count(e);
        urbi_emit_instr(e, uinstr_enc_abx(OP_JMP, 0U,
                                     uemit_jmp_offset_backward(from_pc, loop_start)), line);
    }

    /* 11. Patch exit JMP and break PCs.  When the body captured, the
     * exit target lands ON an exit-path OP_CLOSE: a no-op on normal
     * exit (the per-iteration close at step 8 already ran) but required
     * on break paths, which jump here past steps 7-10
     * (refactor-3 FE-04 follow-on). */
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
 * Register discipline (refactor-3 FE-02 follow-on): the subject is held
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
            /* refactor-3 FE-06: a 65th exit JMP would keep its placeholder
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
/* === end W1/v0.10.5: control-flow emit arms === */
