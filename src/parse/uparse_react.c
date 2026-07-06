/* SPDX-License-Identifier: BSD-3-Clause */
/* Reactive parser fragments: at / whenever / waituntil / tag-prefix. */

#include "parse/uparse_internal.h"
#include "watcher/uwatcher.h"
#include <stddef.h>
#include "lex/ulex.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "value/uarena.h"

/* --- urbi_parse_desugar_postfix_emit: common helper for postfix `e!` desugar.
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
UAstNode *urbi_parse_desugar_postfix_emit(UParser *p, UAstNode *recv, UToken bang_tok) {
    UAstNode *member = urbi_parse_make_node(p, AST_MEMBER_GET, bang_tok.line, bang_tok.col);
    if (!member) return NULL;
    member->u.member.recv       = recv;
    member->u.member.name_start = urbi_parse_kEmitMethodName;
    member->u.member.name_len   = kEmitMethodNameLen;
    member->u.member.value      = NULL;
    if (urbi_parse_peek(p).type == TOK_LPAREN) {
        urbi_parse_consume(p);  /* urbi_parse_consume '(' */
        int arg_count = 0;
        UAstNode *arg0 = NULL;
        if (urbi_parse_peek(p).type != TOK_RPAREN && urbi_parse_peek(p).type != TOK_EOF) {
            arg0 = urbi_parse_inner_tier(p);
            if (!arg0) return NULL;
            if (arg0->kind == AST_ERROR) return arg0;
            arg_count = 1;
            if (urbi_parse_peek(p).type == TOK_COMMA) {
                UToken comma = urbi_parse_consume(p);
                return urbi_parse_make_error(p, PARSE_EMIT_MULTI_ARG_V1,
                                  urbi_parse_kErrorMessages[PARSE_EMIT_MULTI_ARG_V1],
                                  comma.line, comma.col);
            }
        }
        { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }  /* urbi_parse_consume ')' */
        UAstNode **args = NULL;
        if (arg_count > 0) {
            args = (UAstNode **)uarena_alloc(p->arena, sizeof(UAstNode *));
            if (!args) return (UAstNode *)&uparser_oom_sentinel;
            args[0] = arg0;
        }
        UAstNode *call = urbi_parse_make_node(p, AST_CALL, bang_tok.line, bang_tok.col);
        if (!call) return NULL;
        call->u.call.callee    = member;
        call->u.call.args      = args;
        call->u.call.arg_count = arg_count;
        return call;
    }
    /* Bare `e!` — zero-arg emit call. */
    UAstNode *call = urbi_parse_make_node(p, AST_CALL, bang_tok.line, bang_tok.col);
    if (!call) return NULL;
    call->u.call.callee    = member;
    call->u.call.args      = NULL;
    call->u.call.arg_count = 0;
    return call;
}

/* === v0.10.5: tag-expr widening ===
 *
 * parse_tag_prefix_body: shared body-parse helper for both `name:` and
 * `expr:` tag-prefix forms.  Called after `:` has been consumed.
 * `pos_line`/`pos_col` are the position of the tag expression (for the
 * implicit-block node position).
 *
 * PARSE-033 closure: the AST_TAG_PREFIX.onleave field is always NULL at
 * v1.0 — the surface form `tag: { body } onleave handler` is v1.x scope
 * (spec deferred it; class stdlib confirmed v1.0 ships without it).
 * `at (cond) body onleave handler` (AST_WATCHER) IS the supported
 * onleave form today; see uast.h tag_prefix.onleave for the canonical
 * comment. */
