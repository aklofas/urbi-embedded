/* SPDX-License-Identifier: BSD-3-Clause */
/* Reactive parser fragments: at / whenever / waituntil / tag-prefix. */

#include "parse/uparse_internal.h"
#include "watcher/uwatcher.h"
#include <stddef.h>
#include "lex/ulex.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "value/uarena.h"

/* --- desugar_postfix_emit: common helper for postfix `e!` desugar.
 *
 * Called after the `!` token has been consumed.  bang_tok carries the
 * source position.  recv is the left-hand-side expression.
 *
 * Produces:
 *   e!        → AST_CALL { callee = AST_MEMBER_GET(recv, "emit"), args=[], count=0 }
 *   e!(p)     → AST_CALL { callee = AST_MEMBER_GET(recv, "emit"), args=[p], count=1 }
 *   e!(x,y,z) → PARSE_EMIT_MULTI_ARG_V1 error
 *
 * Returns NULL on OOM, AST_ERROR on parse error, or the call node. */
UAstNode *desugar_postfix_emit(UParser *p, UAstNode *recv, UToken bang_tok) {
    UAstNode *member = make_node(p, AST_MEMBER_GET, bang_tok.line, bang_tok.col);
    if (!member) return NULL;
    member->u.member.recv       = recv;
    member->u.member.name_start = kEmitMethodName;
    member->u.member.name_len   = kEmitMethodNameLen;
    member->u.member.value      = NULL;
    if (peek(p).type == TOK_LPAREN) {
        consume(p);  /* consume '(' */
        int arg_count = 0;
        UAstNode *arg0 = NULL;
        if (peek(p).type != TOK_RPAREN && peek(p).type != TOK_EOF) {
            arg0 = parse_inner_tier(p);
            if (!arg0) return NULL;
            if (arg0->kind == AST_ERROR) return arg0;
            arg_count = 1;
            if (peek(p).type == TOK_COMMA) {
                UToken comma = consume(p);
                return make_error(p, PARSE_EMIT_MULTI_ARG_V1,
                                  kErrorMessages[PARSE_EMIT_MULTI_ARG_V1],
                                  comma.line, comma.col);
            }
        }
        UToken rp = peek(p);
        if (rp.type != TOK_RPAREN) {
            return make_error(p, PARSE_EXPECTED_RPAREN,
                              kErrorMessages[PARSE_EXPECTED_RPAREN],
                              rp.line, rp.col);
        }
        consume(p);  /* consume ')' */
        UAstNode **args = NULL;
        if (arg_count > 0) {
            args = (UAstNode **)uarena_alloc(p->arena, sizeof(UAstNode *));
            if (!args) return (UAstNode *)&uparser_oom_sentinel;
            args[0] = arg0;
        }
        UAstNode *call = make_node(p, AST_CALL, bang_tok.line, bang_tok.col);
        if (!call) return NULL;
        call->u.call.callee    = member;
        call->u.call.args      = args;
        call->u.call.arg_count = arg_count;
        return call;
    }
    /* Bare `e!` — zero-arg emit call. */
    UAstNode *call = make_node(p, AST_CALL, bang_tok.line, bang_tok.col);
    if (!call) return NULL;
    call->u.call.callee    = member;
    call->u.call.args      = NULL;
    call->u.call.arg_count = 0;
    return call;
}

/* --- parse_tag_prefix: `name : { body }`
   Called from parse_statement_or_expr after consuming `name` and seeing `:`.
   Produces AST_TAG_PREFIX with tag_expr = AST_IDENT(name), body = AST_BLOCK.
   onleave is always NULL at M3 (M5 wires on-leave syntax). --- */

