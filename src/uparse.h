/* SPDX-License-Identifier: BSD-3-Clause */
/* Streaming Pratt parser over the lexer's token stream. */

#ifndef UPARSE_H
#define UPARSE_H

#include <stdbool.h>

#include "uarena.h"
#include "uast.h"
#include "ulex.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parser — stack-allocated by the caller; initialized by uparse_init.
 * Borrows both ULexer and Arena; both must outlive the Parser and any
 * UAstNode returned from uparse_next_statement.
 */
typedef struct {
    ULexer *lex;
    Arena *arena;
    UToken peek;
    bool have_peek;
} Parser;

/* Initialize.  No allocation.  Both lex and arena must outlive p. */
void uparse_init(Parser *p, ULexer *lex, Arena *arena);

/*
 * Parse the next statement.
 *
 * Returns NULL at EOF (idempotent; further calls continue to return NULL).
 * Returns a non-NULL UAstNode* otherwise, which may have kind AST_ERROR.
 *
 * On error: builds AST_ERROR at the detection point, discards any
 * partial subtree, and advances the lexer past the next TOK_PIPE
 * (or to EOF).  The next call starts cleanly from the following tokens.
 *
 * Returned UAstNodes remain valid until uarena_reset / uarena_destroy
 * on the arena.  Typical use: process the tree, then call
 * uarena_reset(arena) before the next uparse_next_statement call.
 */
UAstNode *uparse_next_statement(Parser *p);

/* Return a static string such as "PARSE_EXPECTED_RPAREN" for the given
   code.  Debug helper; bounds-guarded (out-of-range returns
   "PARSE_UNKNOWN").  Never allocates. */
const char *uparse_error_name(ParseErrorCode code);

#ifdef __cplusplus
}
#endif

#endif
