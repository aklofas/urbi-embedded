/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_react.c — the concurrency separators `,` and `&`, and the one
 * watcher arm every reactive form compiles through.
 *
 * `a, b, c` forks a zero-parameter thunk per child but the last, which
 * runs inline on the current strand and gives the statement its value;
 * nothing waits for the forks.
 *
 * `a & b` builds the right-hand thunk first and pins it, runs the left
 * inline, then forks the thunk joined and waits for it.  The fork comes
 * after the left-hand side, not before it, because a child's handle is
 * only safe to wait on immediately after the fork that made it: a dead
 * child is reaped eagerly, so the verifier requires JOIN_WAIT to follow
 * its joining FORK.  A run `a & b & c` folds left: `(a & b) & c`.
 *
 * A watcher fills three consecutive registers -- the source, the body
 * closure, the alternate closure -- and INSTALLs from the first; the
 * flags say which of the last two the runtime may read.  `waituntil
 * (e?)` is not an install: it is the event's own `waituntil()` method,
 * whose value is the payload the event was emitted with. */

#include "emit/uemit_internal.h"
#include "chunk/uchunk.h"
#include "parse/uast.h"
#include "util/uarena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static uint32_t line_of(const UAstNode *n) {
    return n->line > 0 ? (uint32_t)n->line : 0U;
}

static uint8_t load_void(UEmitter *e, int want, uint32_t line) {
    uint8_t d = uemit_target(e, want);
    (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADVOID, d, 0U, 0U), line);
    return d;
}

/* --- `,` ------------------------------------------------------------------- */

/*     CLOSURE t, P(a); FORK t detach
 *     CLOSURE t, P(b); FORK t detach
 *     <c -> d> */
uint8_t useq_comma(UEmitter *e, UAstNode *n, int want) {
    int count = n->u.seq.count;
    for (int i = 0; i < count - 1 && e->error == EMIT_OK; i++) {
        UAstNode *c = n->u.seq.children[i];
        uint8_t base = e->fs->freereg;
        uint8_t t = uexpr_function(e, NULL, 0, c, false);
        (void)uinstr_emit(e, uinstr_enc_abc(OP_FORK, t, 0xFFU, UFORK_DETACH), line_of(c));
        ureg_free_to(e, base);
    }
    UAstNode *last = n->u.seq.children[count - 1];
    if (want < 0) return uexpr_any(e, last);
    uexpr_to(e, last, (uint8_t)want);
    return (uint8_t)want;
}

/* --- `&` ------------------------------------------------------------------- */

/* SEQ(&, [c0 .. cN]):
 *
 *     CLOSURE pN, P(cN)            pins, one per child but the first,
 *     ...                          the last child's lowest
 *     CLOSURE p1, P(c1)
 *     <c0>
 *     FORK p1 -> h join; JOIN_WAIT h
 *     ...
 *     FORK pN -> h join; JOIN_WAIT h
 *     LOADVOID d
 *
 * which is `((c0 & c1) & ...) & cN`.  Each pin is the newest local when
 * its join is emitted, so it is unpinned right there. */
uint8_t useq_amp(UEmitter *e, UAstNode *n, int want) {
    int count = n->u.seq.count;
    UAstNode **c = n->u.seq.children;
    uint8_t *pins = (uint8_t *)uarena_alloc(e->arena, (size_t)count * sizeof(uint8_t));
    if (pins == NULL) { (void)uemit_fail(e, EMIT_OOM); return 0U; }
    for (int k = count - 1; k >= 1 && e->error == EMIT_OK; k--) {
        pins[k] = upin(e);
        uexpr_function_to(e, NULL, 0, c[k], false, pins[k]);
    }
    ustmt(e, c[0]);
    for (int k = 1; k < count && e->error == EMIT_OK; k++) {
        uint8_t h = ureg_alloc(e);
        (void)uinstr_emit(e, uinstr_enc_abc(OP_FORK, pins[k], h, UFORK_JOIN), line_of(c[k]));
        (void)uinstr_emit(e, uinstr_enc_abc(OP_JOIN_WAIT, h, 0U, 0U), line_of(c[k]));
        uunpin(e, pins[k]);
    }
    return load_void(e, want, line_of(n));
}

/* --- watchers ---------------------------------------------------------------- */

static uint8_t install_mode(int mode, UAstWatchSource source) {
    bool cond = (source == UWSRC_COND);
    switch (mode) {
    case UWATCHER_AT_SYNC:  return cond ? UINSTALL_AT_SYNC_COND : UINSTALL_AT_SYNC_EVENT;
    case UWATCHER_WHENEVER: return cond ? UINSTALL_WHENEVER_COND : UINSTALL_WHENEVER_EVENT;
    case UWATCHER_WAITUNTIL: return UINSTALL_WAITUNTIL;
    default:                return cond ? UINSTALL_AT_COND : UINSTALL_AT_EVENT;
    }
}

