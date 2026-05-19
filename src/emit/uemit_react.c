/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_react.c — reactive / slot-access bytecode emitters.
 * Extracted from uemit.c during v0.5.4-decompose (EMIT-045 #5).
 *
 * Contains emit_expr arm helpers for:
 *   AST_MEMBER_GET, AST_MEMBER_SET   — slot access (M4 T20/T21)
 *   AST_WATCHER                      — at/whenever/at-sync installs (M5 T33)
 *   AST_WAITUNTIL                    — waituntil install (M5 T33)
 *   AST_AT_EVENT                     — event-driven at/at-sync (M5 T45)
 *   AST_AT_SLOT_CHANGE               — slot-change at/at-sync (M5 T63)
 *
 * IMPORTANT: AST_AT_EVENT and AST_AT_SLOT_CHANGE carry the v0.5.2 freereg-sync
 * fix (commit 6eb40a3).  The two `if (e->current_fs->freereg < e->next_reg)`
 * guards at the emit_function_literal call sites in emit_at_event_arm and
 * emit_at_slot_change_arm MUST NOT be removed.  Dropping them allows
 * emit_function_literal to allocate body_reg on top of event_reg, causing
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

uint8_t emit_member_get_arm(UEmitter *e, UAstNode *n) {
    /* M4 T20: obj.x → OP_GETSLOT.  Per pre-M4 GETSLOT/SETSLOT encoding
     * spec §3: ABC layout where A=dst register, B=recv register,
     * C=IC site index assigned by uemit_assign_ic_index. */
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    /* Emit receiver into a temp register. */
    uint8_t recv_reg = emit_expr(e, n->u.member.recv);
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
    emit_instr(e, uinstr_enc_abc(OP_GETSLOT, recv_reg, recv_reg,
                                 (uint8_t)ic_idx),
               (uint32_t)n->line);
    return recv_reg;
}

/* =========================================================================
 * AST_MEMBER_SET — obj.x = v → OP_SETSLOT
 * ========================================================================= */

uint8_t emit_member_set_arm(UEmitter *e, UAstNode *n) {
    /* M4 T21: obj.x = v → OP_SETSLOT.  Per encoding spec §3:
     * ABC layout where A=src register (value to write), B=recv register,
     * C=IC site index.  Assignment evaluates to the assigned value. */
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    /* Emit receiver into a temp, then RHS value into the next temp. */
    uint8_t recv_reg = emit_expr(e, n->u.member.recv);
    if (e->error != EMIT_OK) return 0U;
    uint8_t src_reg = emit_expr(e, n->u.member.value);
    if (e->error != EMIT_OK) return 0U;

    USymbol *name = (USymbol *)ustr_intern(e->vm,
                                           n->u.member.name_start,
                                           (size_t)n->u.member.name_len);
    if (name == NULL) { e->error = EMIT_OOM; return 0U; }

    int ic_idx = uemit_assign_ic_index(e, name);
    if (ic_idx < 0) return 0U;

    emit_instr(e, uinstr_enc_abc(OP_SETSLOT, src_reg, recv_reg,
                                 (uint8_t)ic_idx),
               (uint32_t)n->line);

    /* Assignment expression value is the assigned value.  Collapse the
     * recv temp by moving src down into recv_reg, matching the
     * AST_BINARY convention (lhs holds the result, top temp freed). */
    if (src_reg != recv_reg) {
        emit_instr(e, uinstr_enc_abc(OP_MOVE, recv_reg, src_reg, 0U),
                   (uint32_t)n->line);
    }
    free_reg(e);              /* release the src temp; result in recv_reg */
    return recv_reg;
}

/* =========================================================================
 * AST_WATCHER — at (cond) body [onleave] / at sync / whenever
 * ========================================================================= */

uint8_t emit_watcher_arm(UEmitter *e, UAstNode *n) {
    /* T33: at (cond) body [onleave] / at sync (cond) body /
     *      whenever (cond) body [onleave]
     *
     * Build cond/body/onleave closures via emit_function_literal (T30),
     * then emit the appropriate install opcode (ABC-encoded).
     * Side-effect check on cond per spec #2 §9.1. */
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    UAstNode *cond_ast    = n->u.watcher.cond;
    UAstNode *body_ast    = n->u.watcher.body;
    UAstNode *onleave_ast = n->u.watcher.onleave;  /* NULL if absent */
    int       mode        = n->u.watcher.mode;

    /* Compile-time best-effort cond side-effect warn (spec #2 Q7b). */
    if (cond_has_direct_side_effect(cond_ast)) {
        emit_diag_warn(e, cond_ast,
                       "watcher condition has direct write/assignment; "
                       "may cause feedback loop at runtime");
    }

    uint8_t cond_reg = emit_function_literal(e, NULL, 0,
                                             cond_ast, /*as_expression=*/true);
    if (e->error != EMIT_OK) return 0U;

    uint8_t body_reg = (body_ast != NULL)
        ? emit_function_literal(e, NULL, 0, body_ast, /*as_expression=*/false)
        : 0xFFU;
    if (e->error != EMIT_OK) return 0U;

    uint8_t onleave_reg = (onleave_ast != NULL)
        ? emit_function_literal(e, NULL, 0, onleave_ast, /*as_expression=*/false)
        : 0xFFU;
    if (e->error != EMIT_OK) return 0U;

    UOpcode op;
    switch (mode) {
        case UWATCHER_AT:       op = OP_AT_INSTALL;       break;
        case UWATCHER_AT_SYNC:  op = OP_AT_SYNC_INSTALL;  break;
        case UWATCHER_WHENEVER: op = OP_WHENEVER_INSTALL; break;
        default:                op = OP_AT_INSTALL;       break;
    }
    emit_instr(e, uinstr_enc_abc(op, cond_reg, body_reg, onleave_reg),
               (uint32_t)n->line);

    /* Release temporary closure regs — watcher install is a statement.
     * EMIT-010 (Wave 5): use free_reg_freereg_synced so freereg unwinds
     * symmetrically with next_reg.  emit_function_literal raised both
     * cursors when compiling the cond/body/onleave closures; plain
     * free_reg() would leave freereg promoted, leaking 1-3 register
     * slots past the install statement. */
    if (onleave_ast != NULL) free_reg_freereg_synced(e);
    if (body_ast    != NULL) free_reg_freereg_synced(e);
    free_reg_freereg_synced(e);  /* cond_reg */

    /* Return a nil register as the install expression's value. */
    uint8_t rd = e->next_reg;
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), (uint32_t)n->line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return rd;
}