UAstNode *parse_tag_prefix(UParser *p, UToken name_tok) {
    consume(p);  /* consume ':' */

    UAstNode *body = parse_block(p);
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    UAstNode *tag_expr = make_ident(p, name_tok.u.str.start, name_tok.u.str.len,
                                    name_tok.line, name_tok.col);
    if (!tag_expr) return (UAstNode *)&uparser_oom_sentinel;

    UAstNode *node = make_node(p, AST_TAG_PREFIX, name_tok.line, name_tok.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.tag_prefix.tag_expr = tag_expr;
    node->u.tag_prefix.body     = body;
    node->u.tag_prefix.onleave  = NULL;  /* deferred to M5 */
    return node;
}

/* --- parse_at_slot_change_form: `at (obj.x.changed?) body [onleave h]`
 *
 * Called when cond is a ≥3-segment `obj.x.changed` MEMBER_GET chain.
 * kw is the `at` token (for node position); cond, body, onleave, is_sync
 * are already parsed by parse_at_event_form. */
static UAstNode *parse_at_slot_change_form(UParser *p, UToken kw,
                                            UAstNode *cond,
                                            UAstNode *body, UAstNode *onleave,
                                            bool is_sync) {
    UAstNode *slot_node = cond->u.member.recv;  /* the .x MEMBER_GET */
    UAstNode *node = make_node(p, AST_AT_SLOT_CHANGE, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.at_slot_change.receiver      = slot_node->u.member.recv;
    node->u.at_slot_change.slot_name     = slot_node->u.member.name_start;
    node->u.at_slot_change.slot_name_len = (size_t)slot_node->u.member.name_len;
    node->u.at_slot_change.body          = body;
    node->u.at_slot_change.onleave       = onleave;
    node->u.at_slot_change.is_sync       = is_sync;
    return node;
}

/* --- parse_at_event_form: `at [sync] (e?) body [onleave h]`
 *
 * Called after `?` has been consumed.  kw is `at` position; cond is
 * the expression before `?`; is_sync reflects the `at sync` modifier.
 * Expects `)` as the next token, then body, optional onleave.
 * Disambiguates slot-change vs plain event form. */
static UAstNode *parse_at_event_form(UParser *p, UToken kw,
                                      UAstNode *cond, bool is_sync) {
    UToken rp2 = peek(p);
    if (rp2.type != TOK_RPAREN) {
        return make_error(p, PARSE_EXPECTED_RPAREN,
                          kErrorMessages[PARSE_EXPECTED_RPAREN],
                          rp2.line, rp2.col);
    }
    consume(p);

    UAstNode *body = parse_statement_or_expr(p);
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    /* Optional `onleave` handler. */
    UAstNode *onleave = NULL;
    if (peek(p).type == TOK_KW_ONLEAVE) {
        consume(p);
        onleave = parse_statement_or_expr(p);
        if (!onleave) return (UAstNode *)&uparser_oom_sentinel;
        if (onleave->kind == AST_ERROR) return onleave;
    }

    /* Spec #4 §4.3–§4.5: disambiguate slot-change form.
     * at (obj.x.changed?) → AST_AT_SLOT_CHANGE when:
     *   cond is AST_MEMBER_GET with name=="changed"
     *   AND cond->recv is also AST_MEMBER_GET (≥3 path segments)
     * at (obj.changed?)   → AST_AT_EVENT (2 segments, falls through) */
    if (cond->kind == AST_MEMBER_GET
        && ident_equals(cond->u.member.name_start,
                        cond->u.member.name_len,
                        "changed", 7)
        && cond->u.member.recv != NULL
        && cond->u.member.recv->kind == AST_MEMBER_GET) {
        /* 3+ segments: slot-change form. */
        return parse_at_slot_change_form(p, kw, cond, body, onleave, is_sync);
    }

    /* 2 segments or non-"changed" final segment: event form. */
    UAstNode *node = make_node(p, AST_AT_EVENT, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.at_event.event_expr = cond;
    node->u.at_event.body       = body;
    node->u.at_event.onleave    = onleave;
    node->u.at_event.is_sync    = is_sync;
    return node;
}

/* --- parse_at_cond_form: `at [sync] (cond) body [onleave h]`
 *
 * Called when there is no `?` after cond.  kw is `at` position; cond
 * is the condition expression; mode is UWATCHER_AT or UWATCHER_AT_SYNC.
 * Expects `)` as the next token, then body, optional onleave. */
static UAstNode *parse_at_cond_form(UParser *p, UToken kw,
                                     UAstNode *cond, int mode) {
    UToken rp = peek(p);
    if (rp.type != TOK_RPAREN) {
        return make_error(p, PARSE_EXPECTED_RPAREN,
                          kErrorMessages[PARSE_EXPECTED_RPAREN],
                          rp.line, rp.col);
    }
    consume(p);

    UAstNode *body = parse_statement_or_expr(p);
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    /* Optional `onleave` handler — not allowed with `at sync`. */
    UAstNode *onleave = NULL;
    if (peek(p).type == TOK_KW_ONLEAVE) {
        if (mode == UWATCHER_AT_SYNC) {
            UToken ol = consume(p);
            return make_error(p, PARSE_UNEXPECTED_TOKEN,
                              "onleave not allowed with at sync",
                              ol.line, ol.col);
        }
        consume(p);
        onleave = parse_statement_or_expr(p);
        if (!onleave) return (UAstNode *)&uparser_oom_sentinel;
        if (onleave->kind == AST_ERROR) return onleave;
    }

    UAstNode *node = make_node(p, AST_WATCHER, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.watcher.cond    = cond;
    node->u.watcher.body    = body;
    node->u.watcher.onleave = onleave;
    node->u.watcher.mode    = mode;
    return node;
}

/* --- parse_at: `at` [`sync`|`async`] `(` cond[?] `)` body [`onleave` handler]
 *
 * Postfix `?` inside the parentheses selects the event-subscribe form:
 *   at (e?) body            → AST_AT_EVENT (sync_flag=false)
 *   at sync (e?) body       → AST_AT_EVENT (sync_flag=true)
 * Without `?`, produces AST_WATCHER as before. */
UAstNode *parse_at(UParser *p) {
    UToken kw = consume(p);  /* consume TOK_KW_AT */

    /* Optional `sync` or `async` modifier. */
    int mode = UWATCHER_AT;
    bool is_sync = false;
    UToken mod = peek(p);
    if (mod.type == TOK_KW_SYNC) {
        consume(p);
        mode = UWATCHER_AT_SYNC;
        is_sync = true;
    } else if (mod.type == TOK_KW_ASYNC) {
        consume(p);
        /* `at async` is accepted as `at` (redundant modifier); silent at v1.0. */
        mode = UWATCHER_AT;
    }

    UToken lp = peek(p);
    if (lp.type != TOK_LPAREN) {
        return make_error(p, PARSE_EXPECTED_LPAREN,
                          kErrorMessages[PARSE_EXPECTED_LPAREN],
                          lp.line, lp.col);
    }
    consume(p);

    /* Enable the at_event_cond context so that `?` in the inner expression
     * is not immediately flagged as an error — parse_at checks for it after
     * the expression parse returns. */
    p->at_event_cond = true;
    UAstNode *cond = parse_inner_tier(p);
    p->at_event_cond = false;
    if (!cond) return (UAstNode *)&uparser_oom_sentinel;
    if (cond->kind == AST_ERROR) return cond;

    /* Check for trailing `?` — event-subscribe or slot-change form. */
    if (peek(p).type == TOK_QUESTION) {
        consume(p);  /* consume '?' */
        return parse_at_event_form(p, kw, cond, is_sync);
    }

    /* No `?` — conditional watcher form. */
    return parse_at_cond_form(p, kw, cond, mode);
}

/* --- parse_whenever: `whenever` `(` cond `)` body [`onleave` handler] --- */
UAstNode *parse_whenever(UParser *p) {
    UToken kw = consume(p);  /* consume TOK_KW_WHENEVER */

    UToken lp = peek(p);
    if (lp.type != TOK_LPAREN) {
        return make_error(p, PARSE_EXPECTED_LPAREN,
                          kErrorMessages[PARSE_EXPECTED_LPAREN],
                          lp.line, lp.col);
    }
    consume(p);

    UAstNode *cond = parse_inner_tier(p);
    if (!cond) return (UAstNode *)&uparser_oom_sentinel;
    if (cond->kind == AST_ERROR) return cond;

    UToken rp = peek(p);
    if (rp.type != TOK_RPAREN) {
        return make_error(p, PARSE_EXPECTED_RPAREN,
                          kErrorMessages[PARSE_EXPECTED_RPAREN],
                          rp.line, rp.col);
    }
    consume(p);

    UAstNode *body = parse_statement_or_expr(p);
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    /* Optional `onleave` handler. */
    UAstNode *onleave = NULL;
    if (peek(p).type == TOK_KW_ONLEAVE) {
        consume(p);
        onleave = parse_statement_or_expr(p);
        if (!onleave) return (UAstNode *)&uparser_oom_sentinel;
        if (onleave->kind == AST_ERROR) return onleave;
    }

    UAstNode *node = make_node(p, AST_WATCHER, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.watcher.cond    = cond;
    node->u.watcher.body    = body;
    node->u.watcher.onleave = onleave;
    node->u.watcher.mode    = UWATCHER_WHENEVER;
    return node;
}

/* --- parse_waituntil: `waituntil` `(` cond `)` --- */
UAstNode *parse_waituntil(UParser *p) {
    UToken kw = consume(p);  /* consume TOK_KW_WAITUNTIL */

    UToken lp = peek(p);
    if (lp.type != TOK_LPAREN) {
        return make_error(p, PARSE_EXPECTED_LPAREN,
                          kErrorMessages[PARSE_EXPECTED_LPAREN],
                          lp.line, lp.col);
    }
    consume(p);

    UAstNode *cond = parse_inner_tier(p);
    if (!cond) return (UAstNode *)&uparser_oom_sentinel;
    if (cond->kind == AST_ERROR) return cond;

    UToken rp = peek(p);
    if (rp.type != TOK_RPAREN) {
        return make_error(p, PARSE_EXPECTED_RPAREN,
                          kErrorMessages[PARSE_EXPECTED_RPAREN],
                          rp.line, rp.col);
    }
    consume(p);

    UAstNode *node = make_node(p, AST_WAITUNTIL, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.waituntil.cond = cond;
    return node;
}