/* `waituntil (e?)`: `e.waituntil()`, whose value is the payload. */
static uint8_t wait_event(UEmitter *e, UAstNode *n, int want) {
    UAstNode member, call;
    urbi_zero(&member, sizeof member);
    member.kind = AST_MEMBER_GET;
    member.line = n->line;
    member.col = n->col;
    member.u.member.recv = n->u.watcher.cond;
    member.u.member.name_start = "waituntil";
    member.u.member.name_len = 9;
    urbi_zero(&call, sizeof call);
    call.kind = AST_CALL;
    call.line = n->line;
    call.col = n->col;
    call.u.call.callee = &member;
    if (want < 0) return uexpr_any(e, &call);
    uexpr_to(e, &call, (uint8_t)want);
    return (uint8_t)want;
}

/* The body closure.  A condition's body takes nothing; an event's takes
 * the payload, under the `(var x)` name when the script gave one. */
static void body_closure(UEmitter *e, UAstNode *n, uint8_t dst) {
    UAstNode *body = n->u.watcher.body;
    if (n->u.watcher.source == UWSRC_COND) {
        uexpr_function_to(e, NULL, 0, body, false, dst);
        return;
    }
    UAstNode param;
    urbi_zero(&param, sizeof param);
    param.kind = AST_PARAM;
    param.line = body->line;
    param.col = 1;
    if (n->u.watcher.payload_var != NULL) {
        param.u.param.name_start = n->u.watcher.payload_var;
        param.u.param.name_len = n->u.watcher.payload_var_len;
    } else {
        param.u.param.name_start = "__payload";
        param.u.param.name_len = 9;
    }
    UAstNode *params[1] = { &param };
    uexpr_function_to(e, params, 1, body, false, dst);
}

/*     R[base]   = source             condition closure, event, or slot-change event
 *     R[base+1] = body closure       when there is a body
 *     R[base+2] = alternate closure  the whenever `else`, else the onleave
 *     INSTALL base, mode, flags
 *     LOADNIL d */
uint8_t uwatch_install_node(UEmitter *e, UAstNode *n, int want) {
    uint32_t line = line_of(n);
    int mode = n->u.watcher.mode;
    UAstWatchSource source = n->u.watcher.source;
    UAstNode *cond = n->u.watcher.cond;

    if (mode == UWATCHER_WAITUNTIL && source != UWSRC_COND) return wait_event(e, n, want);
    if (source == UWSRC_COND && urbi_emit_cond_has_direct_side_effect(cond))
        urbi_emit_diag_warn(e, cond,
                            "watcher condition has direct write/assignment; "
                            "may cause feedback loop at runtime");

    uint8_t base = ureg_alloc(e);
    (void)ureg_alloc(e);
    (void)ureg_alloc(e);
    if (source == UWSRC_COND) {
        uexpr_function_to(e, NULL, 0, cond, true, base);
    } else if (source == UWSRC_EVENT) {
        uexpr_to(e, cond, base);
    } else {
        uint8_t r = uexpr_any(e, cond);
        const char *name = uemit_intern(e, n->u.watcher.slot_name, n->u.watcher.slot_name_len);
        if (name == NULL) return 0U;
        uslot_emit(e, OP_GETSLOT_CHANGE_EVENT, base, r, usite(e, name), line);
        ureg_free_to(e, (uint8_t)(base + 3U));
    }

    uint8_t flags = 0U;
    if (mode != UWATCHER_WAITUNTIL) {
        if (n->u.watcher.body != NULL) {
            body_closure(e, n, (uint8_t)(base + 1U));
            flags |= UINSTALL_F_HAS_BODY;
        }
        UAstNode *alt = (mode == UWATCHER_WHENEVER && n->u.watcher.else_body != NULL)
                      ? n->u.watcher.else_body : n->u.watcher.onleave;
        if (alt != NULL) {
            uexpr_function_to(e, NULL, 0, alt, false, (uint8_t)(base + 2U));
            flags |= UINSTALL_F_HAS_ALT;
        }
    }
    (void)uinstr_emit(e, uinstr_enc_abc(OP_INSTALL, base, install_mode(mode, source), flags), line);
    ureg_free_to(e, base);
    uint8_t d = uemit_target(e, want);
    (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADNIL, d, 0U, 0U), line);
    return d;
}
