/* SPDX-License-Identifier: BSD-3-Clause */
/* uparse_expr.c — Pratt expression parser (infix tables, prefix, atom, call,
 * member-access, parse_expression).
 * Extracted from uparse.c during v0.5.4-decompose (PARSE-021 #6). */

#include "parse/uparse.h"
#include "parse/uparse_internal.h"

/* Return the left-binding precedence of an infix token, or 0 if not
   an infix operator (terminates the Pratt climb).
   Comparison operators bind looser than arithmetic:
     3 = equality (==, !=)
     4 = relational (<, <=, >, >=)
     5 = additive (+, -)
     6 = multiplicative (*, /) */
int infix_prec(UTokenType t) {
    switch (t) {
    case TOK_EQEQ:
    case TOK_NEQ:   return 3;
    case TOK_LT:
    case TOK_LE:
    case TOK_GT:
    case TOK_GE:    return 4;
    case TOK_PLUS:
    case TOK_MINUS: return 5;
    case TOK_STAR:
    case TOK_SLASH: return 6;
    default:        return 0;
    }
}

UAstBinaryOp infix_binop(UTokenType t) {
    switch (t) {
    case TOK_PLUS:  return BOP_ADD;
    case TOK_MINUS: return BOP_SUB;
    case TOK_STAR:  return BOP_MUL;
    case TOK_SLASH: return BOP_DIV;
    default:        return BOP_ADD; /* unreachable when prec > 0 */
    }
}

/* True when t is a comparison operator token. */
bool is_compare_token(UTokenType t) {
    return t == TOK_EQEQ || t == TOK_NEQ
        || t == TOK_LT   || t == TOK_LE
        || t == TOK_GT   || t == TOK_GE;
}

UAstCompareOp compare_op(UTokenType t) {
    switch (t) {
    case TOK_EQEQ: return CMP_EQ;
    case TOK_NEQ:  return CMP_NEQ;
    case TOK_LT:   return CMP_LT;
    case TOK_LE:   return CMP_LE;
    case TOK_GT:   return CMP_GT;
    case TOK_GE:   return CMP_GE;
    default:       return CMP_EQ; /* unreachable */
    }
}

UAstNode *make_compare(UParser *p, UAstCompareOp op,
                       UAstNode *lhs, UAstNode *rhs,
                       int line, int col) {
    UAstNode *n = make_node(p, AST_COMPARE, line, col);
    if (!n) return NULL;
    n->u.cmp.op  = op;
    n->u.cmp.lhs = lhs;
    n->u.cmp.rhs = rhs;
    return n;
}

UAstNode *make_bool_node(UParser *p, bool value, int line, int col) {
    UAstNode *n = make_node(p, AST_BOOL, line, col);
    if (!n) return NULL;
    n->u.b = value;
    return n;
}

UAstNode *make_nil_node(UParser *p, int line, int col) {
    return make_node(p, AST_NIL, line, col);
}

/* --- parse_prefix: unary +/- /! then atom.  Unary '+' is a no-op. --- */

UAstNode *parse_prefix(UParser *p) {
    UToken t = peek(p);
    if (t.type == TOK_PLUS) {
        consume(p);
        return parse_prefix(p);             /* +x is x; no node */
    }
    if (t.type == TOK_MINUS) {
        consume(p);
        UAstNode *operand = parse_prefix(p); /* right-assoc: --3 -> -(-3) */
        if (!operand) return NULL;
        if (operand->kind == AST_ERROR) return operand;
        return make_unary(p, UOP_NEG, operand, t.line, t.col);
    }
    if (t.type == TOK_BANG) {
        /* Prefix `!x` — logical NOT.  Recognized here (primary position) so
         * postfix `e!` (in the post-primary loop) does not steal it. */
        consume(p);
        UAstNode *operand = parse_prefix(p);
        if (!operand) return NULL;
        if (operand->kind == AST_ERROR) return operand;
        return make_unary(p, UOP_NOT, operand, t.line, t.col);
    }
    return parse_atom(p);
}

/* --- parse_atom: INT | IDENT | true | false | nil | ( expr ) | error. --- */

