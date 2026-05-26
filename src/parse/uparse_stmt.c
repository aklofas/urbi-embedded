/* SPDX-License-Identifier: BSD-3-Clause */
/* uparse_stmt.c — statement-level parse functions.
 * Extracted from uparse.c during v0.5.4-decompose (PARSE-021 #4). */

#include "parse/uparse.h"
#include "parse/uparse_internal.h"
#include "lex/ulex.h"
#include "parse/uast.h"
#include "value/uarena.h"
#include <stddef.h>

/* --- parse_var_decl: `var x = expr` --- */

UAstNode *parse_var_decl(UParser *p) {
    UToken kw = consume(p);          /* consume TOK_KW_VAR */
    UToken name = peek(p);

    /* Detect reserved keywords used as variable names (T4, spec #2 §3.11).
     * PARSE-007: `async` was previously treated as soft (allowed in
     * var-decl) but `async = 2` failed at the assignment site because
     * parse_statement_or_expr has no IDENT-fallthrough for TOK_KW_ASYNC.
     * That asymmetry meant `var async = 1` succeeded but the variable
     * could never be re-assigned — silently un-usable.  Resolution: treat
     * TOK_KW_ASYNC as fully reserved (matches its modifier role in
     * `at async (...)` per parse_react.c). */
    if (name.type == TOK_KW_AT       || name.type == TOK_KW_WHENEVER  ||
        name.type == TOK_KW_WAITUNTIL || name.type == TOK_KW_ONLEAVE  ||
        name.type == TOK_KW_SYNC      || name.type == TOK_KW_ASYNC) {
        return make_error(p, PARSE_RESERVED_KEYWORD_AS_IDENT,
                          kErrorMessages[PARSE_RESERVED_KEYWORD_AS_IDENT],
                          name.line, name.col);
    }

    if (name.type != TOK_IDENT) {
        return make_error(p, PARSE_EXPECTED_IDENT,
                          kErrorMessages[PARSE_EXPECTED_IDENT],
                          name.line, name.col);
    }
    consume(p);

    /* === W10/v0.10.5: `var obj.slot = value` slot-install form.
     *
     * If the token after the IDENT is TOK_DOT (not TOK_EQ), this is the
     * legacy slot-install form `var obj.slot = value`.  Desugar to
     * `obj.slot = value` (AST_MEMBER_SET): OP_SETSLOT installs the slot
     * when absent, so no new opcode is needed.
     *
     * Handles arbitrarily deep chains: `var a.b.c = v` →
     *   temp = a.b  (AST_MEMBER_GET)
     *   temp.c = v  (AST_MEMBER_SET)
     * achieved naturally by building a full `a.b.c = v` AST_MEMBER_SET
     * tree where the receiver is AST_MEMBER_GET for `a.b`.
     *
     * Ruling: implemented (Wave 6 W10, legacy F14). */
    if (peek(p).type == TOK_DOT) {
        /* Build receiver node from the already-consumed IDENT. */
        UAstNode *recv = make_ident(p, name.u.str.start, name.u.str.len,
                                    name.line, name.col);
        if (!recv) return NULL;
        /* Parse one or more `.slot` member-access suffixes.  After the final
         * DOT we expect `IDENT = value`; intermediate DOTs extend the chain. */
        for (;;) {
            consume(p);  /* consume TOK_DOT */
            UToken slot_name = peek(p);
            if (slot_name.type != TOK_IDENT) {
                return make_error(p, PARSE_EXPECTED_IDENT,
                                  kErrorMessages[PARSE_EXPECTED_IDENT],
                                  slot_name.line, slot_name.col);
            }
            consume(p);
            if (peek(p).type == TOK_DOT) {
                /* Intermediate: build MEMBER_GET and continue. */
                UAstNode *mg = make_node(p, AST_MEMBER_GET,
                                         slot_name.line, slot_name.col);
                if (!mg) return NULL;
                mg->u.member.recv       = recv;
                mg->u.member.name_start = slot_name.u.str.start;
                mg->u.member.name_len   = slot_name.u.str.len;
                mg->u.member.value      = NULL;
                recv = mg;
                continue;
            }
            /* Final slot: consume `=` and parse RHS, produce MEMBER_SET. */
            UToken eq2 = peek(p);
            if (eq2.type != TOK_EQ) {
                return make_error(p, PARSE_VAR_OBJ_SLOT_NO_INIT,
                                  kErrorMessages[PARSE_VAR_OBJ_SLOT_NO_INIT],
                                  eq2.line, eq2.col);
            }
            consume(p);
            UAstNode *val = parse_expression(p, 0);
            if (!val) return NULL;
            if (val->kind == AST_ERROR) return val;
            UAstNode *ms = make_node(p, AST_MEMBER_SET,
                                      slot_name.line, slot_name.col);
            if (!ms) return NULL;
            ms->u.member.recv       = recv;
            ms->u.member.name_start = slot_name.u.str.start;
            ms->u.member.name_len   = slot_name.u.str.len;
            ms->u.member.value      = val;
            return ms;
        }
    }
    /* === end W10/v0.10.5: var obj.slot form === */

    UToken eq = peek(p);
    if (eq.type != TOK_EQ) {
        return make_error(p, PARSE_EXPECTED_EQ,
                          kErrorMessages[PARSE_EXPECTED_EQ],
                          eq.line, eq.col);
    }
    consume(p);

    /* S48-followup (2026-05-16): parse RHS as a Pratt expression, NOT
     * parse_inner_tier — same root cause as the S48 fix to MEMBER_SET.
     * `var x = 1 | y = 2` should parse as `(var x = 1) | (y = 2)` per
     * legacy spec (see legacy/repos/aldebaran-urbi/tests/2.x/atomic.chk
     * `var n = 0 | {};` pattern).  Pre-fix, parse_inner_tier absorbed
     * the `|` into the init expression, producing nested wrong-AST. */
    UAstNode *init = parse_expression(p, 0);
    if (!init) return NULL;
    if (init->kind == AST_ERROR) return init;

    UAstNode *node = make_node(p, AST_VAR_DECL, kw.line, kw.col);
    if (!node) return NULL;
    node->u.var_decl.name_start = name.u.str.start;
    node->u.var_decl.name_len   = name.u.str.len;
    node->u.var_decl.init       = init;
    return node;
}

