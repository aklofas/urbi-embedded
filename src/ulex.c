/* SPDX-License-Identifier: BSD-3-Clause */
/* Lexer. */

#include "ulex.h"

static const char *TOKEN_NAMES[] = {
    "TOK_EOF", "TOK_INT", "TOK_IDENT",
    "TOK_PLUS", "TOK_MINUS", "TOK_STAR", "TOK_SLASH",
    "TOK_LPAREN", "TOK_RPAREN", "TOK_PIPE",
    "TOK_ERROR"
};

void ulex_init(Lexer *lex, const char *src, size_t len) {
    lex->src = src;
    lex->end = src + len;
    lex->cur = src;
    lex->line = 1;
    lex->line_start = src;
}

static Token make_eof(const Lexer *l) {
    Token t;
    t.type = TOK_EOF;
    t.line = l->line;
    t.col = (int)(l->cur - l->line_start) + 1;
    t.len = 0;
    t.i = 0;
    return t;
}

Token ulex_next(Lexer *lex) {
    /* Stub: always return EOF. Scan logic lands in later steps. */
    return make_eof(lex);
}

const char *ulex_token_name(TokenType t) {
    return TOKEN_NAMES[t];
}