UAstNode *parse_atom(UParser *p) {
    UToken t = peek(p);
    switch (t.type) {
    case TOK_INT:
        consume(p);
        return make_int(p, t.u.i, t.line, t.col);
    case TOK_IDENT:
        consume(p);
        return make_ident(p, t.u.str.start, t.u.str.len, t.line, t.col);
    case TOK_KW_TRUE:
        consume(p);
        return make_bool_node(p, true, t.line, t.col);
    case TOK_KW_FALSE:
        consume(p);
        return make_bool_node(p, false, t.line, t.col);
    case TOK_KW_NIL:
        consume(p);
        return make_nil_node(p, t.line, t.col);
    case TOK_LPAREN: {
        consume(p);
        UAstNode *inner = parse_expression(p, 0);
        if (!inner) return NULL;
        if (inner->kind == AST_ERROR) return inner;
        UToken r = peek(p);
        if (r.type != TOK_RPAREN) {
            return make_error(p, PARSE_EXPECTED_RPAREN,
                              kErrorMessages[PARSE_EXPECTED_RPAREN],
                              r.line, r.col);
        }
        consume(p);
        return inner;
    }
    case TOK_KW_FUNCTION:
        return parse_function(p);
    case TOK_KW_TRY:
        return parse_try(p);
    case TOK_KW_THROW:
        return parse_throw(p);
    case TOK_KW_CLOSURE:
        consume(p);
        return make_error(p, PARSE_CLOSURE_KEYWORD,
                          "the 'closure' keyword is retired at v1.0; use 'function' instead. "
                          "MIGRATION TRAP: 'closure' bound 'this' lexically; 'function' binds at call site. "
                          "See REVIVAL §14 L14 for 'lobby.receive' rebind pattern",
                          t.line, t.col);
    case TOK_EOF:
        return make_error(p, PARSE_UNEXPECTED_EOF,
                          kErrorMessages[PARSE_UNEXPECTED_EOF],
                          t.line, t.col);
    case TOK_ERROR:
        /* Note: do NOT consume — the statement-level recovery loop owns lexer advance. */
        return make_error(p, PARSE_LEX_ERROR,
                          t.u.err.message ? t.u.err.message
                                          : kErrorMessages[PARSE_LEX_ERROR],
                          t.line, t.col);
    default:
        return make_error(p, PARSE_EXPECTED_EXPRESSION,
                          kErrorMessages[PARSE_EXPECTED_EXPRESSION],
                          t.line, t.col);
    }
}

/* --- arena_grow_node_array: double a UAstNode* arena array when full.
   Called when count == cap.  Allocates a new block of cap*2 entries from the
   arena, copies the existing entries, and updates *arr and *cap.
   Returns true on success, false on arena OOM (caller should return the
   uparser_oom_sentinel). */
bool arena_grow_node_array(UParser *p, UAstNode ***arr, int *cap, int count) {
    int new_cap = (*cap) * 2;
    UAstNode **bigger = (UAstNode **)uarena_alloc(p->arena,
                                                   (size_t)new_cap * sizeof(UAstNode *));
    if (!bigger) return false;
    for (int i = 0; i < count; i++) bigger[i] = (*arr)[i];
    *arr = bigger;
    *cap = new_cap;
    return true;
}

/* --- parse_call_args: parse `(` arg, arg, ... `)` after a callee expression.
   Returns an AST_CALL node. callee is already parsed. --- */