/* --- parse_assign_after_eq_peek: `x = expr`.
 *
 * Caller contract (closes PARSE-012):
 *   - `name` is the already-consumed IDENT token (passed by value).
 *   - The next lexer token MUST be TOK_EQ; the caller has already
 *     peeked and confirmed it.  This function consumes the TOK_EQ
 *     and parses the RHS.  Calling it without that hidden lookahead
 *     state would mis-parse the expression.
 *
 * The function name encodes that lexer-state precondition explicitly —
 * earlier name `parse_assign_from_ident` did not. --- */

UAstNode *parse_assign_after_eq_peek(UParser *p, UToken name) {
    /* TOK_EQ already peeked/confirmed by caller; consume it. */
    consume(p);

    /* S48-followup (2026-05-16): parse RHS as a Pratt expression, NOT
     * parse_inner_tier — same root cause as the S48 fix to MEMBER_SET.
     * `x = 1 | y = 2` should parse as `(x = 1) | (y = 2)`.  Without
     * this fix, parse_inner_tier absorbs the `|` into the assign RHS. */
    UAstNode *value = parse_expression(p, 0);
    if (!value) return NULL;
    if (value->kind == AST_ERROR) return value;

    UAstNode *node = make_node(p, AST_ASSIGN, name.line, name.col);
    if (!node) return NULL;
    node->u.assign.name_start = name.u.str.start;
    node->u.assign.name_len   = name.u.str.len;
    node->u.assign.value      = value;
    return node;
}

/* parse_assign_or_expr: IDENT already consumed as `name`.
   Handles assignment (`x = expr`), tag-prefix (`mytag: { body }` and
   `expr: { body }`), and expression statements that start with an
   identifier (call chains, member accesses, arithmetic).
   The non-assign/non-tag path delegates to parse_inner_tier_from_lhs
   to avoid duplicating the Pratt loop.

   W8/v0.10.5: member-expr tag form.  After parsing a postfix chain
   (member-access, calls, etc.) from the leading IDENT, if the result
   is followed by `:` at statement level, treat the whole expression as
   the tag-expr of an AST_TAG_PREFIX.  This enables `Tag.scope: body`
   and similar forms.  The check is inserted between parse_expression_cont
   (which builds the chain) and pipe_amp_fold (which folds `|`/`&`) —
   exactly the split that parse_inner_tier_from_lhs performs internally. */
static UAstNode *parse_assign_or_expr(UParser *p, UToken name) {
    /* T41 statement-start getter/setter sugar: `get IDENT (...)` or
     * `set IDENT (...)` produces an AST_PROPERTY_DECL.  Recognized only
     * in the strict 3-token shape `get|set IDENT (`; outside that
     * pattern, `get`/`set` remain plain identifiers (no keyword
     * reservation breakage).  The receiver is implicit (NULL); emit
     * resolves it from the enclosing class body or top-level realm
     * scope. */
    if ((ident_equals(name.u.str.start, name.u.str.len, "get", 3) ||
         ident_equals(name.u.str.start, name.u.str.len, "set", 3))
        && peek(p).type == TOK_IDENT && peek2(p).type == TOK_LPAREN) {
        /* T41 (Phase 2 follow-up): the implicit-receiver form is legal
         * only inside a `class { ... }` body.  At statement start there
         * is no v1.0 resolver for the implicit `this`; reject with a
         * dedicated diagnostic instead of falling through to a generic
         * EMIT_UNSUPPORTED_AST at emit time.  Deferred to v1.x. */
        if (p->class_body_depth == 0) {
            return make_error(p, PARSE_TOPLEVEL_GETSET_NOT_SUPPORTED,
                              kErrorMessages[PARSE_TOPLEVEL_GETSET_NOT_SUPPORTED],
                              name.line, name.col);
        }
        UAstMethodKind kind =
            ident_equals(name.u.str.start, name.u.str.len, "get", 3)
                ? UAST_METHOD_GETTER : UAST_METHOD_SETTER;
        UToken slot_name = consume(p);  /* consume the slot-name IDENT */
        return parse_property_decl(p, /*recv=*/NULL, slot_name, kind,
                                   name.line, name.col);
    }

    if (peek(p).type == TOK_EQ) {
        return parse_assign_after_eq_peek(p, name);
    }
    /* Tag-prefix: `mytag: { body }`.  At statement level, `:` has no
     * other meaning (not an infix operator, not a separator), so seeing
     * IDENT followed by COLON unambiguously introduces a tag scope. */
    if (peek(p).type == TOK_COLON) {
        return parse_tag_prefix(p, name);
    }
    /* Not assignment or bare-IDENT tag: build the ident node and run the
     * Pratt climb (parse_expression_cont) to collect postfix chains such
     * as `.member`, `(args)`, `!`.  Then check for `:` again — a colon
     * after a postfix chain is the member-expr tag form `Tag.scope: body`
     * (W8/v0.10.5).  If no colon, finish with the pipe/amp fold as before. */
    UAstNode *lhs = make_ident(p, name.u.str.start, name.u.str.len,
                               name.line, name.col);
    if (!lhs) return NULL;
    /* Pratt climb: builds full postfix chain from the leading IDENT. */
    lhs = parse_expression_cont(p, lhs, 0);
    if (!lhs) return NULL;
    if (lhs->kind == AST_ERROR) return lhs;
    /* === W8/v0.10.5: member-expr tag check === */
    if (peek(p).type == TOK_COLON) {
        return parse_tag_prefix_from_expr(p, lhs);
    }
    /* === end W8/v0.10.5 === */
    /* Normal expression statement: fold `|` and `&` separators. */
    return pipe_amp_fold(p, lhs);
}

/* --- parse_class_declaration: `class Name [: public P1, P2, ...] { body }`.
 *
 * Phase 6 of M6 stdlib (T38).  Per S-mro-declaration-order, the proto
 * array preserves declaration left-to-right order; the emitter inserts
 * protos in REVERSE order during desugar so the resulting chain ends up
 * [P1, P2, Object] for `: public P1, P2`.
 *
 * Per S-class-name-scope: the class name is NOT in scope while parsing
 * either the proto list or the body.  Any name binding for the class
 * happens at emit time as part of the desugar (the emit arm allocates a
 * local and writes the cloned-Object value into it AFTER body emit).
 * That deferral makes `class a : public a { ... }` resolve the proto
 * `a` to the OUTER `a` (legacy class.chk shadow case).
 *
 * The `public` keyword is required after the colon for syntactic
 * compatibility with legacy urbi 2.x (which had access modifiers); v1.0
 * has no access semantics but the keyword stays as a layout marker.
 *
 * Proto identifiers parse as full primary expressions so that future
 * v1.x extensions (e.g. `class C : public M.Inner`) just work; for
 * Wave 1 we expect AST_IDENT but do not gate on it. --- */