/* =========================================================================
 * AST_WAITUNTIL — waituntil (cond)
 * ========================================================================= */

uint8_t emit_waituntil_arm(UEmitter *e, UAstNode *n) {
    /* T33: waituntil (cond) — one-shot strand-block primitive.
     * Build a cond closure, emit OP_WAITUNTIL_INSTALL (=41).
     * Side-effect check per spec #2 §9.2. */
    if (e->current_fs == NULL || e->vm == NULL) {
        e->error = EMIT_UNSUPPORTED_AST;
        return 0U;
    }

    UAstNode *cond_ast = n->u.waituntil.cond;

    if (cond_has_direct_side_effect(cond_ast)) {
        emit_diag_warn(e, cond_ast,
                       "watcher condition has direct write/assignment; "
                       "may cause feedback loop at runtime");
    }

    uint8_t cond_reg = emit_function_literal(e, NULL, 0,
                                             cond_ast, /*as_expression=*/true);
    if (e->error != EMIT_OK) return 0U;

    emit_instr(e, uinstr_enc_abc(OP_WAITUNTIL_INSTALL, cond_reg, 0U, 0U),
               (uint32_t)n->line);
    free_reg_freereg_synced(e);  /* cond_reg — EMIT-010 (Wave 5) */

    uint8_t rd = e->next_reg;
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), (uint32_t)n->line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return rd;
}

/* =========================================================================
 * AST_AT_EVENT — at (e?) body [onleave] / at sync (e?) body [onleave]
 * ========================================================================= */

