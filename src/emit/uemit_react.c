/* SPDX-License-Identifier: BSD-3-Clause */
/* Contains urbi_emit_expr arm helpers for:
 *   AST_MEMBER_GET, AST_MEMBER_SET   — slot access
 *   AST_WATCHER                      — every reactive form: at / at sync /
 *                                      whenever / waituntil, each crossed
 *                                      with source cond / event / slot-change
 *
 * IMPORTANT: the event (UWSRC_EVENT) and slot-change (UWSRC_SLOT_CHANGE)
 * source forms carry a freereg-sync fix.  The `if (e->current_fs->freereg
 * < e->next_reg)` guards in emit_watcher_event_form and
 * emit_watcher_slot_change_form, right after the event register is
 * computed and before the body closure is built, MUST NOT be removed.
 * Dropping them allows urbi_emit_function_literal to allocate body_reg on
 * top of event_reg, causing OP_CLOSURE to clobber the event pointer at
 * runtime.
 */

#include "emit/uemit_internal.h"  /* uemit_internal.h pulls in umacros.h (urbi_zero) */
#include "emit/uintern.h"        /* ustr_intern */
#include "emit/uemit.h"
#include "chunk/uchunk.h"
#include "parse/uast.h"
#include "util/umacros.h"
#include <stddef.h>
#include <stdint.h>

/* =========================================================================
 * AST_MEMBER_GET — obj.x → OP_GETSLOT
 * ========================================================================= */

uint8_t urbi_emit_member_get_arm(UEmitter *e, UAstNode *n) {
    /* obj.x → OP_GETSLOT.  Per GETSLOT/SETSLOT encoding
     * spec §3: ABC layout where A=dst register, B=recv register,
     * C=IC site index assigned by uemit_assign_ic_index. */
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    /* Emit receiver into a temp register. */
    uint8_t recv_reg = urbi_emit_expr(e, n->u.member.recv);
    if (e->error != EMIT_OK) return 0U;

    /* Intern the slot name to obtain the canonical USymbol pointer. */
    USymbol *name = (USymbol *)ustr_intern(e->vm,
                                           n->u.member.name_start,
                                           (size_t)n->u.member.name_len);
    if (name == NULL) { e->error = EMIT_OOM; return 0U; }

    /* Assign a per-site IC index (independent monomorphism per site). */
    int ic_idx = uemit_assign_ic_index(e, name);
    if (ic_idx < 0) return 0U;

    /* Result reuses recv_reg in place — simple stack discipline. */
    urbi_emit_instr(e, uinstr_enc_abc(OP_GETSLOT, recv_reg, recv_reg,
                                 (uint8_t)ic_idx),
               (uint32_t)n->line);
    return recv_reg;
}

/* =========================================================================
 * AST_MEMBER_SET — obj.x = v → OP_SETSLOT
 * ========================================================================= */

uint8_t urbi_emit_member_set_arm(UEmitter *e, UAstNode *n) {
    /* obj.x = v → OP_SETSLOT.  Per encoding spec §3:
     * ABC layout where A=src register (value to write), B=recv register,
     * C=IC site index.  Assignment evaluates to the assigned value. */
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    /* A function literal with a lazy parameter cannot be stored as a
     * method: the call site cannot see the signature through dynamic
     * dispatch, so the argument would arrive eager regardless. */
    if (n->u.member.value->kind == AST_FUNCTION
        && fn_has_lazy_param(n->u.member.value)) {
        e->error = EMIT_LAZY_ON_METHOD;
        urbi_emit_diag_error(e, n->u.member.value,
                        "lazy parameters are not allowed on methods");
        return 0U;
    }

    /* Emit receiver into a temp, then RHS value into the next temp. */
    uint8_t recv_reg = urbi_emit_expr(e, n->u.member.recv);
    if (e->error != EMIT_OK) return 0U;
    uint8_t src_reg = urbi_emit_expr(e, n->u.member.value);
    if (e->error != EMIT_OK) return 0U;

    USymbol *name = (USymbol *)ustr_intern(e->vm,
                                           n->u.member.name_start,
                                           (size_t)n->u.member.name_len);
    if (name == NULL) { e->error = EMIT_OOM; return 0U; }

    int ic_idx = uemit_assign_ic_index(e, name);
    if (ic_idx < 0) return 0U;

    urbi_emit_instr(e, uinstr_enc_abc(OP_SETSLOT, src_reg, recv_reg,
                                 (uint8_t)ic_idx),
               (uint32_t)n->line);

    /* Assignment expression value is the assigned value.  Collapse the
     * recv temp by moving src down into recv_reg, matching the
     * AST_BINARY convention (lhs holds the result, top temp freed). */
    if (src_reg != recv_reg) {
        urbi_emit_instr(e, uinstr_enc_abc(OP_MOVE, recv_reg, src_reg, 0U),
                   (uint32_t)n->line);
    }
    free_reg(e);              /* release the src temp; result in recv_reg */
    return recv_reg;
}