UAstNode *parse_class_declaration(UParser *p) {
    UToken kw = consume(p);  /* consume TOK_KW_CLASS */

    /* Class name. */
    UToken name = peek(p);
    if (name.type != TOK_IDENT) {
        return make_error(p, PARSE_EXPECTED_IDENT,
                          kErrorMessages[PARSE_EXPECTED_IDENT],
                          name.line, name.col);
    }
    consume(p);

    /* Optional `: public P1[, P2, ...]`. */
    UAstNode **protos = NULL;
    int proto_count = 0;
    if (peek(p).type == TOK_COLON) {
        consume(p);  /* consume ':' */
        UToken pub_tok = peek(p);
        if (pub_tok.type != TOK_KW_PUBLIC) {
            return make_error(p, PARSE_UNEXPECTED_TOKEN,
                              "expected 'public' after ':' in class declaration",
                              pub_tok.line, pub_tok.col);
        }
        consume(p);  /* consume 'public' */

        /* Seed an initial proto array.  parse_prefix gives a single primary
         * expression — not a separator-tier or comma-tier expression — so
         * the comma stays available as the proto-list separator. */
        int proto_cap = 4;
        protos = (UAstNode **)uarena_alloc(p->arena,
                                           (size_t)proto_cap * sizeof(UAstNode *));
        if (protos == NULL) return (UAstNode *)&uparser_oom_sentinel;

        for (;;) {
            UAstNode *proto = parse_prefix(p);
            if (proto == NULL) return NULL;
            if (proto->kind == AST_ERROR) return proto;

            if (proto_count == proto_cap) {
                if (!arena_grow_node_array(p, &protos, &proto_cap,
                                           proto_count)) {
                    return (UAstNode *)&uparser_oom_sentinel;
                }
            }
            protos[proto_count++] = proto;

            if (peek(p).type != TOK_COMMA) break;
            consume(p);  /* consume ',' */
        }
    }

    /* Body — block.  Class name is NOT yet in scope; the body parses
     * with whatever outer binding `name` has (per S-class-name-scope).
     * Bump class_body_depth around the parse so statement-start
     * `get`/`set` inside the body skip the top-level rejection in
     * parse_assign_or_expr (T41 implicit-receiver form is legal in
     * class bodies, illegal at statement-start). */
    UToken body_tok = peek(p);
    if (body_tok.type != TOK_LBRACE) {
        return make_error(p, PARSE_EXPECTED_LBRACE,
                          kErrorMessages[PARSE_EXPECTED_LBRACE],
                          body_tok.line, body_tok.col);
    }
    p->class_body_depth++;
    UAstNode *body = parse_block(p);
    p->class_body_depth--;
    if (body == NULL) return NULL;
    if (body->kind == AST_ERROR) return body;

    UAstNode *node = make_node(p, AST_CLASS_DECL, kw.line, kw.col);
    if (node == NULL) return NULL;
    node->u.class_decl.name_start  = name.u.str.start;
    node->u.class_decl.name_len    = name.u.str.len;
    node->u.class_decl.protos      = protos;
    node->u.class_decl.proto_count = proto_count;
    node->u.class_decl.body        = body;
    return node;
}

/* --- parse_statement_or_expr: var-decl, assign, or inner-tier expression.
   Returns an inner-tier result (arithmetic expression, possibly with
   | / & separators). Used as the child-entry point for both
   uparse_next_statement and the outer-tier loop. --- */

UAstNode *parse_statement_or_expr(UParser *p) {
    UToken t = peek(p);

    switch (t.type) {
    case TOK_KW_WHILE:    return parse_while(p);
    case TOK_KW_IF:       return parse_if(p);
    case TOK_KW_VAR:      return parse_var_decl(p);
    case TOK_KW_RETURN:   return parse_return(p);
    case TOK_KW_TRY:      return parse_try(p);
    case TOK_KW_THROW:    return parse_throw(p);
    case TOK_KW_AT:       return parse_at(p);
    case TOK_KW_WHENEVER: return parse_whenever(p);
    case TOK_KW_WAITUNTIL: return parse_waituntil(p);
    case TOK_KW_EVERY:    return parse_every(p);
    case TOK_KW_CLASS:    return parse_class_declaration(p);
    /* W3/v0.10.5: assert keyword */
    case TOK_KW_ASSERT:   return parse_assert(p);
    /* === W1/v0.10.5: control flow === */
    case TOK_KW_FOR:      return parse_for(p);
    case TOK_KW_BREAK:    return parse_break(p);
    case TOK_KW_CONTINUE: return parse_continue(p);
    case TOK_KW_SWITCH:   return parse_switch(p);
    /* === end W1/v0.10.5: control flow === */
    /* S47 (2026-05-16): allow `{ stmts }` as a statement-or-expression.
     * Original urbi spec supports brace blocks in at-bodies, onleave
     * handlers, whenever bodies, and any inner-tier position (see
     * legacy aldebaran-urbi/tests/2.x/at/ .chk files for examples like
     * `at (e?) { ... }`, `at (cond) { ... } onleave { ... }`).
     * Without this, the parser falls through to parse_inner_tier →
     * parse_expression which doesn't accept LBRACE as an expression
     * prefix, producing "expected expression" at the first statement
     * inside the block.  Surfaced 2026-05-16 by eye_demo's attempt
     * to use a multi-statement at-body. */
    case TOK_LBRACE:      return parse_block(p);
    case TOK_IDENT: {
        /* x = expr — detect by consuming IDENT then peeking for TOK_EQ.
           mytag: { body } — detect by consuming IDENT then peeking for TOK_COLON.
           If neither, continue as a Pratt expression starting with this IDENT. */
        UToken name = consume(p);
        return parse_assign_or_expr(p, name);
    }
    default:
        return parse_inner_tier(p);
    }
}

/* --- parse_block: `{` stmts `}` → AST_BLOCK.
   Each statement inside is a full outer-tier parse (including `;` chains).
   Statements are separated by `;` or `|`; a missing separator ends the block.
   Used by if/else; T13 (while) and T14 (function) will reuse this. --- */
