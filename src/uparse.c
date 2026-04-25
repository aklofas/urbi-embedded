/* SPDX-License-Identifier: BSD-3-Clause */
/* Streaming Pratt parser implementation. */

#include "uparse.h"
#include <stddef.h>

/* --- Static error-message table.  Indices must match UParseError. --- */

static const char * const kErrorMessages[] = {
    "ok",
    "unexpected token",
    "unexpected end of input",
    "expected expression",
    "expected ')'",
    "lex error",                    /* overridden by pass-through lexer message */
    "out of memory during parsing",
    "expected '}'",
    "expected '{'",
    "expected '('",
    "expected identifier",
    "expected '='",
    "expected ';', '|', or end of statement",
    "bare function form 'function name { body }' is retired at v1.0; use 'function name() { body }'",
    "the 'closure' keyword is retired at v1.0; use 'function' instead",
    "trailing '&' is illegal",
    "'lazy' keyword only allowed in parameter lists",
    "lazy parameter cannot have a default value"
};

static const char * const kErrorNames[] = {
    "PARSE_OK",
    "PARSE_UNEXPECTED_TOKEN",
    "PARSE_UNEXPECTED_EOF",
    "PARSE_EXPECTED_EXPRESSION",
    "PARSE_EXPECTED_RPAREN",
    "PARSE_LEX_ERROR",
    "PARSE_OOM",
    "PARSE_EXPECTED_RBRACE",
    "PARSE_EXPECTED_LBRACE",
    "PARSE_EXPECTED_LPAREN",
    "PARSE_EXPECTED_IDENT",
    "PARSE_EXPECTED_EQ",
    "PARSE_EXPECTED_SEMI_OR_PIPE",
    "PARSE_BARE_FUNCTION",
    "PARSE_CLOSURE_KEYWORD",
    "PARSE_TRAILING_AMP",
    "PARSE_LAZY_OUT_OF_PARAM_LIST",
    "PARSE_LAZY_PARAM_DEFAULT"
};

#define N_PARSE_ERROR_CODES ((int)(sizeof kErrorNames / sizeof kErrorNames[0]))

/* --- OOM sentinel.  Returned whenever the arena is in OOM state. --- */

/* Read-only OOM error sentinel returned by parse functions when arena
 * allocation fails. Declared `static const` to satisfy the per-VM
 * audit (see tools/audit-globals.sh + pre-M2 multi-VM-audit spec):
 * functionally immutable, but the public AST API uses `UAstNode *`
 * (non-const), so callers cast away const at return sites. The cast
 * is safe because the sentinel is never mutated by anyone — its
 * contents are inspected only via the const-correct read path
 * (kind == AST_ERROR && u.err.code == PARSE_OOM). */
static const UAstNode uparser_oom_sentinel = {
    AST_ERROR,
    0,
    0,
    { .err = { PARSE_OOM, "out of memory during parsing" } }
};

/* --- ULexer lookahead helpers. --- */

static UToken peek(UParser *p) {
    if (!p->have_peek) {
        p->peek = ulex_next(p->lex);
        p->have_peek = true;
    }
    return p->peek;
}

static UToken consume(UParser *p) {
    UToken t = peek(p);
    p->have_peek = false;
    return t;
}

/* --- AST constructors.  Return NULL on arena OOM. --- */

static UAstNode *make_node(UParser *p, UAstKind k, int line, int col) {
    UAstNode *n = uarena_alloc(p->arena, sizeof *n);
    if (!n) return NULL;
    n->kind = k;
    n->line = line;
    n->col = col;
    return n;
}

static UAstNode *make_int(UParser *p, int64_t v, int line, int col) {
    UAstNode *n = make_node(p, AST_INT, line, col);
    if (!n) return NULL;
    n->u.i = v;
    return n;
}

static UAstNode *make_ident(UParser *p, const char *start, int len, int line, int col) {
    UAstNode *n = make_node(p, AST_IDENT, line, col);
    if (!n) return NULL;
    n->u.ident.start = start;
    n->u.ident.len = len;
    return n;
}

static UAstNode *make_unary(UParser *p, UAstUnaryOp op, UAstNode *operand,
                           int line, int col) {
    UAstNode *n = make_node(p, AST_UNARY, line, col);
    if (!n) return NULL;
    n->u.unary.op = op;
    n->u.unary.operand = operand;
    return n;
}

