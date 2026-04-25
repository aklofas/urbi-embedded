/* SPDX-License-Identifier: BSD-3-Clause */
/* Streaming Pratt parser implementation. */

#include "uparse.h"
#include <stddef.h>

/* --- Static error-message table.  Indices must match UParseError. --- */

static const char * const kErrorMessages[] = {
    "ok",
    "unexpected token; expected '|' or end of input",
    "unexpected end of input",
    "expected expression",
    "expected ')'",
    "lex error",                    /* overridden by pass-through lexer message */
    "out of memory during parsing"
};

static const char * const kErrorNames[] = {
    "PARSE_OK",
    "PARSE_UNEXPECTED_TOKEN",
    "PARSE_UNEXPECTED_EOF",
    "PARSE_EXPECTED_EXPRESSION",
    "PARSE_EXPECTED_RPAREN",
    "PARSE_LEX_ERROR",
    "PARSE_OOM"
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

    UAstNode *expr = parse_expression(p, 0);
    if (!expr || p->arena->oom) return (UAstNode *)&uparser_oom_sentinel;

    if (expr->kind == AST_ERROR) {
        sync_to_statement_boundary(p);
        return expr;
    }

    /* Statement boundary: expect '|' or EOF. */
    UToken term = peek(p);
    if (term.type == TOK_PIPE) {
        consume(p);
        return expr;
    }
    if (term.type == TOK_EOF) {
        return expr;
    }

    /* Unexpected trailing token — discard the valid subtree per the
       "no partial ASTs on error" rule, emit a single error, and sync. */
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