static UAstNode *parse_tag_prefix_body(UParser *p, UAstNode *tag_expr,
                                        int pos_line, int pos_col) {
    UAstNode *body;
    if (urbi_parse_peek(p).type == TOK_LBRACE) {
        body = urbi_parse_block(p);
    } else {
        UAstNode *stmt = urbi_parse_statement_or_expr(p);
        if (!stmt) return (UAstNode *)&uparser_oom_sentinel;
        if (stmt->kind == AST_ERROR) return stmt;
        /* Wrap in single-statement block so the emit path's AST_BLOCK
         * handler runs without modification. */
        body = urbi_parse_make_node(p, AST_BLOCK, pos_line, pos_col);
        if (!body) return (UAstNode *)&uparser_oom_sentinel;
        UAstNode **stmts = (UAstNode **)uarena_alloc(p->arena, sizeof(UAstNode *));
        if (!stmts) return (UAstNode *)&uparser_oom_sentinel;
        stmts[0] = stmt;
        body->u.block.stmts = stmts;
        body->u.block.count = 1;
    }
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    UAstNode *node = urbi_parse_make_node(p, AST_TAG_PREFIX, pos_line, pos_col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.tag_prefix.tag_expr = tag_expr;
    node->u.tag_prefix.body     = body;
    node->u.tag_prefix.onleave  = NULL;  /* tag-prefix onleave is v1.x — see fn comment + uast.h */
    return node;
}

/* --- urbi_parse_tag_prefix: `name : body`
   Called from parse_assign_or_expr after consuming `name` and seeing `:`.
   Produces AST_TAG_PREFIX with tag_expr = AST_IDENT(name).
   v0.10.2: body may be bare stmt (no braces required) — both forms
   produce an AST_BLOCK child so the emit path is uniform.
   Partially closes legacy audit F3; member-expr tag form closed by. */
UAstNode *urbi_parse_tag_prefix(UParser *p, UToken name_tok) {
    urbi_parse_consume(p);  /* urbi_parse_consume ':' */
    UAstNode *tag_expr = urbi_parse_make_ident(p, name_tok.u.str.start, name_tok.u.str.len,
                                    name_tok.line, name_tok.col);
    if (!tag_expr) return (UAstNode *)&uparser_oom_sentinel;
    return parse_tag_prefix_body(p, tag_expr, name_tok.line, name_tok.col);
}

/* --- urbi_parse_tag_prefix_from_expr: `expr : body`                   (v0.10.5)
 *
 * Called from parse_assign_or_expr when a postfix-chain expression is
 * followed by `:` at statement level.  Enables `Tag.scope: { body }` and
 * other member-expr tag forms (legacy manual §9.1.1 example).
 *
 * Contract: `:` has already been peeked (but NOT consumed) by the caller.
 * `tag_expr` is the fully-parsed expression to the left of `:`.
 * Closes legacy audit finding F3 (member-expr tag position). */
UAstNode *urbi_parse_tag_prefix_from_expr(UParser *p, UAstNode *tag_expr) {
    urbi_parse_consume(p);  /* urbi_parse_consume ':' */
    return parse_tag_prefix_body(p, tag_expr, tag_expr->line, tag_expr->col);
}
/* === end v0.10.5 === */

/* --- parse_event_payload_binding: `(var x)` optional suffix after `?`
 *
 * v0.10.5: handles the optional payload-binding suffix that may appear
 * after `?` in event-subscribe forms:
 *   at (e?(var result)) body
 *   whenever (e?(var n)) body
 *   waituntil (e?(var x))
 *
 * Called after the preceding `?` has been consumed.  If the next token is
 * NOT `(`, the function returns NULL (success) with *out_name=NULL and
 * *out_len=0 — the payload variable defaults to `__payload` in the emitter.
 *
 * If `(` is present the function consumes `(var ident)` and writes the
 * identifier into *out_name and *out_len.  Returns an AST_ERROR node on any
 * parse failure, NULL on success.  The returned pointer is used as:
 *   UAstNode *err = parse_event_payload_binding(...);
 *   if (err) return err;   // propagate error */
static UAstNode *parse_event_payload_binding(UParser *p,
                                              const char **out_name,
                                              int         *out_len)
{
    *out_name = NULL;
    *out_len  = 0;

    if (urbi_parse_peek(p).type != TOK_LPAREN)
        return NULL;   /* no payload binding — use default __payload */

    urbi_parse_consume(p);   /* urbi_parse_consume '(' */

    /* Expect `var` keyword. */
    if (urbi_parse_peek(p).type != TOK_KW_VAR) {
        UToken bad = urbi_parse_peek(p);
        return urbi_parse_make_error(p, PARSE_EVENT_PAYLOAD_BIND_EXPECTED_VAR,
                          urbi_parse_kErrorMessages[PARSE_EVENT_PAYLOAD_BIND_EXPECTED_VAR],
                          bad.line, bad.col);
    }
    urbi_parse_consume(p);   /* urbi_parse_consume 'var' */

    /* Expect identifier. */
    if (urbi_parse_peek(p).type != TOK_IDENT) {
        UToken bad = urbi_parse_peek(p);
        return urbi_parse_make_error(p, PARSE_EVENT_PAYLOAD_BIND_EXPECTED_IDENT,
                          urbi_parse_kErrorMessages[PARSE_EVENT_PAYLOAD_BIND_EXPECTED_IDENT],
                          bad.line, bad.col);
    }
    UToken id_tok = urbi_parse_consume(p);
    *out_name = id_tok.u.str.start;
    *out_len  = (int)id_tok.u.str.len;

    /* Expect closing ')'. */
    if (urbi_parse_peek(p).type != TOK_RPAREN) {
        UToken bad = urbi_parse_peek(p);
        return urbi_parse_make_error(p, PARSE_EVENT_PAYLOAD_BIND_EXPECTED_RPAREN,
                          urbi_parse_kErrorMessages[PARSE_EVENT_PAYLOAD_BIND_EXPECTED_RPAREN],
                          bad.line, bad.col);
    }
    urbi_parse_consume(p);   /* urbi_parse_consume ')' */
    return NULL;  /* success */
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
    UAstNode *node = urbi_parse_make_node(p, AST_AT_SLOT_CHANGE, kw.line, kw.col);
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
    /* Optional `(var x)` payload binding immediately after `?` and
     * before the `)` that closes the at-condition. */
    const char *pname = NULL;
    int         plen  = 0;
    UAstNode *perr = parse_event_payload_binding(p, &pname, &plen);
    if (perr) return perr;

    { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }

    UAstNode *body = urbi_parse_statement_or_expr(p);
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    /* Optional `onleave` handler. */
    UAstNode *onleave = NULL;
    if (urbi_parse_peek(p).type == TOK_KW_ONLEAVE) {
        urbi_parse_consume(p);
        onleave = urbi_parse_statement_or_expr(p);
        if (!onleave) return (UAstNode *)&uparser_oom_sentinel;
        if (onleave->kind == AST_ERROR) return onleave;
    }

    /* Spec #4 §4.3–§4.5: disambiguate slot-change form.
     * at (obj.x.changed?) → AST_AT_SLOT_CHANGE when:
     *   cond is AST_MEMBER_GET with name=="changed"
     *   AND cond->recv is also AST_MEMBER_GET (≥3 path segments)
     * at (obj.changed?)   → AST_AT_EVENT (2 segments, falls through) */
    if (cond->kind == AST_MEMBER_GET
        && urbi_parse_ident_equals(cond->u.member.name_start,
                        cond->u.member.name_len,
                        "changed", 7)
        && cond->u.member.recv != NULL
        && cond->u.member.recv->kind == AST_MEMBER_GET) {
        /* 3+ segments: slot-change form. */
        return parse_at_slot_change_form(p, kw, cond, body, onleave, is_sync);
    }

    /* 2 segments or non-"changed" final segment: event form. */
    UAstNode *node = urbi_parse_make_node(p, AST_AT_EVENT, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.at_event.event_expr      = cond;
    node->u.at_event.body            = body;
    node->u.at_event.onleave         = onleave;
    node->u.at_event.is_sync         = is_sync;
    node->u.at_event.is_whenever     = false;  /* At (e?) is not whenever */
    node->u.at_event.payload_var_name = pname;  /* User name or NULL */
    node->u.at_event.payload_var_len  = plen;
    return node;
}

/* --- parse_at_cond_form: `at [sync] (cond) body [onleave h]`
 *
 * Called when there is no `?` after cond.  kw is `at` position; cond
 * is the condition expression; mode is UWATCHER_AT or UWATCHER_AT_SYNC.
 * Expects `)` as the next token, then body, optional onleave. */
static UAstNode *parse_at_cond_form(UParser *p, UToken kw,
                                     UAstNode *cond, int mode) {
    { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }

    UAstNode *body = urbi_parse_statement_or_expr(p);
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    /* Optional `onleave` handler — not allowed with `at sync`.
     * PARSE-009: report a dedicated code so callers can distinguish this
     * specific conflict from the generic PARSE_UNEXPECTED_TOKEN. */
    UAstNode *onleave = NULL;
    if (urbi_parse_peek(p).type == TOK_KW_ONLEAVE) {
        if (mode == UWATCHER_AT_SYNC) {
            UToken ol = urbi_parse_consume(p);
            return urbi_parse_make_error(p, PARSE_AT_SYNC_DOES_NOT_SUPPORT_ONLEAVE,
                              urbi_parse_kErrorMessages[PARSE_AT_SYNC_DOES_NOT_SUPPORT_ONLEAVE],
                              ol.line, ol.col);
        }
        urbi_parse_consume(p);
        onleave = urbi_parse_statement_or_expr(p);
        if (!onleave) return (UAstNode *)&uparser_oom_sentinel;
        if (onleave->kind == AST_ERROR) return onleave;
    }

    UAstNode *node = urbi_parse_make_node(p, AST_WATCHER, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.watcher.cond      = cond;
    node->u.watcher.body      = body;
    node->u.watcher.onleave   = onleave;
    node->u.watcher.else_body = NULL;   /* Only WHENEVER sets else_body */
    node->u.watcher.mode      = mode;
    return node;
}

/* --- urbi_parse_at: `at` [`sync`|`async`] `(` cond[?] `)` body [`onleave` handler]
 *
 * Postfix `?` inside the parentheses selects the event-subscribe form:
 *   at (e?) body            → AST_AT_EVENT (sync_flag=false)
 *   at sync (e?) body       → AST_AT_EVENT (sync_flag=true)
 * Without `?`, produces AST_WATCHER as before. */
UAstNode *urbi_parse_at(UParser *p) {
    UToken kw = urbi_parse_consume(p);  /* urbi_parse_consume TOK_KW_AT */

    /* Optional `sync` or `async` modifier. */
    int mode = UWATCHER_AT;
    bool is_sync = false;
    UToken mod = urbi_parse_peek(p);
    if (mod.type == TOK_KW_SYNC) {
        urbi_parse_consume(p);
        mode = UWATCHER_AT_SYNC;
        is_sync = true;
    } else if (mod.type == TOK_KW_ASYNC) {
        urbi_parse_consume(p);
        /* `at async` is accepted as `at` (redundant modifier); silent at v1.0. */
        mode = UWATCHER_AT;
    }

    { UAstNode *err = NULL; if (!expect(p, TOK_LPAREN, PARSE_EXPECTED_LPAREN, &err)) return err; }

    /* Enable the at_event_cond context so that `?` in the inner expression
     * is not immediately flagged as an error — urbi_parse_at checks for it after
     * the expression parse returns.  Save/restore rather than write false:
     * a nested event form (waituntil is an expression primary) would
     * otherwise clobber an enclosing condition's flag (refactor-3 FE-22). */
    bool saved_at_event_cond = p->at_event_cond;
    p->at_event_cond = true;
    UAstNode *cond = urbi_parse_inner_tier(p);
    p->at_event_cond = saved_at_event_cond;
    if (!cond) return (UAstNode *)&uparser_oom_sentinel;
    if (cond->kind == AST_ERROR) return cond;

    /* Check for trailing `?` — event-subscribe or slot-change form. */
    if (urbi_parse_peek(p).type == TOK_QUESTION) {
        urbi_parse_consume(p);  /* urbi_parse_consume '?' */
        return parse_at_event_form(p, kw, cond, is_sync);
    }

    /* No `?` — conditional watcher form. */
    return parse_at_cond_form(p, kw, cond, mode);
}

/* --- urbi_parse_whenever: `whenever` `(` cond `)` body [`onleave` handler]
 *                   | `whenever` `(` event `?` `)` body [`onleave` handler]
 *
 * v0.10.2: the event arm (TOK_QUESTION after cond) mirrors urbi_parse_at's
 * parse_at_event_form path.  Produces AST_AT_EVENT with is_whenever=true.
 * The cond arm (no `?`) produces AST_WATCHER with mode=UWATCHER_WHENEVER
 * as before. */
UAstNode *urbi_parse_whenever(UParser *p) {
    UToken kw = urbi_parse_consume(p);  /* urbi_parse_consume TOK_KW_WHENEVER */

    { UAstNode *err = NULL; if (!expect(p, TOK_LPAREN, PARSE_EXPECTED_LPAREN, &err)) return err; }

    /* Enable the at_event_cond context so that `?` in the inner expression
     * is not immediately flagged as an error — urbi_parse_whenever checks for it
     * after the expression parse returns.  Mirrors urbi_parse_at's pattern,
     * including the FE-22 save/restore (no absolute clear). */
    bool saved_at_event_cond = p->at_event_cond;
    p->at_event_cond = true;
    UAstNode *cond = urbi_parse_inner_tier(p);
    p->at_event_cond = saved_at_event_cond;
    if (!cond) return (UAstNode *)&uparser_oom_sentinel;
    if (cond->kind == AST_ERROR) return cond;

    /* Event-arm branch — mirror urbi_parse_at's TOK_QUESTION handling.
     * `whenever (e?) body` is a perpetual event subscriber: the body
     * re-fires on every emission of e, without one-shot teardown.
     * Optional `(var x)` payload binding after `?`. */
    if (urbi_parse_peek(p).type == TOK_QUESTION) {
        urbi_parse_consume(p);  /* urbi_parse_consume '?' */

        /* Optional payload binding. */
        const char *pname = NULL;
        int         plen  = 0;
        UAstNode *perr = parse_event_payload_binding(p, &pname, &plen);
        if (perr) return perr;

        { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }
        UAstNode *body = urbi_parse_statement_or_expr(p);
        if (!body) return (UAstNode *)&uparser_oom_sentinel;
        if (body->kind == AST_ERROR) return body;
        UAstNode *onleave = NULL;
        if (urbi_parse_peek(p).type == TOK_KW_ONLEAVE) {
            urbi_parse_consume(p);
            onleave = urbi_parse_statement_or_expr(p);
            if (!onleave) return (UAstNode *)&uparser_oom_sentinel;
            if (onleave->kind == AST_ERROR) return onleave;
        }
        UAstNode *node = urbi_parse_make_node(p, AST_AT_EVENT, kw.line, kw.col);
        if (!node) return (UAstNode *)&uparser_oom_sentinel;
        node->u.at_event.event_expr       = cond;
        node->u.at_event.body             = body;
        node->u.at_event.onleave          = onleave;
        node->u.at_event.is_sync          = false;  /* whenever has no sync form */
        node->u.at_event.is_whenever      = true;   /* Distinguishes from at (e?) */
        node->u.at_event.payload_var_name = pname;  /* User name or NULL */
        node->u.at_event.payload_var_len  = plen;
        return node;
    }

    { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }

    UAstNode *body = urbi_parse_statement_or_expr(p);
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    /* Optional `onleave` handler. */
    UAstNode *onleave = NULL;
    if (urbi_parse_peek(p).type == TOK_KW_ONLEAVE) {
        urbi_parse_consume(p);
        onleave = urbi_parse_statement_or_expr(p);
        if (!onleave) return (UAstNode *)&uparser_oom_sentinel;
        if (onleave->kind == AST_ERROR) return onleave;
    }

    /* `whenever (cond) body else else_body` — falling-edge handler.
     * `else` is consumed only for WHENEVER mode; `at` does not support it. */
    UAstNode *else_body = NULL;
    if (urbi_parse_peek(p).type == TOK_KW_ELSE) {
        urbi_parse_consume(p);
        else_body = urbi_parse_statement_or_expr(p);
        if (!else_body) return (UAstNode *)&uparser_oom_sentinel;
        if (else_body->kind == AST_ERROR) return else_body;
    }

    UAstNode *node = urbi_parse_make_node(p, AST_WATCHER, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.watcher.cond      = cond;
    node->u.watcher.body      = body;
    node->u.watcher.onleave   = onleave;
    node->u.watcher.else_body = else_body;  /* Nullable falling-edge handler */
    node->u.watcher.mode      = UWATCHER_WHENEVER;
    return node;
}

/* --- urbi_parse_every: `every` `(` period `)` body
 *
 * Pure parser-level desugar (no new AST node kind, no new opcode):
 *
 *   every (E) S   =>   every(E, function () { S })
 *
 * The desugared call resolves at runtime to the stdlib C-native function
 * `every` registered by urbi_stdlib_boot.  Wrapping the body in a
 * zero-parameter function literal gives it the closure semantics the
 * runtime helper expects (re-invoked each tick; captures enclosing
 * lexical scope via the existing upvalue mechanism).  The body is
 * parsed with urbi_parse_statement_or_expr — same shape as the at/whenever
 * body — so any legal statement form, including a brace block, works.
 *
 * The original urbi v2 surface used the retired `closure { ... }` form
 * here; v1.0 substitutes `function () { ... }` — the difference is
 * at-call `this` binding vs. lexical, which doesn't
 * affect the body shape `every` runs (the helper invokes the closure
 * with no explicit receiver).
 */
UAstNode *urbi_parse_every(UParser *p) {
    UToken kw = urbi_parse_consume(p);  /* urbi_parse_consume TOK_KW_EVERY */

    { UAstNode *err = NULL; if (!expect(p, TOK_LPAREN, PARSE_EXPECTED_LPAREN, &err)) return err; }

    UAstNode *period = urbi_parse_inner_tier(p);
    if (!period) return (UAstNode *)&uparser_oom_sentinel;
    if (period->kind == AST_ERROR) return period;

    { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }

    UAstNode *body = urbi_parse_statement_or_expr(p);
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    /* Wrap the body in a zero-parameter function literal — params=NULL,
     * param_count=0.  urbi_emit_function_literal accepts any node shape for
     * the body (it routes through urbi_emit_expr internally), so single-
     * statement bodies need no AST_BLOCK wrapping. */
    UAstNode *body_fn = urbi_parse_make_node(p, AST_FUNCTION, kw.line, kw.col);
    if (!body_fn) return (UAstNode *)&uparser_oom_sentinel;
    body_fn->u.func.params      = NULL;
    body_fn->u.func.param_count = 0;
    body_fn->u.func.body        = body;

    /* Build the 2-arg call `every(period, body_fn)`.  Callee is a bare
     * IDENT — runtime resolution finds the stdlib C-native function. */
    UAstNode *callee = urbi_parse_make_ident(p, "every", 5, kw.line, kw.col);
    if (!callee) return (UAstNode *)&uparser_oom_sentinel;

    UAstNode **args = (UAstNode **)uarena_alloc(p->arena,
                                                 2U * sizeof(UAstNode *));
    if (!args) return (UAstNode *)&uparser_oom_sentinel;
    args[0] = period;
    args[1] = body_fn;

    UAstNode *call = urbi_parse_make_node(p, AST_CALL, kw.line, kw.col);
    if (!call) return (UAstNode *)&uparser_oom_sentinel;
    call->u.call.callee    = callee;
    call->u.call.args      = args;
    call->u.call.arg_count = 2;
    return call;
}

/* --- urbi_parse_waituntil: `waituntil` `(` cond[?[(var x)]] `)`
 *
 * v0.10.5: two forms:
 *   waituntil (cond)          — condition-based block; existing form
 *   waituntil (e?)            — event-subscribe block; desugars to e.waituntil()
 *   waituntil (e?(var x))     — event-subscribe with named payload binding
 *
 * The event form is identified by trailing `?` after the condition expression,
 * mirroring urbi_parse_at's pattern.  The cond form is unchanged. */
UAstNode *urbi_parse_waituntil(UParser *p) {
    UToken kw = urbi_parse_consume(p);  /* urbi_parse_consume TOK_KW_WAITUNTIL */

    { UAstNode *err = NULL; if (!expect(p, TOK_LPAREN, PARSE_EXPECTED_LPAREN, &err)) return err; }

    /* Enable at_event_cond so `?` in the inner expression isn't flagged.
     * Save/restore: waituntil is an expression primary, so this very parse
     * can sit inside another condition's flag scope (refactor-3 FE-22). */
    bool saved_at_event_cond = p->at_event_cond;
    p->at_event_cond = true;
    UAstNode *cond = urbi_parse_inner_tier(p);
    p->at_event_cond = saved_at_event_cond;
    if (!cond) return (UAstNode *)&uparser_oom_sentinel;
    if (cond->kind == AST_ERROR) return cond;

    /* Detect trailing `?` — event form. */
    bool is_event_form = false;
    const char *pname = NULL;
    int         plen  = 0;
    if (urbi_parse_peek(p).type == TOK_QUESTION) {
        urbi_parse_consume(p);  /* urbi_parse_consume '?' */
        is_event_form = true;

        /* Optional payload binding `(var x)`. */
        UAstNode *perr = parse_event_payload_binding(p, &pname, &plen);
        if (perr) return perr;
    }

    { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }

    UAstNode *node = urbi_parse_make_node(p, AST_WAITUNTIL, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.waituntil.cond             = cond;
    node->u.waituntil.is_event_form    = is_event_form;
    node->u.waituntil.payload_var_name = pname;
    node->u.waituntil.payload_var_len  = plen;
    return node;
}
