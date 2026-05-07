/* SPDX-License-Identifier: BSD-3-Clause */
/* uparse_stmt.c — statement-level parse functions.
 * Extracted from uparse.c during v0.5.4-decompose (PARSE-021 #4). */

#include "parse/uparse.h"
#include "parse/uparse_internal.h"

/* --- parse_var_decl: `var x = expr` --- */

UAstNode *parse_var_decl(UParser *p) {
    UToken kw = consume(p);          /* consume TOK_KW_VAR */
    UToken name = peek(p);

    /* Detect hard reserved keywords used as variable names (T4, spec #2 §3.11).
     * TOK_KW_ASYNC is soft — allowed as identifier at v1.0. */
    if (name.type == TOK_KW_AT       || name.type == TOK_KW_WHENEVER  ||
        name.type == TOK_KW_WAITUNTIL || name.type == TOK_KW_ONLEAVE  ||
        name.type == TOK_KW_SYNC) {
        return make_error(p, PARSE_RESERVED_KEYWORD_AS_IDENT,
                          kErrorMessages[PARSE_RESERVED_KEYWORD_AS_IDENT],
                          name.line, name.col);
    }

    /* TOK_KW_ASYNC is a soft keyword — accepted as an identifier at v1.0.
     * The lexer always populates u.str for keyword tokens, so u.str.start
     * and u.str.len are valid even when type == TOK_KW_ASYNC. */
    if (name.type != TOK_IDENT && name.type != TOK_KW_ASYNC) {
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

    UAstNode *init = parse_inner_tier(p);
    if (!init) return NULL;
    if (init->kind == AST_ERROR) return init;

    UAstNode *node = make_node(p, AST_VAR_DECL, kw.line, kw.col);
    if (!node) return NULL;
    node->u.var_decl.name_start = name.u.str.start;
    node->u.var_decl.name_len   = name.u.str.len;
    node->u.var_decl.init       = init;
    return node;
}

/* --- parse_assign: `x = expr` — IDENT already consumed as `name`. --- */

UAstNode *parse_assign_from_ident(UParser *p, UToken name) {
    /* TOK_EQ already peeked/confirmed by caller; consume it. */
    consume(p);

    UAstNode *value = parse_inner_tier(p);
    if (!value) return NULL;
    if (value->kind == AST_ERROR) return value;

    UAstNode *node = make_node(p, AST_ASSIGN, name.line, name.col);
    if (!node) return NULL;
    node->u.assign.name_start = name.u.str.start;
    node->u.assign.name_len   = name.u.str.len;
    node->u.assign.value      = value;
    return node;
}

/* --- parse_statement_or_expr: var-decl, assign, or inner-tier expression.
   Returns an inner-tier result (arithmetic expression, possibly with
   | / & separators). Used as the child-entry point for both
   uparse_next_statement and the outer-tier loop. --- */

UAstNode *parse_statement_or_expr(UParser *p) {
    UToken t = peek(p);

    /* while (cond) { body } */
    if (t.type == TOK_KW_WHILE) {
        return parse_while(p);
    }

    /* if (cond) { ... } [else { ... }] */
    if (t.type == TOK_KW_IF) {
        return parse_if(p);
    }

    /* var x = expr */
    if (t.type == TOK_KW_VAR) {
        return parse_var_decl(p);
    }

    /* return [expr] */
    if (t.type == TOK_KW_RETURN) {
        return parse_return(p);
    }

    /* try { ... } [catch (e) { ... }] [finally { ... }] */
    if (t.type == TOK_KW_TRY) {
        return parse_try(p);
    }

    /* throw expr */
    if (t.type == TOK_KW_THROW) {
        return parse_throw(p);
    }

    /* at (cond) body [onleave handler]
     * at sync (cond) body
     * at async (cond) body */
    if (t.type == TOK_KW_AT) {
        return parse_at(p);
    }

    /* whenever (cond) body [onleave handler] */
    if (t.type == TOK_KW_WHENEVER) {
        return parse_whenever(p);
    }

    /* waituntil (cond) */
    if (t.type == TOK_KW_WAITUNTIL) {
        return parse_waituntil(p);
    }

    /* x = expr — detect by consuming IDENT then peeking for TOK_EQ.
       mytag: { body } — detect by consuming IDENT then peeking for TOK_COLON.
       If neither, put the ident back as the LHS and continue with
       the normal inner-tier path (Pratt climb + pipe/amp loop). */
    if (t.type == TOK_IDENT) {
        UToken name = consume(p);
        if (peek(p).type == TOK_EQ) {
            return parse_assign_from_ident(p, name);
        }
        /* Tag-prefix: `mytag: { body }`.  At statement level, `:` has no
         * other meaning (not an infix operator, not a separator), so seeing
         * IDENT followed by COLON unambiguously introduces a tag scope. */
        if (peek(p).type == TOK_COLON) {
            return parse_tag_prefix(p, name);
        }
        /* Not assignment: build the ident node and finish the Pratt climb
           for the arithmetic expression (including postfix calls), then
           hand to the inner-tier pipe/amp separator loop. */
        UAstNode *lhs = make_ident(p, name.u.str.start, name.u.str.len,
                                   name.line, name.col);
        if (!lhs) return NULL;
        /* Pratt climb: handle postfix `(args)`, member access, and
           arithmetic operators.  Mirrors parse_expression's postfix tier. */
        for (;;) {
            UToken op = peek(p);
            /* Postfix call: highest precedence. */
            if (op.type == TOK_LPAREN) {
                lhs = parse_call_args(p, lhs);
                if (!lhs) return NULL;
                if (lhs->kind == AST_ERROR) return lhs;
                continue;
            }
            /* Postfix member-access: `.IDENT [= rhs]` / `->IDENT [= rhs]`.
               On SET, parse_member_access already consumed `= value` via
               parse_inner_tier (which itself greedily absorbs `|`/`&`),
               so return immediately rather than re-entering the outer
               separator loop. */
            if (op.type == TOK_DOT || op.type == TOK_ARROW) {
                bool is_assign = false;
                lhs = parse_member_access(p, lhs, &is_assign);
                if (!lhs) return NULL;
                if (lhs->kind == AST_ERROR) return lhs;
                if (is_assign) return lhs;
                /* Spec #4 §4.4–§4.6: bare/emit `.changed` outside at(...).
                 * Flag as an error so users are guided to the correct form. */
                if (!p->at_event_cond
                    && lhs->kind == AST_MEMBER_GET
                    && ident_equals(lhs->u.member.name_start,
                                    lhs->u.member.name_len,
                                    "changed", 7)) {
                    UToken nxt = peek(p);
                    if (nxt.type == TOK_BANG) {
                        return make_error(p, PARSE_SLOT_CHANGED_EMIT_V1,
                                          kErrorMessages[PARSE_SLOT_CHANGED_EMIT_V1],
                                          nxt.line, nxt.col);
                    }
                    return make_error(p, PARSE_SLOT_CHANGED_BARE_V1,
                                      kErrorMessages[PARSE_SLOT_CHANGED_BARE_V1],
                                      lhs->line, lhs->col);
                }
                continue;
            }
            /* Postfix `?` — only valid inside at(...) condition context. */
            if (op.type == TOK_QUESTION) {
                if (p->at_event_cond) break;  /* let parse_at consume it */
                consume(p);
                return make_error(p, PARSE_QUESTION_OUTSIDE_AT,
                                  kErrorMessages[PARSE_QUESTION_OUTSIDE_AT],
                                  op.line, op.col);
            }
            /* Postfix `e!` — desugar to `e.emit([arg])`. */
            if (op.type == TOK_BANG) {
                consume(p);
                lhs = desugar_postfix_emit(p, lhs, op);
                if (!lhs) return NULL;
                if (lhs->kind == AST_ERROR) return lhs;
                continue;
            }
            int prec = infix_prec(op.type);
            if (prec == 0) break;
            consume(p);
            UAstNode *rhs = parse_expression(p, prec + 1);
            if (!rhs) return NULL;
            if (rhs->kind == AST_ERROR) return rhs;
            lhs = make_binary(p, infix_binop(op.type), lhs, rhs,
                              op.line, op.col);
            if (!lhs) return NULL;
        }
        /* Inner-tier pipe/amp separator loop (mirrors parse_inner_tier body). */
        for (;;) {
            UToken sep = peek(p);
            if (sep.type != TOK_PIPE && sep.type != TOK_AMP) break;
            consume(p);
            UAstSeparator s = (sep.type == TOK_PIPE) ? SEP_PIPE : SEP_AMP;
            bool trail = at_statement_end(p)
                      || peek(p).type == TOK_SEMI
                      || peek(p).type == TOK_COMMA
                      || peek(p).type == TOK_PIPE;
            if (trail) {
                if (s == SEP_PIPE) return lhs;
                return make_error(p, PARSE_TRAILING_AMP,
                                  kErrorMessages[PARSE_TRAILING_AMP],
                                  sep.line, sep.col);
            }
            UAstNode *rhs = parse_expression(p, 0);
            if (!rhs) return NULL;
            if (rhs->kind == AST_ERROR) return rhs;
            UAstNode *node = make_node(p, AST_BIN_SEP, sep.line, sep.col);
            if (!node) return NULL;
            node->u.bin_sep.separator = s;
            node->u.bin_sep.lhs = lhs;
            node->u.bin_sep.rhs = rhs;
            lhs = node;
        }
        return lhs;
    }

    return parse_inner_tier(p);
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
            int new_cap = cap * 2;
            UAstNode **bigger = (UAstNode **)uarena_alloc(p->arena,
                                                           (size_t)new_cap * sizeof(UAstNode *));
            if (!bigger) return (UAstNode *)&uparser_oom_sentinel;
            for (int i = 0; i < count; i++) bigger[i] = stmts[i];
            stmts = bigger;
            cap = new_cap;
        }
        stmts[count++] = s;

        /* Statements within a block are separated by `;` or `|`.
         * `|` acts as the REPL-boundary convention inside blocks too.
         * If neither is present, the block ends (next token is `}` or
         * an expression starting another statement — stop and expect `}`). */
        UToken sep = peek(p);
        if (sep.type == TOK_SEMI) {
            consume(p);
        } else if (sep.type == TOK_PIPE) {
            consume(p);
        } else {
            break;
        }
        /* Trailing sep just before `}` — continue loop; it will break on `}`. */
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
 * rejects (bare `function {` or `function name {` without parens), otherwise
 * NULL.  As a side effect, when an IDENT follows `function` the name token is
 * consumed and discarded (named-function name handling deferred to T15). */
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
        /* Peek ahead: consume ident, check if next is '{' (bare named form)
         * or '(' (good: named function with parens — T15 wires named funcs).
         * For T14 we only support anonymous `function(...)`. If there's an
         * IDENT followed by LBRACE, reject as bare. If IDENT followed by
         * LPAREN, we just parse as anonymous (name is ignored for now). */
        UToken name_tok = consume(p);
        if (peek(p).type == TOK_LBRACE) {
            return make_error(p, PARSE_BARE_FUNCTION,
                              "bare 'function name { body }' is retired at v1.0; "
                              "use 'function name() { body }' (add empty parens). "
                              "Per REVIVAL §14 L13",
                              name_tok.line, name_tok.col);
        }
        /* IDENT followed by '(' — treat as named function (name stored but
         * not yet used by emit at T14; T15 will wire named-function emit). */
        (void)name_tok;
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
            int new_cap = cap * 2;
            UAstNode **bigger = (UAstNode **)uarena_alloc(p->arena,
                                                           (size_t)new_cap * sizeof(UAstNode *));
            if (!bigger) return (UAstNode *)&uparser_oom_sentinel;
            int i;
            for (i = 0; i < count; i++) bigger[i] = params[i];
            params = bigger;
            cap = new_cap;
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
            value = parse_inner_tier(p);
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

    UAstNode *value = parse_inner_tier(p);
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
