/* SPDX-License-Identifier: BSD-3-Clause */
/* Public lexer API. */

#ifndef ULEX_H
#define ULEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOK_EOF = 0,      /* end of input — sentinel */
    TOK_INT,          /* integer literal */
    TOK_IDENT,        /* identifier [a-zA-Z_][a-zA-Z0-9_]* */
    TOK_PLUS,         /* + */
    TOK_MINUS,        /* - */
    TOK_STAR,         /* * */
    TOK_SLASH,        /* / */
    TOK_LPAREN,       /* ( */
    TOK_RPAREN,       /* ) */
    TOK_PIPE,         /* | */
    TOK_ERROR         /* malformed input */
} TokenType;

typedef enum {
    LEX_OK = 0,
    LEX_UNKNOWN_CHAR,
    LEX_UNTERMINATED_BLOCK_COMMENT,
    LEX_AMBIGUOUS_LEADING_ZERO,
    LEX_EMPTY_RADIX,
    LEX_MALFORMED_HEX,
    LEX_MALFORMED_BIN,
    LEX_MALFORMED_OCT,
    LEX_LEADING_UNDERSCORE,
    LEX_TRAILING_UNDERSCORE,
    LEX_ADJACENT_UNDERSCORES,
    LEX_INT_OVERFLOW
} LexErrorCode;

typedef struct {
    TokenType type;
    int line;         /* 1-based */
    int col;          /* 1-based column of first char */
    int len;          /* span length in source chars */
    union {
        int64_t i;                          /* TOK_INT */
        struct {                            /* TOK_IDENT */
            const char *start;
            int len;
        } str;
        struct {                            /* TOK_ERROR */
            int code;                       /* LexErrorCode */
            const char *message;            /* static string */
        } err;
    } u;
} Token;

typedef struct {
    const char *src;
    const char *end;
    const char *cur;
    int line;
    const char *line_start;
} Lexer;

void ulex_init(Lexer *lex, const char *src, size_t len);
Token ulex_next(Lexer *lex);
const char *ulex_token_name(TokenType t);

#ifdef __cplusplus
}
#endif

#endif
