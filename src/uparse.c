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
static UAstNode *parse_inner_tier(UParser *p);
static UAstNode *parse_outer_tier(UParser *p);
static UAstNode *parse_statement_or_expr(UParser *p);
static bool at_statement_end(UParser *p);

/* Return the left-binding precedence of an infix token, or 0 if not
   an infix operator (terminates the Pratt climb). */
static int infix_prec(UTokenType t) {
    switch (t) {
    case TOK_PLUS:
    case TOK_MINUS: return 1;
    case TOK_STAR:
    case TOK_SLASH: return 2;
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

/* --- parse_atom: INT | IDENT | ( expr ) | error. --- */

static UAstNode *parse_atom(UParser *p) {
    UToken t = peek(p);
    switch (t.type) {
    case TOK_INT:
        consume(p);
        return make_int(p, t.u.i, t.line, t.col);
    case TOK_IDENT:
        consume(p);
        return make_ident(p, t.u.str.start, t.u.str.len, t.line, t.col);
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

    /* var x = expr */
    if (t.type == TOK_KW_VAR) {
        return parse_var_decl(p);
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
           for the arithmetic expression, then hand to the inner-tier
           pipe/amp separator loop. */
        UAstNode *lhs = make_ident(p, name.u.str.start, name.u.str.len,
                                   name.line, name.col);
        if (!lhs) return NULL;
        /* Pratt climb with min_prec=1 (ident is already parsed; continue
           climbing for any trailing arithmetic operators). */
        for (;;) {
            UToken op = peek(p);
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

/* --- parse_expression: Pratt precedence climbing over parse_prefix. --- */

static UAstNode *parse_expression(UParser *p, int min_prec) {
    UAstNode *left = parse_prefix(p);
    if (!left) return NULL;
    if (left->kind == AST_ERROR) return left;

    for (;;) {
        UToken op = peek(p);
        int prec = infix_prec(op.type);
        if (prec < min_prec || prec == 0) break;

        consume(p);
        UAstNode *right = parse_expression(p, prec + 1);
        if (!right) return NULL;
        if (right->kind == AST_ERROR) return right;

        left = make_binary(p, infix_binop(op.type), left, right,
                           op.line, op.col);
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
