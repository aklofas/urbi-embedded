/* SPDX-License-Identifier: BSD-3-Clause */
/* Lexer. */

#include "ulex.h"

#include <string.h>

static const char *TOKEN_NAMES[] = {
    "TOK_EOF", "TOK_INT", "TOK_IDENT",
    "TOK_PLUS", "TOK_MINUS", "TOK_STAR", "TOK_SLASH",
    "TOK_LPAREN", "TOK_RPAREN", "TOK_PIPE",
    "TOK_ERROR"
};

static const char *ERR_MSG[] = {
    "ok",
    "unknown character",
    "unterminated block comment",
    "ambiguous leading-zero numeric; use 0x / 0b / 0o or drop the leading zero",
    "empty radix literal",
    "malformed hex literal",
    "malformed binary literal",
    "malformed octal literal",
    "leading underscore in numeric literal",
    "trailing underscore in numeric literal",
    "adjacent underscores in numeric literal",
    "integer literal exceeds INT64_MAX"
};

static Token make_error(const Lexer *l, LexErrorCode code,
                       int line, int col, int len) {
    Token t;
    memset(&t, 0, sizeof(t));
    t.type = TOK_ERROR;
    t.line = line;
    t.col = col;
    t.len = len;
    t.u.err.code = code;
    t.u.err.message = ERR_MSG[code];
    (void)l;
    return t;
}

static Token make_tok(const Lexer *l, TokenType type,
                     const char *start, int len) {
    Token t;
    memset(&t, 0, sizeof(t));
    t.type = type;
    t.line = l->line;
    t.col = (int)(start - l->line_start) + 1;
    t.len = len;
    return t;
}

void ulex_init(Lexer *lex, const char *src, size_t len) {
    lex->src = src;
    lex->end = src + len;
    lex->cur = src;
    lex->line = 1;
    lex->line_start = src;
}

static Token make_eof(const Lexer *l) {
    Token t;
    memset(&t, 0, sizeof(t));
    t.type = TOK_EOF;
    t.line = l->line;
    t.col = (int)(l->cur - l->line_start) + 1;
    return t;
}

typedef struct {
    LexErrorCode code;
    int line;
    int col;
} TriviaResult;

static TriviaResult skip_trivia(Lexer *l) {
    TriviaResult r = {LEX_OK, 0, 0};
    while (l->cur < l->end) {
        char c = *l->cur;
        if (c == ' ' || c == '\t') {
            l->cur++;
        } else if (c == '\n') {
            l->cur++;
            l->line++;
            l->line_start = l->cur;
        } else if (c == '\r') {
            l->cur++;
            if (l->cur < l->end && *l->cur == '\n') {
                l->cur++;
            }
            l->line++;
            l->line_start = l->cur;
        } else if (c == '/' && l->cur + 1 < l->end && l->cur[1] == '/') {
            /* Line comment — skip to LF or EOF. */
            l->cur += 2;
            while (l->cur < l->end && *l->cur != '\n') {
                l->cur++;
            }
        } else if (c == '/' && l->cur + 1 < l->end && l->cur[1] == '*') {
            /* Block comment — record start for error reporting. */
            int start_line = l->line;
            int start_col = (int)(l->cur - l->line_start) + 1;
            l->cur += 2;
            int closed = 0;
            while (l->cur + 1 < l->end) {
                if (l->cur[0] == '*' && l->cur[1] == '/') {
                    l->cur += 2;
                    closed = 1;
                    break;
                }
                if (*l->cur == '\n') {
                    l->line++;
                    l->line_start = l->cur + 1;
                }
                l->cur++;
            }
            if (!closed) {
                /* Advance cur to end so caller sees EOF on retry. */
                l->cur = l->end;
                r.code = LEX_UNTERMINATED_BLOCK_COMMENT;
                r.line = start_line;
                r.col = start_col;
                return r;
            }
        } else {
            break;
        }
    }
    return r;
}

Token ulex_next(Lexer *lex) {
    TriviaResult tr = skip_trivia(lex);
    if (tr.code != LEX_OK) {
        return make_error(lex, tr.code, tr.line, tr.col, 2);
    }
    if (lex->cur >= lex->end) {
        return make_eof(lex);
    }

    const char *start = lex->cur;
    char c = *lex->cur;
    switch (c) {
    case '+': lex->cur++; return make_tok(lex, TOK_PLUS,   start, 1);
    case '-': lex->cur++; return make_tok(lex, TOK_MINUS,  start, 1);
    case '*': lex->cur++; return make_tok(lex, TOK_STAR,   start, 1);
    case '/': lex->cur++; return make_tok(lex, TOK_SLASH,  start, 1);
    case '(': lex->cur++; return make_tok(lex, TOK_LPAREN, start, 1);
    case ')': lex->cur++; return make_tok(lex, TOK_RPAREN, start, 1);
    case '|': lex->cur++; return make_tok(lex, TOK_PIPE,   start, 1);
    default:
        /* Unknown character — populated in a later task. */
        lex->cur++;
        return make_eof(lex);
    }
}

const char *ulex_token_name(TokenType t) {
    if ((unsigned)t >= sizeof(TOKEN_NAMES) / sizeof(TOKEN_NAMES[0])) {
        return "TOK_UNKNOWN";
    }
    return TOKEN_NAMES[t];
}
