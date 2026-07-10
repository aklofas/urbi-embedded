/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_react.c — reactive / slot-access bytecode emitters.
 * Extracted from uemit.c during v0.5.4-decompose (EMIT-045 #5).
 *
 * Contains urbi_emit_expr arm helpers for:
 *   AST_MEMBER_GET, AST_MEMBER_SET   — slot access
 *   AST_WATCHER                      — at/whenever/at-sync installs
 *   AST_WAITUNTIL                    — waituntil install
 *   AST_AT_EVENT                     — event-driven at/at-sync
 *   AST_AT_SLOT_CHANGE               — slot-change at/at-sync
 *
 * IMPORTANT: AST_AT_EVENT and AST_AT_SLOT_CHANGE carry the v0.5.2 freereg-sync
 * fix (commit 6eb40a3).  The two `if (e->current_fs->freereg < e->next_reg)`
 * guards at the urbi_emit_function_literal call sites in urbi_emit_at_event_arm and
 * urbi_emit_at_slot_change_arm MUST NOT be removed.  Dropping them allows
 * urbi_emit_function_literal to allocate body_reg on top of event_reg, causing
 * OP_CLOSURE to clobber the event pointer at runtime. */

#include "emit/uemit_internal.h"  /* uemit_internal.h pulls in umacros.h (urbi_zero) */
#include "value/uintern.h"        /* ustr_intern */
#include "watcher/uwatcher.h"     /* UWATCHER_AT / _AT_SYNC / _WHENEVER */
#include "emit/uemit.h"
#include "chunk/uchunk.h"
#include "parse/uast.h"
#include "runtime/umacros.h"
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
 * AST_WATCHER — at (cond) body [onleave] / at sync / whenever
 * ========================================================================= */

uint8_t urbi_emit_watcher_arm(UEmitter *e, UAstNode *n) {
    /* at (cond) body [onleave] / at sync (cond) body /
     *      whenever (cond) body [onleave]
     *
     * Build cond/body/onleave closures via urbi_emit_function_literal,
     * then emit the appropriate install opcode (ABC-encoded).
     * Side-effect check on cond per spec #2 §9.1. */
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    UAstNode *cond_ast      = n->u.watcher.cond;
    UAstNode *body_ast      = n->u.watcher.body;
    UAstNode *onleave_ast   = n->u.watcher.onleave;  /* NULL if absent */
    UAstNode *else_body_ast = n->u.watcher.else_body; /* Nullable; WHENEVER only */
    int       mode          = n->u.watcher.mode;

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

    uint8_t body_reg = (body_ast != NULL)
        ? urbi_emit_function_literal(e, NULL, 0, body_ast, /*as_expression=*/false)
        : 0xFFU;
    if (e->error != EMIT_OK) return 0U;

    /* alt_reg: compiled from else_body_ast (WHENEVER) or onleave_ast.
     * alt_ast is pre-selected above; when both else_body and onleave are
     * absent alt_ast is NULL → 0xFF sentinel. */
    uint8_t alt_reg = (alt_ast != NULL)
        ? urbi_emit_function_literal(e, NULL, 0, alt_ast, /*as_expression=*/false)
        : 0xFFU;
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

    /* Release temporary closure regs — watcher install is a statement.
     * EMIT-010 use free_reg_freereg_synced so freereg unwinds
     * symmetrically with next_reg.  urbi_emit_function_literal raised both
     * cursors when compiling the cond/body/alt closures; plain
     * free_reg() would leave freereg promoted, leaking 1-3 register
     * slots past the install statement. */
    if (alt_ast  != NULL) free_reg_freereg_synced(e);
    if (body_ast != NULL) free_reg_freereg_synced(e);
    free_reg_freereg_synced(e);  /* cond_reg */

    /* Return a nil register as the install expression's value. */
    uint8_t rd = e->next_reg;
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), (uint32_t)n->line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return rd;
}

/* =========================================================================
 * AST_WAITUNTIL — waituntil (cond)
 * ========================================================================= */

