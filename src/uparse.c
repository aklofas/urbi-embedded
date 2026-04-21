/* SPDX-License-Identifier: BSD-3-Clause */
/* Streaming Pratt parser implementation. */

#include "uparse.h"
#include <stddef.h>

/* --- Static error-message table.  Indices must match ParseErrorCode. --- */

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

static AstNode uparser_oom_sentinel = {
    AST_ERROR,
    0,
    0,
    { .err = { PARSE_OOM, "out of memory during parsing" } }
};

/* --- Lexer lookahead helpers. --- */

static Token peek(Parser *p) {
    if (!p->have_peek) {
        p->peek = ulex_next(p->lex);
        p->have_peek = true;
    }
    return p->peek;
}

static Token consume(Parser *p) {
    Token t = peek(p);
    p->have_peek = false;
    return t;
}

/* --- AST constructors.  Return NULL on arena OOM. --- */

static AstNode *make_node(Parser *p, AstKind k, int line, int col) {
    AstNode *n = uarena_alloc(p->arena, sizeof *n);
    if (!n) return NULL;
    n->kind = k;
    n->line = line;
    n->col = col;
    return n;
}

static AstNode *make_int(Parser *p, int64_t v, int line, int col) {
    AstNode *n = make_node(p, AST_INT, line, col);
    if (!n) return NULL;
    n->u.i = v;
    return n;
}

static AstNode *make_ident(Parser *p, const char *start, int len, int line, int col) {
    AstNode *n = make_node(p, AST_IDENT, line, col);
    if (!n) return NULL;
    n->u.ident.start = start;
    n->u.ident.len = len;
    return n;
}

static AstNode *make_error(Parser *p, ParseErrorCode code, const char *msg,
                           int line, int col) {
    AstNode *n = make_node(p, AST_ERROR, line, col);
    if (!n) return NULL;
    n->u.err.code = (int)code;
    n->u.err.message = msg ? msg : kErrorMessages[code];
    return n;
}

/* --- Forward declarations for mutual recursion. --- */

static AstNode *parse_expression(Parser *p, int min_prec);
static AstNode *parse_atom(Parser *p);

/* --- parse_atom: INT | IDENT | ( expr ) | error. --- */

static AstNode *parse_atom(Parser *p) {
    Token t = peek(p);
    switch (t.type) {
    case TOK_INT:
        consume(p);
        return make_int(p, t.u.i, t.line, t.col);
    case TOK_IDENT:
        consume(p);
        return make_ident(p, t.u.str.start, t.u.str.len, t.line, t.col);
    case TOK_LPAREN: {
        consume(p);
        AstNode *inner = parse_expression(p, 0);
        if (!inner) return NULL;
        if (inner->kind == AST_ERROR) return inner;
        Token r = peek(p);
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

/* --- parse_expression stub: atoms only for now (unary / binary later). --- */

static AstNode *parse_expression(Parser *p, int min_prec) {
    (void)min_prec;
    return parse_atom(p);
}

/* --- Public API. --- */

void uparse_init(Parser *p, Lexer *lex, Arena *arena) {
    p->lex = lex;
    p->arena = arena;
    p->have_peek = false;
}

AstNode *uparse_next_statement(Parser *p) {
    if (p->arena->oom) return &uparser_oom_sentinel;

    Token t = peek(p);
    if (t.type == TOK_EOF) return NULL;

    AstNode *expr = parse_expression(p, 0);
    if (!expr || p->arena->oom) return &uparser_oom_sentinel;

    /* Optional trailing '|'. */
    Token term = peek(p);
    if (term.type == TOK_PIPE) consume(p);

    return expr;
}

const char *uparse_error_name(ParseErrorCode code) {
    int i = (int)code;
    if (i < 0 || i >= N_PARSE_ERROR_CODES) return "PARSE_UNKNOWN";
    return kErrorNames[i];
}