static UAstNode *make_binary(UParser *p, UAstBinaryOp op, UAstNode *lhs, UAstNode *rhs,
                            int line, int col) {
    UAstNode *n = make_node(p, AST_BINARY, line, col);
    if (!n) return NULL;
    n->u.binary.op = op;
    n->u.binary.lhs = lhs;
    n->u.binary.rhs = rhs;
    return n;
}

static UAstNode *make_error(UParser *p, UParseError code, const char *msg,
                           int line, int col) {
    UAstNode *n = make_node(p, AST_ERROR, line, col);
    if (!n) return NULL;
    n->u.err.code = (int)code;
    n->u.err.message = msg ? msg : kErrorMessages[code];
    return n;
}

/* --- Forward declarations for mutual recursion. --- */

static UAstNode *parse_expression(UParser *p, int min_prec);
static UAstNode *parse_prefix(UParser *p);
static UAstNode *parse_atom(UParser *p);
static UAstNode *parse_call_args(UParser *p, UAstNode *callee);
static UAstNode *parse_inner_tier(UParser *p);
static UAstNode *parse_outer_tier(UParser *p);
static UAstNode *parse_statement_or_expr(UParser *p);
static UAstNode *parse_block(UParser *p);
static UAstNode *parse_if(UParser *p);
static UAstNode *parse_while(UParser *p);
static UAstNode *parse_function(UParser *p);
static UAstNode *parse_return(UParser *p);
static bool at_statement_end(UParser *p);

/* Return the left-binding precedence of an infix token, or 0 if not
   an infix operator (terminates the Pratt climb).
   Comparison operators bind looser than arithmetic:
     3 = equality (==, !=)
     4 = relational (<, <=, >, >=)
     5 = additive (+, -)
     6 = multiplicative (*, /) */