uint8_t urbi_emit_waituntil_arm(UEmitter *e, UAstNode *n) {
    /* waituntil (cond) — one-shot strand-block primitive.
     * Build a cond closure, emit OP_WAITUNTIL_INSTALL (=41).
     * Side-effect check per spec #2 §9.2.
     *
     * v0.10.5: waituntil (e?) event form desugars to e.waituntil().
     * The `urbi_event_waituntil` runtime function parks the calling strand on
     * the event's waiters_head until an emit fires; the emit payload is
     * deposited in s->last_event_payload and becomes the call's return
     * value when the strand resumes.  Stack-allocated AST nodes avoid arena
     * allocation for the desugar — their lifetime spans only this emit call. */
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    /* Event form — desugar waituntil (e?) to e.waituntil(). */
    if (n->u.waituntil.is_event_form) {
        UAstNode *event_ast = n->u.waituntil.cond;

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
        uint8_t rd = urbi_emit_expr(e, &call_node);
        return rd;
    }

    UAstNode *cond_ast = n->u.waituntil.cond;

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

    uint8_t rd = e->next_reg;
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), (uint32_t)n->line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return rd;
}

/* =========================================================================
 * AST_AT_EVENT — at (e?) body [onleave] / at sync (e?) body [onleave]
 * ========================================================================= */

uint8_t urbi_emit_at_event_arm(UEmitter *e, UAstNode *n) {
    /* at (e?) body [onleave] / at sync (e?) body [onleave]
     *
     * Emit the event-expression into a register, build a 1-param body
     * closure (R[0] receives the emit payload per spec #3 §5.5) and an
     * optional 0-param onleave closure, then emit the appropriate install
     * opcode: OP_AT_EVENT_INSTALL (=42) or OP_AT_EVENT_SYNC_INSTALL (=43).
     * 0xFF in the alt_reg slot signals "no onleave" to the runtime. */
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    UAstNode *event_ast     = n->u.at_event.event_expr;
    UAstNode *body_ast      = n->u.at_event.body;
    UAstNode *onleave_ast   = n->u.at_event.onleave;
    bool      sync_flag     = n->u.at_event.is_sync;
    bool      whenever_flag = n->u.at_event.is_whenever;

    uint8_t event_reg = urbi_emit_expr(e, event_ast);
    if (e->error != EMIT_OK) return 0U;

    /* Sync freereg up to next_reg before allocating the body closure.
     * AST_IDENT global-fallback (line ~824) and the chains it feeds
     * (AST_MEMBER_GET et al.) bump only e->next_reg, leaving freereg
     * stale.  urbi_emit_function_literal allocates body_reg from freereg,
     * so without this sync body_reg can land on top of event_reg —
     * OP_CLOSURE then clobbers the event pointer at runtime.
     * AST_WATCHER avoids this by routing cond through
     * urbi_emit_function_literal symmetrically.
     * v0.5.2 freereg-sync fix (commit 6eb40a3) — do NOT remove. */
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;

    /* Body closure: 1 param (payload).
     * Use user-specified name from `at (e?(var x))` if present;
     * fall back to `__payload` for anonymous payload (legacy default). */
    const char *pname = (n->u.at_event.payload_var_name != NULL)
                      ? n->u.at_event.payload_var_name
                      : "__payload";
    int plen = (n->u.at_event.payload_var_name != NULL)
             ? n->u.at_event.payload_var_len
             : 9;

    UAstNode payload_param;
    urbi_zero(&payload_param, sizeof payload_param);
    payload_param.kind               = AST_PARAM;
    payload_param.line               = body_ast ? body_ast->line : n->line;
    payload_param.col                = 1;
    payload_param.u.param.name_start = pname;
    payload_param.u.param.name_len   = plen;
    UAstNode *params_arr[1] = { &payload_param };

    uint8_t body_reg = (body_ast != NULL)
        ? urbi_emit_function_literal(e, params_arr, 1, body_ast, /*as_expression=*/false)
        : 0xFFU;
    if (e->error != EMIT_OK) return 0U;

    uint8_t alt_reg = (onleave_ast != NULL)
        ? urbi_emit_function_literal(e, NULL, 0, onleave_ast, /*as_expression=*/false)
        : 0xFFU;
    if (e->error != EMIT_OK) return 0U;

    /* v0.10.2: three-way opcode select.
     *   whenever (e?)  → OP_WHENEVER_EVENT_INSTALL (=48; re-fires every emit)
     *   at sync (e?)   → OP_AT_EVENT_SYNC_INSTALL  (=43; sync, one-shot per emit)
     *   at (e?)        → OP_AT_EVENT_INSTALL        (=42; async, one-shot per emit)
     * whenever-sync has no valid surface syntax; urbi_parse_whenever never sets
     * is_sync=true, so whenever_flag && sync_flag cannot both be true. */
    UOpcode op = whenever_flag ? OP_WHENEVER_EVENT_INSTALL
               : sync_flag    ? OP_AT_EVENT_SYNC_INSTALL
               :                OP_AT_EVENT_INSTALL;
    urbi_emit_instr(e, uinstr_enc_abc(op, event_reg, body_reg, alt_reg),
               (uint32_t)n->line);

    /* EMIT-010 unwind both cursors symmetrically. */
    if (alt_reg  != 0xFFU) free_reg_freereg_synced(e);
    if (body_reg != 0xFFU) free_reg_freereg_synced(e);
    free_reg_freereg_synced(e);  /* event_reg */

    uint8_t rd = e->next_reg;
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), (uint32_t)n->line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return rd;
}