UAstNode *parse_call_args(UParser *p, UAstNode *callee) {
    UToken lparen = consume(p);  /* consume '(' */

    int cap = 4;
    UAstNode **args = (UAstNode **)uarena_alloc(p->arena,
                                                (size_t)cap * sizeof(UAstNode *));
    if (!args) return (UAstNode *)&uparser_oom_sentinel;
    int count = 0;

    while (peek(p).type != TOK_RPAREN && peek(p).type != TOK_EOF) {
        UAstNode *arg = parse_inner_tier(p);
        if (!arg) return (UAstNode *)&uparser_oom_sentinel;
        if (arg->kind == AST_ERROR) return arg;

        if (count == cap) {
            if (!arena_grow_node_array(p, &args, &cap, count))
                return (UAstNode *)&uparser_oom_sentinel;
        }
        args[count++] = arg;

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

    UAstNode *node = make_node(p, AST_CALL, lparen.line, lparen.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.call.callee    = callee;
    node->u.call.args      = args;
    node->u.call.arg_count = count;
    return node;
}

/* --- parse_member_access: handle a single `.IDENT` or `->IDENT` postfix.
   Caller has confirmed peek() is TOK_DOT or TOK_ARROW; this function
   consumes the operator, the IDENT, and (optionally) `= rhs`.

   Shape produced:
     obj.x        → AST_MEMBER_GET
     obj.x = v    → AST_MEMBER_SET   (consumes `= v`)
     obj.x->y     → AST_PROP_GET
     obj.x->y = v → AST_PROP_SET     (consumes `= v`)

   The caller's postfix loop should `break` after a SET arm (assignment
   terminates further chaining) and `continue` after a GET arm so that
   `obj.x.y` and `obj.x()` keep building on the result.

   *out_is_assign is set to true when the SET form was produced.

   Returns the new node, or an AST_ERROR / OOM sentinel on failure. --- */

UAstNode *parse_member_access(UParser *p, UAstNode *recv,
                                     bool *out_is_assign) {
    UToken op = consume(p);  /* TOK_DOT or TOK_ARROW */
    *out_is_assign = false;

    UToken name = peek(p);
    if (name.type != TOK_IDENT) {
        return make_error(p, PARSE_EXPECTED_IDENT,
                          kErrorMessages[PARSE_EXPECTED_IDENT],
                          name.line, name.col);
    }
    consume(p);

    const bool is_arrow = (op.type == TOK_ARROW);

    if (peek(p).type == TOK_EQ) {
        consume(p);  /* consume '=' */
        UAstNode *value = parse_inner_tier(p);
        if (!value) return NULL;
        if (value->kind == AST_ERROR) return value;
        UAstNode *node = make_node(p, is_arrow ? AST_PROP_SET : AST_MEMBER_SET,
                                   op.line, op.col);
        if (!node) return NULL;
        if (is_arrow) {
            node->u.prop.recv            = recv;
            node->u.prop.prop_name_start = name.u.str.start;
            node->u.prop.prop_name_len   = name.u.str.len;
            node->u.prop.value           = value;
        } else {
            node->u.member.recv       = recv;
            node->u.member.name_start = name.u.str.start;
            node->u.member.name_len   = name.u.str.len;
            node->u.member.value      = value;
        }
        *out_is_assign = true;
        return node;
    }

    UAstNode *node = make_node(p, is_arrow ? AST_PROP_GET : AST_MEMBER_GET,
                               op.line, op.col);
    if (!node) return NULL;
    if (is_arrow) {
        node->u.prop.recv            = recv;
        node->u.prop.prop_name_start = name.u.str.start;
        node->u.prop.prop_name_len   = name.u.str.len;
        node->u.prop.value           = NULL;
    } else {
        node->u.member.recv       = recv;
        node->u.member.name_start = name.u.str.start;
        node->u.member.name_len   = name.u.str.len;
        node->u.member.value      = NULL;
    }
    return node;
}

/* --- parse_expression: Pratt precedence climbing over parse_prefix. --- */

UAstNode *parse_expression(UParser *p, int min_prec) {
    UAstNode *left = parse_prefix(p);
    if (!left) return NULL;
    if (left->kind == AST_ERROR) return left;

    for (;;) {
        UToken op = peek(p);

        /* Postfix call: `expr(args)` — highest precedence (7). */
        if (op.type == TOK_LPAREN && min_prec <= 7) {
            left = parse_call_args(p, left);
            if (!left) return NULL;
            if (left->kind == AST_ERROR) return left;
            continue;
        }

        /* Postfix member-access: `expr.IDENT [= rhs]` and `expr->IDENT [= rhs]`.
           Same precedence tier as the postfix call.  After a SET form, stop
           climbing — assignment terminates the postfix chain.  After a GET,
           keep looping so chains like `a.b.c` and `a.b()` keep building. */
        if ((op.type == TOK_DOT || op.type == TOK_ARROW) && min_prec <= 7) {
            bool is_assign = false;
            left = parse_member_access(p, left, &is_assign);
            if (!left) return NULL;
            if (left->kind == AST_ERROR) return left;
            if (is_assign) break;
            /* Spec #4 §4.4–§4.6: bare/emit `.changed` outside at(...). */
            if (!p->at_event_cond
                && left->kind == AST_MEMBER_GET
                && ident_equals(left->u.member.name_start,
                                left->u.member.name_len,
                                "changed", 7)) {
                UToken nxt = peek(p);
                if (nxt.type == TOK_BANG) {
                    return make_error(p, PARSE_SLOT_CHANGED_EMIT_V1,
                                      kErrorMessages[PARSE_SLOT_CHANGED_EMIT_V1],
                                      nxt.line, nxt.col);
                }
                return make_error(p, PARSE_SLOT_CHANGED_BARE_V1,
                                  kErrorMessages[PARSE_SLOT_CHANGED_BARE_V1],
                                  left->line, left->col);
            }
            continue;
        }

        /* Postfix `e!` — desugar to `e.emit([arg])`.
           `e!`        → AST_CALL { callee=left, method="emit", args=[] }
           `e!(p)`     → AST_CALL { callee=left, method="emit", args=[p] }
           `e!(x,y,z)` → PARSE_EMIT_MULTI_ARG_V1 error */
        if (op.type == TOK_BANG && min_prec <= 7) {
            consume(p);  /* consume '!' */
            left = desugar_postfix_emit(p, left, op);
            if (!left) return NULL;
            if (left->kind == AST_ERROR) return left;
            continue;
        }

        /* Postfix `?` — only valid inside at(...) condition.
         * When at_event_cond is set, pass through (parse_at will consume it).
         * Otherwise it is an error. */
        if (op.type == TOK_QUESTION && min_prec <= 7) {
            if (p->at_event_cond) break;  /* let parse_at consume it */
            consume(p);
            return make_error(p, PARSE_QUESTION_OUTSIDE_AT,
                              kErrorMessages[PARSE_QUESTION_OUTSIDE_AT],
                              op.line, op.col);
        }

        int prec = infix_prec(op.type);
        if (prec < min_prec || prec == 0) break;

        consume(p);
        /* Comparison operators are left-associative; use prec+1 for right
           so that `a == b == c` is rejected as ambiguous (each side parses
           as atoms, and chained comparisons are a parse error in urbiscript). */
        UAstNode *right = parse_expression(p, prec + 1);
        if (!right) return NULL;
        if (right->kind == AST_ERROR) return right;

        if (is_compare_token(op.type)) {
            left = make_compare(p, compare_op(op.type), left, right,
                                op.line, op.col);
        } else {
            left = make_binary(p, infix_binop(op.type), left, right,
                               op.line, op.col);
        }
        if (!left) return NULL;
    }
    return left;
}