static int infix_prec(UTokenType t) {
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

static UAstBinaryOp infix_binop(UTokenType t) {
    switch (t) {
    case TOK_PLUS:  return BOP_ADD;
    case TOK_MINUS: return BOP_SUB;
    case TOK_STAR:  return BOP_MUL;
    case TOK_SLASH: return BOP_DIV;
    default:        return BOP_ADD; /* unreachable when prec > 0 */
    }
}

/* True when t is a comparison operator token. */
static bool is_compare_token(UTokenType t) {
    return t == TOK_EQEQ || t == TOK_NEQ
        || t == TOK_LT   || t == TOK_LE
        || t == TOK_GT   || t == TOK_GE;
}

static UAstCompareOp compare_op(UTokenType t) {
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

static UAstNode *make_compare(UParser *p, UAstCompareOp op,
                              UAstNode *lhs, UAstNode *rhs,
                              int line, int col) {
    UAstNode *n = make_node(p, AST_COMPARE, line, col);
    if (!n) return NULL;
    n->u.cmp.op  = op;
    n->u.cmp.lhs = lhs;
    n->u.cmp.rhs = rhs;
    return n;
}

static UAstNode *make_bool_node(UParser *p, bool value, int line, int col) {
    UAstNode *n = make_node(p, AST_BOOL, line, col);
    if (!n) return NULL;
    n->u.b = value;
    return n;
}

static UAstNode *make_nil_node(UParser *p, int line, int col) {
    return make_node(p, AST_NIL, line, col);
}

/* --- parse_prefix: unary +/- then atom.  Unary '+' is a no-op. --- */

static UAstNode *parse_prefix(UParser *p) {
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
    return parse_atom(p);
}

/* --- parse_atom: INT | IDENT | true | false | nil | ( expr ) | error. --- */

static UAstNode *parse_atom(UParser *p) {
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
    case TOK_KW_CLOSURE:
        consume(p);
        return make_error(p, PARSE_CLOSURE_KEYWORD,
                          kErrorMessages[PARSE_CLOSURE_KEYWORD],
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

/* --- parse_var_decl: `var x = expr` --- */

static UAstNode *parse_var_decl(UParser *p) {
    UToken kw = consume(p);          /* consume TOK_KW_VAR */
    UToken name = peek(p);
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

static UAstNode *parse_assign_from_ident(UParser *p, UToken name) {
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

static UAstNode *parse_statement_or_expr(UParser *p) {
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

    /* x = expr — detect by consuming IDENT then peeking for TOK_EQ.
       If not TOK_EQ, put the ident back as the LHS and continue with
       the normal inner-tier path (Pratt climb + pipe/amp loop). */
    if (t.type == TOK_IDENT) {
        UToken name = consume(p);
        if (peek(p).type == TOK_EQ) {
            return parse_assign_from_ident(p, name);
        }
        /* Not assignment: build the ident node and finish the Pratt climb
           for the arithmetic expression (including postfix calls), then
           hand to the inner-tier pipe/amp separator loop. */
        UAstNode *lhs = make_ident(p, name.u.str.start, name.u.str.len,
                                   name.line, name.col);
        if (!lhs) return NULL;
        /* Pratt climb: handle postfix `(args)` and arithmetic operators. */
        for (;;) {
            UToken op = peek(p);
            /* Postfix call: highest precedence. */
            if (op.type == TOK_LPAREN) {
                lhs = parse_call_args(p, lhs);
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

/* --- parse_call_args: parse `(` arg, arg, ... `)` after a callee expression.
   Returns an AST_CALL node. callee is already parsed. --- */

static UAstNode *parse_call_args(UParser *p, UAstNode *callee) {
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
            int new_cap = cap * 2;
            UAstNode **bigger = (UAstNode **)uarena_alloc(p->arena,
                                                           (size_t)new_cap * sizeof(UAstNode *));
            if (!bigger) return (UAstNode *)&uparser_oom_sentinel;
            int i;
            for (i = 0; i < count; i++) bigger[i] = args[i];
            args = bigger;
            cap = new_cap;
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

/* --- parse_expression: Pratt precedence climbing over parse_prefix. --- */

static UAstNode *parse_expression(UParser *p, int min_prec) {
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

/* True when the next token is a statement-terminating position:
   end-of-block, end-of-paren, EOF, or the REPL-boundary '|'.
   Used by the trailing-drop rules for outer-tier ';'/','  and
   inner-tier '|'/'&'. */
static bool at_statement_end(UParser *p) {
    UTokenType t = peek(p).type;
    return t == TOK_EOF || t == TOK_RBRACE || t == TOK_RPAREN
        || t == TOK_PIPE;
}

/* Inner-tier: parse an arithmetic expression, then left-fold `|` and `&`
   binops at equal precedence (left-associative).
   Trailing `|` at statement-end is silently dropped.
   Trailing `&` at statement-end is a parse error (PARSE_TRAILING_AMP). */
static UAstNode *parse_inner_tier(UParser *p) {
    UAstNode *lhs = parse_expression(p, 0);
    if (!lhs) return NULL;
    if (lhs->kind == AST_ERROR) return lhs;

    for (;;) {
        UToken sep = peek(p);
        if (sep.type != TOK_PIPE && sep.type != TOK_AMP) break;
        consume(p);
        UAstSeparator s = (sep.type == TOK_PIPE) ? SEP_PIPE : SEP_AMP;

        /* Trailing-drop: pipe at end → drop silently and return lhs.
           Trailing amp at end → parse error per spec §3.
           Also treat an immediately following '|' as a trailing boundary
           so that the REPL's appended " |" doesn't hide a trailing '&'. */
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

/* --- parse_block: `{` stmts `}` → AST_BLOCK.
   Each statement inside is a full outer-tier parse (including `;` chains).
   Statements are separated by `;` or `|`; a missing separator ends the block.
   Used by if/else; T13 (while) and T14 (function) will reuse this. --- */
static UAstNode *parse_block(UParser *p) {
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
static UAstNode *parse_while(UParser *p) {
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
static UAstNode *parse_if(UParser *p) {
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

/* --- parse_function: `function` [`name`] `(` params `)` `{` body `}` --- */

static UAstNode *parse_function(UParser *p) {
    UToken kw = consume(p);   /* consume TOK_KW_FUNCTION */

    /* Detect bare-function forms and reject them.
       `function {`    → bare anonymous (no parens)
       `function name {` → bare named (no parens after name)
       Both are retired at v1.0. */
    {
        UToken next = peek(p);
        if (next.type == TOK_LBRACE) {
            return make_error(p, PARSE_BARE_FUNCTION,
                              kErrorMessages[PARSE_BARE_FUNCTION],
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
                                  kErrorMessages[PARSE_BARE_FUNCTION],
                                  name_tok.line, name_tok.col);
            }
            /* IDENT followed by '(' — treat as named function (name stored but
             * not yet used by emit at T14; T15 will wire named-function emit). */
            /* Fall through to parse the param list. */
            /* Note: name_tok is consumed; we don't store it at T14. */
            (void)name_tok;
        }
    }

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

static UAstNode *parse_return(UParser *p) {
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

/* Outer-tier: parse one or more inner-tier expressions joined by `;` or `,`.
   Returns a single node (no Nary) if only one inner-tier child exists.
   Trailing `;` or `,` at statement-end is silently dropped.
   Mixing `;` and `,` in the same outer-tier group is an error. */
static UAstNode *parse_outer_tier(UParser *p) {
    UAstNode *first = parse_statement_or_expr(p);
    if (!first) return NULL;
    if (first->kind == AST_ERROR) return first;

    /* If no outer-tier separator follows, return the single node unwrapped. */
    UToken sep0 = peek(p);
    if (sep0.type != TOK_SEMI && sep0.type != TOK_COMMA) return first;

    consume(p);
    UAstSeparator sep_kind = (sep0.type == TOK_SEMI) ? SEP_SEMI : SEP_COMMA;

    /* Trailing-drop: separator immediately at statement-end → return first. */
    if (at_statement_end(p)) return first;

    /* Build children array.  Use a fixed-capacity initial block; grow by
       doubling (all via arena — no free, arena owns all memory). */
    int cap = 8;
    UAstNode **children = (UAstNode **)uarena_alloc(p->arena,
                                                     (size_t)cap * sizeof(UAstNode *));
    if (!children) return (UAstNode *)&uparser_oom_sentinel;
    children[0] = first;
    int count = 1;

    for (;;) {
        UAstNode *child = parse_statement_or_expr(p);
        if (!child) return NULL;
        if (child->kind == AST_ERROR) return child;

        if (count == cap) {
            int new_cap = cap * 2;
            UAstNode **bigger = (UAstNode **)uarena_alloc(p->arena,
                                                           (size_t)new_cap * sizeof(UAstNode *));
            if (!bigger) return (UAstNode *)&uparser_oom_sentinel;
            for (int i = 0; i < count; i++) bigger[i] = children[i];
            children = bigger;
            cap = new_cap;
        }
        children[count++] = child;

        UToken s = peek(p);
        if (s.type != TOK_SEMI && s.type != TOK_COMMA) break;

        UAstSeparator s_kind = (s.type == TOK_SEMI) ? SEP_SEMI : SEP_COMMA;
        if (s_kind != sep_kind) {
            consume(p);
            return make_error(p, PARSE_UNEXPECTED_TOKEN,
                              "mixing ';' and ',' in same group is ambiguous; group with parens",
                              s.line, s.col);
        }
        consume(p);

        /* Trailing-drop: separator at statement-end → stop collecting. */
        if (at_statement_end(p)) break;
    }

    UAstNode *node = make_node(p, AST_NARY, first->line, first->col);
    if (!node) return NULL;
    node->u.nary.separator = sep_kind;
    node->u.nary.children  = children;
    node->u.nary.count     = count;
    return node;
}

/* Advance the lexer until peek is TOK_PIPE or TOK_EOF.  If we land on
   TOK_PIPE, consume it so the next statement starts clean. */
static void sync_to_statement_boundary(UParser *p) {
    for (;;) {
        UToken t = peek(p);
        if (t.type == TOK_PIPE) { consume(p); return; }
        if (t.type == TOK_EOF) return;
        consume(p);
    }
}

/* --- Public API. --- */

void uparse_init(UParser *p, ULexer *lex, UArena *arena) {
    p->lex = lex;
    p->arena = arena;
    p->have_peek = false;
}

UAstNode *uparse_next_statement(UParser *p) {
    if (p->arena->oom) return (UAstNode *)&uparser_oom_sentinel;

    UToken t = peek(p);
    if (t.type == TOK_EOF) return NULL;

    UAstNode *stmt = parse_outer_tier(p);
    if (!stmt || p->arena->oom) return (UAstNode *)&uparser_oom_sentinel;

    if (stmt->kind == AST_ERROR) {
        sync_to_statement_boundary(p);
        return stmt;
    }

    /* Consume trailing `|` (REPL statement-boundary convention). */
    if (peek(p).type == TOK_PIPE) {
        consume(p);
        return stmt;
    }
    if (peek(p).type == TOK_EOF) {
        return stmt;
    }

    /* Unexpected trailing token — discard the valid subtree per the
       "no partial ASTs on error" rule, emit a single error, and sync. */
    UToken term = peek(p);
    UAstNode *err = make_error(p, PARSE_UNEXPECTED_TOKEN,
                              kErrorMessages[PARSE_UNEXPECTED_TOKEN],
                              term.line, term.col);
    if (!err || p->arena->oom) return (UAstNode *)&uparser_oom_sentinel;
    sync_to_statement_boundary(p);
    return err;
}

const char *uparse_error_name(UParseError code) {
    int i = (int)code;
    if (i < 0 || i >= N_PARSE_ERROR_CODES) return "PARSE_UNKNOWN";
    return kErrorNames[i];
}