/* =========================================================================
 * AST_WATCHER — at / at sync / whenever / waituntil, every source form
 * ========================================================================= */

/* Shared tail: emit a nil register as a reactive install's expression
 * value (watchers are statements, but urbi_emit_expr must return a
 * register). */
static uint8_t emit_watcher_nil_result(UEmitter *e, int line) {
    uint8_t rd = e->next_reg;
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), (uint32_t)line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return rd;
}

/* Shared: a 0-param closure for a body/else/onleave AST, or the 0xFF
 * "absent" sentinel the install opcodes read when body_ast is NULL. */
static uint8_t emit_watcher_closure_or_absent(UEmitter *e, UAstNode *body_ast) {
    if (body_ast == NULL) return 0xFFU;
    return urbi_emit_function_literal(e, NULL, 0, body_ast, /*as_expression=*/false);
}

/* Shared: a 1-param closure (the event/slot-change payload parameter)
 * for an EVENT- or SLOT_CHANGE-source body, or 0xFF if absent. */
static uint8_t emit_watcher_payload_closure_or_absent(UEmitter *e, UAstNode *body_ast,
                                                       const char *pname, int plen) {
    if (body_ast == NULL) return 0xFFU;
    UAstNode payload_param;
    urbi_zero(&payload_param, sizeof payload_param);
    payload_param.kind               = AST_PARAM;
    payload_param.line               = body_ast->line;
    payload_param.col                = 1;
    payload_param.u.param.name_start = pname;
    payload_param.u.param.name_len   = plen;
    UAstNode *params_arr[1] = { &payload_param };
    return urbi_emit_function_literal(e, params_arr, 1, body_ast, /*as_expression=*/false);
}

/* --- UWSRC_COND: at (cond) body [onleave] / at sync (cond) body /
 * whenever (cond) body [onleave] [else else_body]
 *
 * Build cond/body/alt closures via urbi_emit_function_literal, then emit
 * the appropriate install opcode (ABC-encoded).  Side-effect check on
 * cond per spec #2 §9.1. */
static uint8_t emit_watcher_cond_form(UEmitter *e, UAstNode *n, int mode) {
    UAstNode *cond_ast      = n->u.watcher.cond;
    UAstNode *body_ast      = n->u.watcher.body;
    UAstNode *onleave_ast   = n->u.watcher.onleave;   /* NULL if absent */
    UAstNode *else_body_ast = n->u.watcher.else_body; /* Nullable; WHENEVER only */

    /* For WHENEVER mode, else_body takes precedence over onleave as the
     * alt closure stored in register C of OP_WHENEVER_INSTALL.  The runtime
     * invokes the alt on the falling edge (true→false transition) after at
     * least one body firing.  AT/AT_SYNC modes do not support else_body. */
    UAstNode *alt_ast = (mode == UWATCHER_WHENEVER && else_body_ast != NULL)
                      ? else_body_ast
                      : onleave_ast;

    /* Compile-time best-effort cond side-effect warn (spec #2 Q7b). */
    if (urbi_emit_cond_has_direct_side_effect(cond_ast)) {
        urbi_emit_diag_warn(e, cond_ast,
                       "watcher condition has direct write/assignment; "
                       "may cause feedback loop at runtime");
    }

    uint8_t cond_reg = urbi_emit_function_literal(e, NULL, 0,
                                             cond_ast, /*as_expression=*/true);
    if (e->error != EMIT_OK) return 0U;

    uint8_t body_reg = emit_watcher_closure_or_absent(e, body_ast);
    if (e->error != EMIT_OK) return 0U;

    /* alt_reg: compiled from else_body_ast (WHENEVER) or onleave_ast.
     * alt_ast is pre-selected above; when both else_body and onleave are
     * absent alt_ast is NULL → 0xFF sentinel. */
    uint8_t alt_reg = emit_watcher_closure_or_absent(e, alt_ast);
    if (e->error != EMIT_OK) return 0U;

    UOpcode op;
    switch (mode) {
        case UWATCHER_AT:       op = OP_AT_INSTALL;       break;
        case UWATCHER_AT_SYNC:  op = OP_AT_SYNC_INSTALL;  break;
        case UWATCHER_WHENEVER: op = OP_WHENEVER_INSTALL; break;
        default:                op = OP_AT_INSTALL;       break;
    }
    urbi_emit_instr(e, uinstr_enc_abc(op, cond_reg, body_reg, alt_reg),
               (uint32_t)n->line);

    if (alt_ast  != NULL) free_reg_freereg_synced(e);
    if (body_ast != NULL) free_reg_freereg_synced(e);
    free_reg_freereg_synced(e);  /* cond_reg */

    return emit_watcher_nil_result(e, n->line);
}

