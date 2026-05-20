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
   Handles assignment (`x = expr`), tag-prefix (`mytag: { body }`), and
   expression statements that start with an identifier (call chains,
   member accesses, arithmetic).  The non-assign/non-tag path delegates
   to parse_inner_tier_from_lhs to avoid duplicating the Pratt loop. */
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
    /* Not assignment or tag: build the ident node and hand to the
     * inner-tier entry point that accepts an already-parsed lhs.  This
     * runs parse_expression_cont (Pratt climb) + the pipe/amp fold —
     * identical to parse_inner_tier but without re-consuming the IDENT. */
    UAstNode *lhs = make_ident(p, name.u.str.start, name.u.str.len,
                               name.line, name.col);
    if (!lhs) return NULL;
    return parse_inner_tier_from_lhs(p, lhs);
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

    UAstNode *body = parse_block(p);
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
                          "Per REVIVAL §14 L13: legacy bare functions ambiguously meant "
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
                              "use 'function name() { body }' (add empty parens). "
                              "Per REVIVAL §14 L13",
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

/* --- parse_try: `try { body } [catch (e) { handler }] [finally { cleanup }]`
   Both catch and finally are optional, but at least one must be present. --- */

UAstNode *parse_try(UParser *p) {
    UToken kw = consume(p);  /* consume TOK_KW_TRY */

    UAstNode *body = parse_block(p);
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    const char *catch_var_start = NULL;
    int         catch_var_len   = 0;
    UAstNode   *catch_body      = NULL;
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

        UToken var_tok = peek(p);
        if (var_tok.type != TOK_IDENT) {
            return make_error(p, PARSE_EXPECTED_IDENT,
                              kErrorMessages[PARSE_EXPECTED_IDENT],
                              var_tok.line, var_tok.col);
        }
        consume(p);
        catch_var_start = var_tok.u.str.start;
        catch_var_len   = var_tok.u.str.len;

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
    node->u.try_stmt.finally_body    = finally_body;
    return node;
}
