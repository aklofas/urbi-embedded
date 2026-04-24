/* SPDX-License-Identifier: BSD-3-Clause */
/* Public lexer API. */

#ifndef ULEX_H
#define ULEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Full token-type space of the lexer.  Every call to ulex_next returns
   exactly one of these values in UToken.type. */
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
} UTokenType;

/* Error codes carried in UToken.u.err.code when UToken.type == TOK_ERROR. */
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
} ULexError;

/*
 * UToken — returned by value from ulex_next; no heap allocation.
 *
 * Lifetime: u.str.start is a non-owning pointer into the caller's source
 * buffer.  The source buffer MUST outlive any UToken that references it.
 *
 * Union invariants (active member per type):
 *   u.i    — TOK_INT: the parsed integer value
 *   u.str  — TOK_IDENT: start/len point into the source buffer
 *   u.err  — TOK_ERROR: code (ULexError) + static message string
 *   (other types leave u zero-valued)
 *
 * Position fields: line and col are 1-based; len is the span in source bytes.
 */
typedef struct {
    UTokenType type;
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
            int code;                       /* ULexError */
            const char *message;            /* static string */
        } err;
    } u;
} UToken;

/*
 * ULexer — stack-allocated by the caller; zero-initialized by ulex_init.
 * No heap allocation at any point during lexing.
 *
 * The source buffer (src) must outlive the ULexer and any UTokens derived
 * from it, and must not be modified while lexing is in progress.
 */
typedef struct {
    const char *src;
    const char *end;
    const char *cur;
    int line;
    const char *line_start;
} ULexer;

/* Initialize the ULexer over a source buffer.  No allocation.
   src must point to at least len valid bytes and must remain valid and
   unmodified for the lifetime of the ULexer and all UTokens it produces. */
void ulex_init(ULexer *lex, const char *src, size_t len);

/* Read and return the next UToken.  Idempotent at EOF — subsequent calls
   keep returning TOK_EOF.  After TOK_ERROR the cursor has advanced past the
   offending byte; the caller may continue lexing for error recovery. */
UToken ulex_next(ULexer *lex);

/* Return a static string such as "TOK_PLUS" for the given type.
   Debug helper; bounds-guarded (out-of-range returns "TOK_UNKNOWN").
   Never allocates. */
const char *ulex_token_name(UTokenType t);

#ifdef __cplusplus
}
#endif

#endif