UAstNode *parse_block(UParser *p) {
    UToken lbrace = peek(p);
    if (lbrace.type != TOK_LBRACE) {
        return make_error(p, PARSE_EXPECTED_LBRACE,
                          kErrorMessages[PARSE_EXPECTED_LBRACE],
                          lbrace.line, lbrace.col);
    }
    consume(p);

    int cap = 4;
    UAstNode **stmts = (UAstNode **)uarena_alloc(p->arena,
                                                  (size_t)cap * sizeof(UAstNode *));
    if (!stmts) return (UAstNode *)&uparser_oom_sentinel;
    int count = 0;

    while (peek(p).type != TOK_RBRACE && peek(p).type != TOK_EOF) {
        UAstNode *s = parse_outer_tier(p);
        if (!s) return (UAstNode *)&uparser_oom_sentinel;
        if (s->kind == AST_ERROR) return s;

        if (count == cap) {
            if (!arena_grow_node_array(p, &stmts, &cap, count))
                return (UAstNode *)&uparser_oom_sentinel;
        }
        stmts[count++] = s;

        /* Statements within a block are separated by `;` or `|`.
         * `|` acts as the REPL-boundary convention inside blocks too.
         * If neither is present, the block ends (next token is `}` or
         * an expression starting another statement — stop and expect `}`).
         *
         * Trailing-separator handling: after consuming `;` or `|` we
         * fall back to the top of the while-loop; the loop guard then
         * exits on `}` or TOK_EOF, so a trailing `; }` or `| }` is
         * silently accepted (closes PARSE-010). */
        UToken sep = peek(p);
        /* `;` and `|` are both block-statement separators here (REPL-boundary
         * convention applies inside blocks too). Either consumes one token; a
         * trailing-separator `; }` / `| }` falls through to the loop guard
         * which exits on `}` or TOK_EOF (closes PARSE-010). */
        if (sep.type == TOK_SEMI || sep.type == TOK_PIPE) {
            consume(p);
        } else {
            break;
        }
    }

    UToken rbrace = peek(p);
    if (rbrace.type != TOK_RBRACE) {
        return make_error(p, PARSE_EXPECTED_RBRACE,
                          kErrorMessages[PARSE_EXPECTED_RBRACE],
                          rbrace.line, rbrace.col);
    }
    consume(p);

    UAstNode *node = make_node(p, AST_BLOCK, lbrace.line, lbrace.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.block.stmts = stmts;
    node->u.block.count = count;
    return node;
}

/* --- parse_while: `while` `(` cond `)` body-block --- */
UAstNode *parse_while(UParser *p) {
    UToken kw = consume(p);  /* consume TOK_KW_WHILE */

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

    /* W1/v0.10.5: bump loop_depth so break/continue are legal in body. */
    p->loop_depth++;
    UAstNode *body = parse_block(p);
    p->loop_depth--;
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    UAstNode *node = make_node(p, AST_WHILE, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.while_stmt.cond = cond;
    node->u.while_stmt.body = body;
    return node;
}

/* --- parse_if: `if` `(` cond `)` then-block [`else` else-block] --- */
UAstNode *parse_if(UParser *p) {
    UToken kw = consume(p);  /* consume TOK_KW_IF */

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

    UAstNode *then_block = parse_block(p);
    if (!then_block) return (UAstNode *)&uparser_oom_sentinel;
    if (then_block->kind == AST_ERROR) return then_block;

    UAstNode *else_block = NULL;
    if (peek(p).type == TOK_KW_ELSE) {
        consume(p);
        else_block = parse_block(p);
        if (!else_block) return (UAstNode *)&uparser_oom_sentinel;
        if (else_block->kind == AST_ERROR) return else_block;
    }

    UAstNode *node = make_node(p, AST_IF, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.if_stmt.cond       = cond;
    node->u.if_stmt.then_block = then_block;
    node->u.if_stmt.else_block = else_block;
    return node;
}

/* Returns an AST_ERROR node if the parser sees a syntactic shape that v1.0
 * rejects:
 *   - bare `function {` (no parens)
 *   - bare `function name {` (no parens)
 *   - named `function name(...) {` (PARSE-004: v1.0 has no named-function
 *     decl form; use `var name = function(...){...}` instead).
 * Returns NULL if the form is the supported anonymous `function(...){...}`. */
static UAstNode *reject_bare_function_forms(UParser *p) {
    UToken next = peek(p);
    if (next.type == TOK_LBRACE) {
        return make_error(p, PARSE_BARE_FUNCTION,
                          "bare 'function { body }' is retired at v1.0; "
                          "use 'function() { body }' (add empty parens). "
                          "Legacy bare functions ambiguously meant "
                          "either 0-arg or no-formals — v1.0 requires explicit parens",
                          next.line, next.col);
    }
    if (next.type == TOK_IDENT) {
        /* `function name {` is the bare named form; `function name(` is a
         * named-function decl (PARSE-004: not supported at v1.0). */
        UToken name_tok = consume(p);
        if (peek(p).type == TOK_LBRACE) {
            return make_error(p, PARSE_BARE_FUNCTION,
                              "bare 'function name { body }' is retired at v1.0; "
                              "use 'function name() { body }' (add empty parens)",
                              name_tok.line, name_tok.col);
        }
        return make_error(p, PARSE_NAMED_FUNCTION_NOT_SUPPORTED,
                          kErrorMessages[PARSE_NAMED_FUNCTION_NOT_SUPPORTED],
                          name_tok.line, name_tok.col);
    }
    return NULL;
}

/* --- parse_function: `function` [`name`] `(` params `)` `{` body `}` --- */

UAstNode *parse_function(UParser *p) {
    UToken kw = consume(p);   /* consume TOK_KW_FUNCTION */

    UAstNode *err = reject_bare_function_forms(p);
    if (err) return err;

    if (peek(p).type != TOK_LPAREN) {
        return make_error(p, PARSE_EXPECTED_LPAREN,
                          kErrorMessages[PARSE_EXPECTED_LPAREN],
                          peek(p).line, peek(p).col);
    }
    consume(p);  /* consume '(' */

    /* Parameter list. */
    int cap = 4;
    UAstNode **params = (UAstNode **)uarena_alloc(p->arena,
                                                   (size_t)cap * sizeof(UAstNode *));
    if (!params) return (UAstNode *)&uparser_oom_sentinel;
    int count = 0;

    while (peek(p).type != TOK_RPAREN && peek(p).type != TOK_EOF) {
        bool is_lazy = false;
        if (peek(p).type == TOK_KW_LAZY) {
            consume(p);
            is_lazy = true;
        }

        UToken name = peek(p);
        if (name.type != TOK_IDENT) {
            return make_error(p, PARSE_EXPECTED_IDENT,
                              kErrorMessages[PARSE_EXPECTED_IDENT],
                              name.line, name.col);
        }
        consume(p);

        UAstNode *pn = make_node(p, is_lazy ? AST_LAZY_PARAM : AST_PARAM,
                                 name.line, name.col);
        if (!pn) return (UAstNode *)&uparser_oom_sentinel;
        pn->u.param.name_start = name.u.str.start;
        pn->u.param.name_len   = name.u.str.len;

        if (count == cap) {
            if (!arena_grow_node_array(p, &params, &cap, count))
                return (UAstNode *)&uparser_oom_sentinel;
        }
        params[count++] = pn;

        if (peek(p).type == TOK_COMMA) {
            consume(p);
        } else {
            break;
        }
    }

    if (peek(p).type != TOK_RPAREN) {
        return make_error(p, PARSE_EXPECTED_RPAREN,
                          kErrorMessages[PARSE_EXPECTED_RPAREN],
                          peek(p).line, peek(p).col);
    }
    consume(p);  /* consume ')' */

    UAstNode *body = parse_block(p);
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    UAstNode *node = make_node(p, AST_FUNCTION, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.func.params      = params;
    node->u.func.param_count = count;
    node->u.func.body        = body;
    return node;
}

/* --- parse_property_decl: `(params) { body }` after a `get`/`set` IDENT.
 *
 * T41 (M6 Wave 2).  Caller has already consumed the leading IDENT
 * (`get`/`set`) and the following slot-name IDENT — `name_tok` carries
 * the slot-name token.  The current peek is `(`.
 *
 * The desugar is parse-only: no new opcodes.  The emit arm walks the
 * AST_FUNCTION inside u.property_decl.func and routes the resulting
 * closure to install_property (URBI_SLOT_FLAG_OGET / OSET) instead of
 * a plain setSlot.  Parses the same `(params) { body }` shape as
 * `parse_function`. --- */
UAstNode *parse_property_decl(UParser *p, UAstNode *recv, UToken name_tok,
                              UAstMethodKind kind, int line, int col) {
    if (peek(p).type != TOK_LPAREN) {
        return make_error(p, PARSE_EXPECTED_LPAREN,
                          kErrorMessages[PARSE_EXPECTED_LPAREN],
                          peek(p).line, peek(p).col);
    }
    consume(p);  /* consume '(' */

    /* Parameter list — identical shape to parse_function. */
    int cap = 4;
    UAstNode **params = (UAstNode **)uarena_alloc(p->arena,
                                                   (size_t)cap * sizeof(UAstNode *));
    if (!params) return (UAstNode *)&uparser_oom_sentinel;
    int count = 0;

    while (peek(p).type != TOK_RPAREN && peek(p).type != TOK_EOF) {
        bool is_lazy = false;
        if (peek(p).type == TOK_KW_LAZY) {
            consume(p);
            is_lazy = true;
        }

        UToken pname = peek(p);
        if (pname.type != TOK_IDENT) {
            return make_error(p, PARSE_EXPECTED_IDENT,
                              kErrorMessages[PARSE_EXPECTED_IDENT],
                              pname.line, pname.col);
        }
        consume(p);

        UAstNode *pn = make_node(p, is_lazy ? AST_LAZY_PARAM : AST_PARAM,
                                 pname.line, pname.col);
        if (!pn) return (UAstNode *)&uparser_oom_sentinel;
        pn->u.param.name_start = pname.u.str.start;
        pn->u.param.name_len   = pname.u.str.len;

        if (count == cap) {
            if (!arena_grow_node_array(p, &params, &cap, count))
                return (UAstNode *)&uparser_oom_sentinel;
        }
        params[count++] = pn;

        if (peek(p).type == TOK_COMMA) {
            consume(p);
        } else {
            break;
        }
    }

    if (peek(p).type != TOK_RPAREN) {
        return make_error(p, PARSE_EXPECTED_RPAREN,
                          kErrorMessages[PARSE_EXPECTED_RPAREN],
                          peek(p).line, peek(p).col);
    }
    consume(p);  /* consume ')' */

    UAstNode *body = parse_block(p);
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    /* Build the inner AST_FUNCTION carrying params + body. */
    UAstNode *func = make_node(p, AST_FUNCTION, line, col);
    if (!func) return (UAstNode *)&uparser_oom_sentinel;
    func->u.func.params      = params;
    func->u.func.param_count = count;
    func->u.func.body        = body;

    /* Wrap in AST_PROPERTY_DECL. */
    UAstNode *node = make_node(p, AST_PROPERTY_DECL, line, col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.property_decl.recv       = recv;        /* may be NULL */
    node->u.property_decl.name_start = name_tok.u.str.start;
    node->u.property_decl.name_len   = name_tok.u.str.len;
    node->u.property_decl.kind       = kind;
    node->u.property_decl.func       = func;
    return node;
}

/* --- parse_return: `return [expr]` --- */

UAstNode *parse_return(UParser *p) {
    UToken kw = consume(p);  /* consume TOK_KW_RETURN */

    /* Determine whether a return value follows.  Stop at any statement-ending
       token: EOF, `}`, `)`, `|`, `;`, or `,`. */
    UAstNode *value = NULL;
    {
        UTokenType nt = peek(p).type;
        bool no_value = nt == TOK_EOF
                     || nt == TOK_RBRACE
                     || nt == TOK_RPAREN
                     || nt == TOK_PIPE
                     || nt == TOK_SEMI
                     || nt == TOK_COMMA;
        if (!no_value) {
            /* S48-followup (2026-05-16): `return EXPR | rest` should parse
             * as `(return EXPR) | rest` (return is final per legacy
             * aldebaran-urbi convention).  Same root cause as the
             * MEMBER_SET / var-decl / local-assign fixes: parse_inner_tier
             * absorbs the pipe into the return value. */
            value = parse_expression(p, 0);
            if (!value) return (UAstNode *)&uparser_oom_sentinel;
            if (value->kind == AST_ERROR) return value;
        }
    }

    UAstNode *node = make_node(p, AST_RETURN, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.ret.value = value;
    return node;
}

/* --- parse_throw: `throw expr` --- */

UAstNode *parse_throw(UParser *p) {
    UToken kw = consume(p);  /* consume TOK_KW_THROW */

    /* S48-followup (2026-05-16): `throw EXPR | rest` should parse as
     * `(throw EXPR) | rest` per legacy aldebaran-urbi convention (see
     * aldebaran-urbi/tests/2.x/urbistyle.chk for `throw Exception.new(...) |`
     * patterns).  Same root cause as MEMBER_SET / var-decl / local-assign
     * fixes: parse_inner_tier would absorb the pipe into the throw value. */
    UAstNode *value = parse_expression(p, 0);
    if (!value) return (UAstNode *)&uparser_oom_sentinel;
    if (value->kind == AST_ERROR) return value;

    UAstNode *node = make_node(p, AST_THROW, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.throw_expr.value = value;
    return node;
}

/* === W3/v0.10.5: assert keyword ===
 * parse_assert — `assert(expr)` or `assert { block }`.
 *
 * Paren form:   assert(expr)
 *   Records the source text span of `expr` for use in the failure diagnostic.
 *   src_text points into the source buffer between the `(` and `)` characters
 *   (trailing whitespace trimmed).
 *
 * Block form:   assert { stmts }
 *   Evaluates the block; truthy final value = pass (no throw).
 *   src_text/src_len = NULL/0.
 *
 * Ruling: implemented (Wave 6 W3, legacy F9).
 * Lowers at emit time to: if (!expr) throw "assertion failed[: <src>]"
 * No new opcode needed. */
UAstNode *parse_assert(UParser *p) {
    UToken kw = consume(p);  /* consume TOK_KW_ASSERT */

    UToken next = peek(p);

    if (next.type == TOK_LBRACE) {
        /* Block form: assert { stmts } */
        UAstNode *block = parse_block(p);
        if (!block) return (UAstNode *)&uparser_oom_sentinel;
        if (block->kind == AST_ERROR) return block;

        UAstNode *node = make_node(p, AST_ASSERT, kw.line, kw.col);
        if (!node) return (UAstNode *)&uparser_oom_sentinel;
        node->u.assert_stmt.expr     = block;
        node->u.assert_stmt.src_text = NULL;
        node->u.assert_stmt.src_len  = 0;
        return node;
    }

    if (next.type != TOK_LPAREN) {
        return make_error(p, PARSE_EXPECTED_LPAREN,
                          kErrorMessages[PARSE_EXPECTED_LPAREN],
                          next.line, next.col);
    }
    consume(p);  /* consume '(' */

    /* Capture source text start: p->lex->cur is now right after '('.
     * Trim leading whitespace so the diagnostic text starts at the expression.
     * The source buffer is guaranteed to outlive the AST node. */
    const char *src_start = p->lex->cur;
    while (*src_start == ' ' || *src_start == '\t'
           || *src_start == '\r' || *src_start == '\n') {
        src_start++;
    }

    UAstNode *expr = parse_inner_tier(p);
    if (!expr) return (UAstNode *)&uparser_oom_sentinel;
    if (expr->kind == AST_ERROR) return expr;

    /* Compute source text end: after parse_inner_tier, the parser has peeked
     * the first token after the expression.  That peeked token (')') was
     * produced by ulex_next which advanced p->lex->cur past ')'.
     * The ')' starts at (p->lex->cur - p->peek.len) when have_peek is set. */
    const char *src_end = p->have_peek ? (p->lex->cur - (size_t)p->peek.len)
                                       : p->lex->cur;
    /* Trim trailing whitespace so the diagnostic text is clean. */
    while (src_end > src_start && (src_end[-1] == ' ' || src_end[-1] == '\t'
                                    || src_end[-1] == '\r' || src_end[-1] == '\n')) {
        src_end--;
    }

    UToken rp = peek(p);
    if (rp.type != TOK_RPAREN) {
        return make_error(p, PARSE_EXPECTED_RPAREN,
                          kErrorMessages[PARSE_EXPECTED_RPAREN],
                          rp.line, rp.col);
    }
    consume(p);  /* consume ')' */

    UAstNode *node = make_node(p, AST_ASSERT, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.assert_stmt.expr     = expr;
    node->u.assert_stmt.src_text = src_start;
    node->u.assert_stmt.src_len  = (int)(src_end - src_start);
    return node;
}

/* --- parse_try: `try { body } [catch ([var] e [if guard]) { handler }] [else { body }] [finally { cleanup }]`
 *
 * Wave 6 W5 (v0.10.5): extended grammar to accept:
 *   - optional `var` keyword before the catch variable name
 *   - optional `if expr` guard after the catch variable name
 *   - optional `else { body }` clause after catch (runs when no exception thrown)
 *
 * Both catch and finally remain optional, but at least one must be present.
 * `else` requires a preceding catch clause. */

UAstNode *parse_try(UParser *p) {
    UToken kw = consume(p);  /* consume TOK_KW_TRY */

    UAstNode *body = parse_block(p);
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    const char *catch_var_start = NULL;
    int         catch_var_len   = 0;
    UAstNode   *catch_body      = NULL;
    UAstNode   *catch_guard     = NULL;
    UAstNode   *else_body       = NULL;
    UAstNode   *finally_body    = NULL;

    /* Optional catch clause. */
    if (peek(p).type == TOK_KW_CATCH) {
        consume(p);  /* consume 'catch' */

        UToken lp = peek(p);
        if (lp.type != TOK_LPAREN) {
            return make_error(p, PARSE_EXPECTED_LPAREN,
                              kErrorMessages[PARSE_EXPECTED_LPAREN],
                              lp.line, lp.col);
        }
        consume(p);

        /* Accept optional `var` keyword before the catch variable name. */
        if (peek(p).type == TOK_KW_VAR) {
            consume(p);  /* consume 'var' — treated as sugar, no semantic change */
        }

        UToken var_tok = peek(p);
        if (var_tok.type != TOK_IDENT) {
            return make_error(p, PARSE_EXPECTED_IDENT,
                              kErrorMessages[PARSE_EXPECTED_IDENT],
                              var_tok.line, var_tok.col);
        }
        consume(p);
        catch_var_start = var_tok.u.str.start;
        catch_var_len   = var_tok.u.str.len;

        /* Accept optional `if expr` guard. */
        if (peek(p).type == TOK_KW_IF) {
            consume(p);  /* consume 'if' */
            catch_guard = parse_expression(p, 0);
            if (!catch_guard) return (UAstNode *)&uparser_oom_sentinel;
            if (catch_guard->kind == AST_ERROR) return catch_guard;
        }

        UToken rp = peek(p);
        if (rp.type != TOK_RPAREN) {
            return make_error(p, PARSE_EXPECTED_RPAREN,
                              kErrorMessages[PARSE_EXPECTED_RPAREN],
                              rp.line, rp.col);
        }
        consume(p);

        catch_body = parse_block(p);
        if (!catch_body) return (UAstNode *)&uparser_oom_sentinel;
        if (catch_body->kind == AST_ERROR) return catch_body;

        /* Accept optional `else { body }` clause after catch. */
        if (peek(p).type == TOK_KW_ELSE) {
            consume(p);  /* consume 'else' */
            else_body = parse_block(p);
            if (!else_body) return (UAstNode *)&uparser_oom_sentinel;
            if (else_body->kind == AST_ERROR) return else_body;
        }
    }

    /* Optional finally clause. */
    if (peek(p).type == TOK_KW_FINALLY) {
        consume(p);  /* consume 'finally' */

        finally_body = parse_block(p);
        if (!finally_body) return (UAstNode *)&uparser_oom_sentinel;
        if (finally_body->kind == AST_ERROR) return finally_body;
    }

    /* Require at least one of catch or finally. */
    if (catch_body == NULL && finally_body == NULL) {
        return make_error(p, PARSE_TRY_NEEDS_CATCH_OR_FINALLY,
                          kErrorMessages[PARSE_TRY_NEEDS_CATCH_OR_FINALLY],
                          kw.line, kw.col);
    }

    UAstNode *node = make_node(p, AST_TRY, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.try_stmt.body            = body;
    node->u.try_stmt.catch_var_start = catch_var_start;
    node->u.try_stmt.catch_var_len   = catch_var_len;
    node->u.try_stmt.catch_body      = catch_body;
    node->u.try_stmt.catch_guard     = catch_guard;
    node->u.try_stmt.else_body       = else_body;
    node->u.try_stmt.finally_body    = finally_body;
    return node;
}

/* === W1/v0.10.5: control flow ===
 *
 * parse_for — `for (var x : iter_expr) body` or `for (var x in iter_expr) body`
 *
 * Ruling: implemented (Wave 6 W1, legacy F2).
 *
 * Supports only the for-each form.  C-style `for (init; cond; step)` is a
 * migration (see docs/migration/control-flow-migration.md).  Count-form
 * `for (N) body` and flavoured `for|`/`for&` are deferred-v1.x.
 *
 * Produces AST_FOR_EACH with:
 *   var_name_start / var_name_len — the loop variable name
 *   iter_expr                     — the iterable (evaluated once)
 *   body                          — AST_BLOCK (loop body)
 *
 * The emitter lowers AST_FOR_EACH to a while-loop index pattern:
 *   var _iter = iter_expr;
 *   var _n    = _iter.length();
 *   var _i    = 0;
 *   while (_i < _n) { var x = _iter.get(_i); body; _i = _i + 1 }
 *
 * break/continue work inside AST_FOR_EACH bodies because the emitter
 * tracks the break/continue patch-lists in the loop context. */
UAstNode *parse_for(UParser *p) {
    UToken kw = consume(p);  /* consume TOK_KW_FOR */

    UToken lp = peek(p);
    if (lp.type != TOK_LPAREN) {
        return make_error(p, PARSE_EXPECTED_LPAREN,
                          kErrorMessages[PARSE_EXPECTED_LPAREN],
                          lp.line, lp.col);
    }
    consume(p);

    /* Require `var` before the loop variable. */
    UToken var_tok = peek(p);
    if (var_tok.type != TOK_KW_VAR) {
        return make_error(p, PARSE_FOR_EXPECTED_VAR,
                          kErrorMessages[PARSE_FOR_EXPECTED_VAR],
                          var_tok.line, var_tok.col);
    }
    consume(p);

    UToken name_tok = peek(p);
    if (name_tok.type != TOK_IDENT) {
        return make_error(p, PARSE_EXPECTED_IDENT,
                          kErrorMessages[PARSE_EXPECTED_IDENT],
                          name_tok.line, name_tok.col);
    }
    consume(p);

    /* Separator: `:` or `in`. */
    UToken sep_tok = peek(p);
    const int sep_is_colon = (sep_tok.type == TOK_COLON);
    const int sep_is_in    = (sep_tok.type == TOK_IDENT &&
                              ident_equals(sep_tok.u.str.start, sep_tok.u.str.len, "in", 2));
    if (!sep_is_colon && !sep_is_in) {
        return make_error(p, PARSE_FOR_EXPECTED_COLON_OR_IN,
                          kErrorMessages[PARSE_FOR_EXPECTED_COLON_OR_IN],
                          sep_tok.line, sep_tok.col);
    }
    consume(p);

    /* Iterable expression (allows commas inside parens — parse_inner_tier). */
    UAstNode *iter = parse_inner_tier(p);
    if (!iter) return (UAstNode *)&uparser_oom_sentinel;
    if (iter->kind == AST_ERROR) return iter;

    UToken rp = peek(p);
    if (rp.type != TOK_RPAREN) {
        return make_error(p, PARSE_EXPECTED_RPAREN,
                          kErrorMessages[PARSE_EXPECTED_RPAREN],
                          rp.line, rp.col);
    }
    consume(p);

    /* Body block.  Bump loop_depth so break/continue are valid inside. */
    p->loop_depth++;
    UAstNode *body = parse_block(p);
    p->loop_depth--;
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    UAstNode *node = make_node(p, AST_FOR_EACH, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.for_each.var_name_start = name_tok.u.str.start;
    node->u.for_each.var_name_len   = name_tok.u.str.len;
    node->u.for_each.iter_expr      = iter;
    node->u.for_each.body           = body;
    return node;
}

/* parse_break — `break` (statement).
 * Ruling: implemented (Wave 6 W1, legacy F2).
 * No payload beyond position.  Error if not inside a for/while. */
UAstNode *parse_break(UParser *p) {
    UToken kw = consume(p);  /* consume TOK_KW_BREAK */
    if (p->loop_depth == 0) {
        return make_error(p, PARSE_BREAK_OUTSIDE_LOOP,
                          kErrorMessages[PARSE_BREAK_OUTSIDE_LOOP],
                          kw.line, kw.col);
    }
    UAstNode *node = make_node(p, AST_BREAK, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    return node;
}

/* parse_continue — `continue` (statement).
 * Ruling: implemented (Wave 6 W1, legacy F2).
 * No payload beyond position.  Error if not inside a for/while. */
UAstNode *parse_continue(UParser *p) {
    UToken kw = consume(p);  /* consume TOK_KW_CONTINUE */
    if (p->loop_depth == 0) {
        return make_error(p, PARSE_CONTINUE_OUTSIDE_LOOP,
                          kErrorMessages[PARSE_CONTINUE_OUTSIDE_LOOP],
                          kw.line, kw.col);
    }
    UAstNode *node = make_node(p, AST_CONTINUE, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    return node;
}

/* parse_switch — `switch (expr) { case v1: body1; case v2: body2; }`
 *
 * Ruling: implemented (Wave 6 W1, legacy F2).
 * Equality-based only (no pattern matching — that is deferred-v1.x).
 * Produces AST_SWITCH with parallel arrays of case-value nodes and body nodes.
 *
 * Grammar:
 *   switch ( expr ) { ( case expr : stmts )* }
 *
 * The emitter lowers to a chain of if-else comparisons.
 * Break inside a case body exits the switch (switch is a loop-context
 * in the emitter's patch-list sense). */
UAstNode *parse_switch(UParser *p) {
    UToken kw = consume(p);  /* consume TOK_KW_SWITCH */

    UToken lp = peek(p);
    if (lp.type != TOK_LPAREN) {
        return make_error(p, PARSE_EXPECTED_LPAREN,
                          kErrorMessages[PARSE_EXPECTED_LPAREN],
                          lp.line, lp.col);
    }
    consume(p);

    UAstNode *expr = parse_inner_tier(p);
    if (!expr) return (UAstNode *)&uparser_oom_sentinel;
    if (expr->kind == AST_ERROR) return expr;

    UToken rp = peek(p);
    if (rp.type != TOK_RPAREN) {
        return make_error(p, PARSE_EXPECTED_RPAREN,
                          kErrorMessages[PARSE_EXPECTED_RPAREN],
                          rp.line, rp.col);
    }
    consume(p);

    UToken lb = peek(p);
    if (lb.type != TOK_LBRACE) {
        return make_error(p, PARSE_EXPECTED_LBRACE,
                          kErrorMessages[PARSE_EXPECTED_LBRACE],
                          lb.line, lb.col);
    }
    consume(p);

    /* Parse case list. */
    int cap = 4;
    UAstNode **case_vals   = (UAstNode **)uarena_alloc(p->arena,
                                                        (size_t)cap * sizeof(UAstNode *));
    UAstNode **case_bodies = (UAstNode **)uarena_alloc(p->arena,
                                                        (size_t)cap * sizeof(UAstNode *));
    if (!case_vals || !case_bodies) return (UAstNode *)&uparser_oom_sentinel;
    int case_count = 0;

    /* switch body: break inside a case must exit the switch */
    p->loop_depth++;

    while (peek(p).type != TOK_RBRACE && peek(p).type != TOK_EOF) {
        /* Skip statement separators between cases. */
        while (peek(p).type == TOK_SEMI || peek(p).type == TOK_PIPE) {
            consume(p);
        }
        if (peek(p).type == TOK_RBRACE || peek(p).type == TOK_EOF) break;

        UToken case_tok = peek(p);
        if (case_tok.type != TOK_KW_CASE) {
            p->loop_depth--;
            return make_error(p, PARSE_SWITCH_EXPECTED_CASE,
                              kErrorMessages[PARSE_SWITCH_EXPECTED_CASE],
                              case_tok.line, case_tok.col);
        }
        consume(p);  /* consume 'case' */

        /* Case value expression (not a separator tier — parse as atom/prefix). */
        UAstNode *val = parse_expression(p, 0);
        if (!val) { p->loop_depth--; return (UAstNode *)&uparser_oom_sentinel; }
        if (val->kind == AST_ERROR) { p->loop_depth--; return val; }

        UToken colon = peek(p);
        if (colon.type != TOK_COLON) {
            p->loop_depth--;
            return make_error(p, PARSE_SWITCH_EXPECTED_COLON,
                              kErrorMessages[PARSE_SWITCH_EXPECTED_COLON],
                              colon.line, colon.col);
        }
        consume(p);  /* consume ':' */

        /* Collect statements until the next `case`, `}`, or EOF.
         * Build them into an implicit AST_BLOCK. */
        int stmt_cap = 4;
        UAstNode **stmts = (UAstNode **)uarena_alloc(p->arena,
                                                       (size_t)stmt_cap * sizeof(UAstNode *));
        if (!stmts) { p->loop_depth--; return (UAstNode *)&uparser_oom_sentinel; }
        int stmt_count = 0;

        while (peek(p).type != TOK_KW_CASE &&
               peek(p).type != TOK_RBRACE &&
               peek(p).type != TOK_EOF) {
            /* Use parse_statement_or_expr (not parse_outer_tier) so that ';'
             * between case bodies is not greedily consumed across 'case'
             * boundaries — parse_outer_tier would eat the ';' and then try to
             * parse 'case' as an expression, yielding "expected expression". */
            UAstNode *s = parse_statement_or_expr(p);
            if (!s) { p->loop_depth--; return (UAstNode *)&uparser_oom_sentinel; }
            if (s->kind == AST_ERROR) { p->loop_depth--; return s; }

            if (stmt_count == stmt_cap) {
                if (!arena_grow_node_array(p, &stmts, &stmt_cap, stmt_count)) {
                    p->loop_depth--;
                    return (UAstNode *)&uparser_oom_sentinel;
                }
            }
            stmts[stmt_count++] = s;

            /* Consume separator if present. */
            if (peek(p).type == TOK_SEMI || peek(p).type == TOK_PIPE) {
                consume(p);
            }
        }

        UAstNode *body = make_node(p, AST_BLOCK, case_tok.line, case_tok.col);
        if (!body) { p->loop_depth--; return (UAstNode *)&uparser_oom_sentinel; }
        body->u.block.stmts = stmts;
        body->u.block.count = stmt_count;

        /* Grow parallel arrays if needed. */
        if (case_count == cap) {
            if (!arena_grow_node_array(p, &case_vals,   &cap, case_count) ||
                !arena_grow_node_array(p, &case_bodies, &cap, case_count)) {
                p->loop_depth--;
                return (UAstNode *)&uparser_oom_sentinel;
            }
        }
        case_vals[case_count]   = val;
        case_bodies[case_count] = body;
        case_count++;
    }

    p->loop_depth--;

    UToken rb = peek(p);
    if (rb.type != TOK_RBRACE) {
        return make_error(p, PARSE_EXPECTED_RBRACE,
                          kErrorMessages[PARSE_EXPECTED_RBRACE],
                          rb.line, rb.col);
    }
    consume(p);

    UAstNode *node = make_node(p, AST_SWITCH, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.switch_stmt.expr        = expr;
    node->u.switch_stmt.case_vals   = case_vals;
    node->u.switch_stmt.case_bodies = case_bodies;
    node->u.switch_stmt.case_count  = case_count;
    return node;
}
/* === end W1/v0.10.5: control flow === */
