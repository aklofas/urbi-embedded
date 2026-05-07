/* SPDX-License-Identifier: BSD-3-Clause */
/* Reactive parser fragments: at / whenever / waituntil / tag-prefix. */

#include "parse/uparse_internal.h"
#include "watcher/uwatcher.h"
#include <stddef.h>

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

/* --- parse_at: `at` [`sync`|`async`] `(` cond[?] `)` body [`onleave` handler]
 *
 * Postfix `?` inside the parentheses selects the event-subscribe form:
 *   at (e?) body            → AST_AT_EVENT (sync_flag=false)
 *   at sync (e?) body       → AST_AT_EVENT (sync_flag=true)
 * Without `?`, produces AST_WATCHER as before. --- */
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

    /* Check for trailing `?` — event-subscribe form. */
    if (peek(p).type == TOK_QUESTION) {
        UToken q = consume(p);  /* consume '?' */
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
            UAstNode *slot_node = cond->u.member.recv;  /* the .x MEMBER_GET */
            UAstNode *node = make_node(p, AST_AT_SLOT_CHANGE, kw.line, kw.col);
            if (!node) return (UAstNode *)&uparser_oom_sentinel;
            node->u.at_slot_change.receiver      = slot_node->u.member.recv;
            node->u.at_slot_change.slot_name     = slot_node->u.member.name_start;
            node->u.at_slot_change.slot_name_len = (size_t)slot_node->u.member.name_len;
            node->u.at_slot_change.body          = body;
            node->u.at_slot_change.onleave       = onleave;
            node->u.at_slot_change.is_sync       = is_sync;
            (void)q;
            return node;
        }

        /* 2 segments or non-"changed" final segment: event form. */
        UAstNode *node = make_node(p, AST_AT_EVENT, kw.line, kw.col);
        if (!node) return (UAstNode *)&uparser_oom_sentinel;
        node->u.at_event.event_expr = cond;
        node->u.at_event.body       = body;
        node->u.at_event.onleave    = onleave;
        node->u.at_event.is_sync    = is_sync;
        (void)q;  /* position used for kw */
        return node;
    }

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