/* Shared tail for UWSRC_EVENT and UWSRC_SLOT_CHANGE: given an already-
 * computed event_reg and the install opcode to use, build the 1-param
 * body closure (R[0] receives the emit payload per spec #3 §5.5) and the
 * optional 0-param onleave closure, emit the install, and free temps.
 * 0xFF in the alt_reg slot signals "no onleave" to the runtime. */
static uint8_t emit_watcher_event_install(UEmitter *e, UAstNode *n,
                                           uint8_t event_reg, UOpcode op) {
    UAstNode *body_ast    = n->u.watcher.body;
    UAstNode *onleave_ast = n->u.watcher.onleave;

    /* Body closure: 1 param (payload).  Use the user-specified name from
     * `(var x)` if present; fall back to `__payload` for anonymous
     * payload (legacy default). */
    const char *pname = (n->u.watcher.payload_var != NULL)
                      ? n->u.watcher.payload_var
                      : "__payload";
    int plen = (n->u.watcher.payload_var != NULL)
             ? n->u.watcher.payload_var_len
             : 9;

    uint8_t body_reg = emit_watcher_payload_closure_or_absent(e, body_ast, pname, plen);
    if (e->error != EMIT_OK) return 0U;

    uint8_t alt_reg = emit_watcher_closure_or_absent(e, onleave_ast);
    if (e->error != EMIT_OK) return 0U;

    urbi_emit_instr(e, uinstr_enc_abc(op, event_reg, body_reg, alt_reg),
               (uint32_t)n->line);

    if (alt_reg  != 0xFFU) free_reg_freereg_synced(e);
    if (body_reg != 0xFFU) free_reg_freereg_synced(e);
    free_reg_freereg_synced(e);  /* event_reg */

    return emit_watcher_nil_result(e, n->line);
}

/* --- UWSRC_EVENT: at (e?) body [onleave] / at sync (e?) body [onleave] /
 * whenever (e?) body [onleave]
 *
 * Emit the event-expression into a register, then build the body/onleave
 * closures and the install opcode: OP_AT_EVENT_INSTALL (=42),
 * OP_AT_EVENT_SYNC_INSTALL (=43), or OP_WHENEVER_EVENT_INSTALL. */
static uint8_t emit_watcher_event_form(UEmitter *e, UAstNode *n, int mode) {
    UAstNode *event_ast = n->u.watcher.cond;

    uint8_t event_reg = urbi_emit_expr(e, event_ast);
    if (e->error != EMIT_OK) return 0U;

    /* Sync freereg up to next_reg before allocating the body closure.
     * AST_IDENT global-fallback and the chains it feeds (AST_MEMBER_GET
     * et al.) bump only e->next_reg, leaving freereg stale.
     * urbi_emit_function_literal allocates body_reg from freereg, so
     * without this sync body_reg can land on top of event_reg —
     * OP_CLOSURE then clobbers the event pointer at runtime.  The COND
     * source form avoids this by routing cond through
     * urbi_emit_function_literal symmetrically.  Do NOT remove. */
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;

    UOpcode op = (mode == UWATCHER_WHENEVER) ? OP_WHENEVER_EVENT_INSTALL
               : (mode == UWATCHER_AT_SYNC)  ? OP_AT_EVENT_SYNC_INSTALL
               :                               OP_AT_EVENT_INSTALL;
    return emit_watcher_event_install(e, n, event_reg, op);
}

/* --- UWSRC_SLOT_CHANGE: at (obj.x.changed?) body [onleave] / at sync
 * variant.  Spec #4 §4.2: emit GETSLOT_CHANGE_EVENT then AT_EVENT_INSTALL.
 *
 *   recv_reg  := emit receiver expression (n->u.watcher.cond)
 *   ic_idx    := uemit_assign_ic_index for slot name
 *   event_reg := OP_GETSLOT_CHANGE_EVENT(event_reg, recv_reg, ic_idx)
 *   body_reg  := urbi_emit_function_literal(body, 1 param)
 *   alt_reg   := urbi_emit_function_literal(onleave, 0 params) or 0xFF
 *                OP_AT_EVENT_INSTALL / OP_AT_EVENT_SYNC_INSTALL
 *
 * Never reached in WHENEVER mode: `whenever` never slot-change-dispatches. */