uint8_t emit_at_event_arm(UEmitter *e, UAstNode *n) {
    /* T45: at (e?) body [onleave] / at sync (e?) body [onleave]
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

    UAstNode *event_ast   = n->u.at_event.event_expr;
    UAstNode *body_ast    = n->u.at_event.body;
    UAstNode *onleave_ast = n->u.at_event.onleave;
    bool      sync_flag   = n->u.at_event.is_sync;

    uint8_t event_reg = emit_expr(e, event_ast);
    if (e->error != EMIT_OK) return 0U;

    /* Sync freereg up to next_reg before allocating the body closure.
     * AST_IDENT global-fallback (line ~824) and the chains it feeds
     * (AST_MEMBER_GET et al.) bump only e->next_reg, leaving freereg
     * stale.  emit_function_literal allocates body_reg from freereg,
     * so without this sync body_reg can land on top of event_reg —
     * OP_CLOSURE then clobbers the event pointer at runtime.
     * AST_WATCHER avoids this by routing cond through
     * emit_function_literal symmetrically.
     * v0.5.2 freereg-sync fix (commit 6eb40a3) — do NOT remove. */
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;

    /* Body closure: 1 param (payload). */
    UAstNode payload_param;
    urbi_zero(&payload_param, sizeof payload_param);
    payload_param.kind               = AST_PARAM;
    payload_param.line               = body_ast ? body_ast->line : n->line;
    payload_param.col                = 1;
    payload_param.u.param.name_start = "__payload";
    payload_param.u.param.name_len   = 9;
    UAstNode *params_arr[1] = { &payload_param };

    uint8_t body_reg = (body_ast != NULL)
        ? emit_function_literal(e, params_arr, 1, body_ast, /*as_expression=*/false)
        : 0xFFU;
    if (e->error != EMIT_OK) return 0U;

    uint8_t alt_reg = (onleave_ast != NULL)
        ? emit_function_literal(e, NULL, 0, onleave_ast, /*as_expression=*/false)
        : 0xFFU;
    if (e->error != EMIT_OK) return 0U;

    UOpcode op = sync_flag ? OP_AT_EVENT_SYNC_INSTALL : OP_AT_EVENT_INSTALL;
    emit_instr(e, uinstr_enc_abc(op, event_reg, body_reg, alt_reg),
               (uint32_t)n->line);

    /* EMIT-010 (Wave 5): unwind both cursors symmetrically. */
    if (alt_reg  != 0xFFU) free_reg_freereg_synced(e);
    if (body_reg != 0xFFU) free_reg_freereg_synced(e);
    free_reg_freereg_synced(e);  /* event_reg */

    uint8_t rd = e->next_reg;
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), (uint32_t)n->line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return rd;
}

/* =========================================================================
 * AST_AT_SLOT_CHANGE — at (obj.x.changed?) body [onleave] / at sync variant
 * ========================================================================= */

uint8_t emit_at_slot_change_arm(UEmitter *e, UAstNode *n) {
    /* T63: at (obj.x.changed?) body [onleave] / at sync variant.
     * Spec #4 §4.2: emit GETSLOT_CHANGE_EVENT then AT_EVENT_INSTALL.
     *
     *   recv_reg  := emit receiver expression
     *   ic_idx    := uemit_assign_ic_index for slot name
     *   event_reg := OP_GETSLOT_CHANGE_EVENT(event_reg, recv_reg, ic_idx)
     *   body_reg  := emit_function_literal(body, 1 param)
     *   alt_reg   := emit_function_literal(onleave, 0 params) or 0xFF
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

    uint8_t recv_reg = emit_expr(e, recv_ast);
    if (e->error != EMIT_OK) return 0U;

    USymbol *slot_sym = (USymbol *)ustr_intern(e->vm, sname, sname_len);
    if (slot_sym == NULL) { e->error = EMIT_OOM; return 0U; }

    int ic_idx = uemit_assign_ic_index(e, slot_sym);
    if (ic_idx < 0) return 0U;

    /* Emit the event-lookup; result overwrites recv_reg (same
     * register reuse as OP_GETSLOT in AST_MEMBER_GET). */
    uint8_t event_reg = recv_reg;
    emit_instr(e, uinstr_enc_abc(OP_GETSLOT_CHANGE_EVENT,
                                 event_reg, recv_reg, (uint8_t)ic_idx),
               (uint32_t)n->line);

    /* Sync freereg up to next_reg before allocating the body closure
     * (mirrors AST_AT_EVENT).  AST_IDENT global-fallback feeding
     * recv_ast bumps next_reg only, leaving freereg stale, so
     * emit_function_literal can otherwise allocate body_reg on top
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
        ? emit_function_literal(e, params_arr, 1, body_ast, /*as_expression=*/false)
        : 0xFFU;
    if (e->error != EMIT_OK) return 0U;

    uint8_t alt_reg = (onleave_ast != NULL)
        ? emit_function_literal(e, NULL, 0, onleave_ast, /*as_expression=*/false)
        : 0xFFU;
    if (e->error != EMIT_OK) return 0U;

    UOpcode op = sync_flag ? OP_AT_EVENT_SYNC_INSTALL : OP_AT_EVENT_INSTALL;
    emit_instr(e, uinstr_enc_abc(op, event_reg, body_reg, alt_reg),
               (uint32_t)n->line);

    /* EMIT-010 (Wave 5): unwind both cursors symmetrically. */
    if (alt_reg  != 0xFFU) free_reg_freereg_synced(e);
    if (body_reg != 0xFFU) free_reg_freereg_synced(e);
    free_reg_freereg_synced(e);  /* event_reg */

    uint8_t rd = e->next_reg;
    emit_instr(e, uinstr_enc_abc(OP_LOADNIL, rd, 0U, 0U), (uint32_t)n->line);
    e->next_reg++;
    if (e->next_reg > e->max_reg_seen) e->max_reg_seen = e->next_reg;
    if (e->current_fs->freereg < e->next_reg)
        e->current_fs->freereg = e->next_reg;
    return rd;
}