/* =========================================================================
 * AST_AT_SLOT_CHANGE — at (obj.x.changed?) body [onleave] / at sync variant
 * ========================================================================= */

uint8_t urbi_emit_at_slot_change_arm(UEmitter *e, UAstNode *n) {
    /* at (obj.x.changed?) body [onleave] / at sync variant.
     * Spec #4 §4.2: emit GETSLOT_CHANGE_EVENT then AT_EVENT_INSTALL.
     *
     *   recv_reg  := emit receiver expression
     *   ic_idx    := uemit_assign_ic_index for slot name
     *   event_reg := OP_GETSLOT_CHANGE_EVENT(event_reg, recv_reg, ic_idx)
     *   body_reg  := urbi_emit_function_literal(body, 1 param)
     *   alt_reg   := urbi_emit_function_literal(onleave, 0 params) or 0xFF
     *                OP_AT_EVENT_INSTALL / OP_AT_EVENT_SYNC_INSTALL
     */
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    UAstNode *recv_ast    = n->u.at_slot_change.receiver;
    const char *sname     = n->u.at_slot_change.slot_name;
    size_t      sname_len = n->u.at_slot_change.slot_name_len;
    UAstNode *body_ast    = n->u.at_slot_change.body;
    UAstNode *onleave_ast = n->u.at_slot_change.onleave;
    bool      sync_flag   = n->u.at_slot_change.is_sync;

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
     * (mirrors AST_AT_EVENT).  AST_IDENT global-fallback feeding
     * recv_ast bumps next_reg only, leaving freereg stale, so
     * urbi_emit_function_literal can otherwise allocate body_reg on top
     * of event_reg.
     * v0.5.2 freereg-sync fix (commit 6eb40a3) — do NOT remove. */
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;

    /* Body closure: 1 param (payload value on event fire). */
    UAstNode payload_param;
    urbi_zero(&payload_param, sizeof payload_param);
    payload_param.kind               = AST_PARAM;
    payload_param.line               = body_ast ? body_ast->line : n->line;
    payload_param.col                = 1;
    payload_param.u.param.name_start = "__payload";
    payload_param.u.param.name_len   = 9;
    UAstNode *params_arr[1] = { &payload_param };

    uint8_t body_reg = (body_ast != NULL)
        ? urbi_emit_function_literal(e, params_arr, 1, body_ast, /*as_expression=*/false)
        : 0xFFU;
    if (e->error != EMIT_OK) return 0U;

    uint8_t alt_reg = (onleave_ast != NULL)
        ? urbi_emit_function_literal(e, NULL, 0, onleave_ast, /*as_expression=*/false)
        : 0xFFU;
    if (e->error != EMIT_OK) return 0U;

    UOpcode op = sync_flag ? OP_AT_EVENT_SYNC_INSTALL : OP_AT_EVENT_INSTALL;
    urbi_emit_instr(e, uinstr_enc_abc(op, event_reg, body_reg, alt_reg),
               (uint32_t)n->line);

    /* EMIT-010 unwind both cursors symmetrically. */
    if (alt_reg  != 0xFFU) free_reg_freereg_synced(e);
    if (body_reg != 0xFFU) free_reg_freereg_synced(e);
    free_reg_freereg_synced(e);  /* event_reg */

    uint8_t rd = e->next_reg;
    urbi_emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), (uint32_t)n->line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return rd;
}