static uint8_t emit_watcher_slot_change_form(UEmitter *e, UAstNode *n, int mode) {
    UAstNode   *recv_ast    = n->u.watcher.cond;
    const char *sname       = n->u.watcher.slot_name;
    size_t      sname_len   = (size_t)n->u.watcher.slot_name_len;

    uint8_t recv_reg = urbi_emit_expr(e, recv_ast);
    if (e->error != EMIT_OK) return 0U;

    USymbol *slot_sym = (USymbol *)ustr_intern(e->vm, sname, sname_len);
    if (slot_sym == NULL) { e->error = EMIT_OOM; return 0U; }

    int ic_idx = uemit_assign_ic_index(e, slot_sym);
    if (ic_idx < 0) return 0U;

    /* Emit the event-lookup; result overwrites recv_reg (same
     * register reuse as OP_GETSLOT in AST_MEMBER_GET). */
    uint8_t event_reg = recv_reg;
    urbi_emit_instr(e, uinstr_enc_abc(OP_GETSLOT_CHANGE_EVENT,
                                 event_reg, recv_reg, (uint8_t)ic_idx),
               (uint32_t)n->line);

    /* Sync freereg up to next_reg before allocating the body closure
     * (mirrors the EVENT source form).  AST_IDENT global-fallback feeding
     * recv_ast bumps next_reg only, leaving freereg stale, so
     * urbi_emit_function_literal can otherwise allocate body_reg on top
     * of event_reg.  Do NOT remove. */
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;

    UOpcode op = (mode == UWATCHER_AT_SYNC) ? OP_AT_EVENT_SYNC_INSTALL : OP_AT_EVENT_INSTALL;
    return emit_watcher_event_install(e, n, event_reg, op);
}

/* --- UWATCHER_WAITUNTIL + UWSRC_COND: waituntil (cond) — one-shot
 * strand-block primitive.  Build a cond closure, emit
 * OP_WAITUNTIL_INSTALL (=41).  Side-effect check per spec #2 §9.2. */
static uint8_t emit_watcher_waituntil_cond_form(UEmitter *e, UAstNode *n) {
    UAstNode *cond_ast = n->u.watcher.cond;

    if (urbi_emit_cond_has_direct_side_effect(cond_ast)) {
        urbi_emit_diag_warn(e, cond_ast,
                       "watcher condition has direct write/assignment; "
                       "may cause feedback loop at runtime");
    }

    uint8_t cond_reg = urbi_emit_function_literal(e, NULL, 0,
                                             cond_ast, /*as_expression=*/true);
    if (e->error != EMIT_OK) return 0U;

    urbi_emit_instr(e, uinstr_enc_abc(OP_WAITUNTIL_INSTALL, cond_reg, 0U, 0U),
               (uint32_t)n->line);
    free_reg_freereg_synced(e);  /* cond_reg — EMIT-010 */

    return emit_watcher_nil_result(e, n->line);
}

/* --- UWATCHER_WAITUNTIL + UWSRC_EVENT: desugar waituntil (e?) to
 * e.waituntil(). */
static uint8_t emit_watcher_waituntil_event_form(UEmitter *e, UAstNode *n) {
    UAstNode *event_ast = n->u.watcher.cond;

    /* Build stack-allocated AST: member_node = AST_MEMBER_GET(event_ast, "waituntil") */
    UAstNode member_node;
    urbi_zero(&member_node, sizeof member_node);
    member_node.kind                 = AST_MEMBER_GET;
    member_node.line                 = n->line;
    member_node.col                  = n->col;
    member_node.u.member.recv        = event_ast;
    member_node.u.member.name_start  = "waituntil";
    member_node.u.member.name_len    = 9;
    member_node.u.member.value       = NULL;

    /* Build stack-allocated AST: call_node = AST_CALL(member_node, args=NULL, 0) */
    UAstNode call_node;
    urbi_zero(&call_node, sizeof call_node);
    call_node.kind            = AST_CALL;
    call_node.line            = n->line;
    call_node.col             = n->col;
    call_node.u.call.callee   = &member_node;
    call_node.u.call.args     = NULL;
    call_node.u.call.arg_count = 0;

    /* Emit the desugared call — result is the payload value returned
     * by urbi_event_waituntil / the strand's last_event_payload on resume. */
    return urbi_emit_expr(e, &call_node);
}

uint8_t urbi_emit_watcher_arm(UEmitter *e, UAstNode *n) {
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    int             mode   = n->u.watcher.mode;
    UAstWatchSource source = n->u.watcher.source;

    if (mode == UWATCHER_WAITUNTIL) {
        return (source == UWSRC_EVENT)
            ? emit_watcher_waituntil_event_form(e, n)
            : emit_watcher_waituntil_cond_form(e, n);
    }

    switch (source) {
        case UWSRC_EVENT:       return emit_watcher_event_form(e, n, mode);
        case UWSRC_SLOT_CHANGE: return emit_watcher_slot_change_form(e, n, mode);
        case UWSRC_COND:
        default:                return emit_watcher_cond_form(e, n, mode);
    }
}
