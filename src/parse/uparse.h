/* SPDX-License-Identifier: BSD-3-Clause */
/* Streaming Pratt parser over the lexer's token stream. */

#ifndef UPARSE_H
#define UPARSE_H

#include <stdbool.h>

#include "value/uarena.h"
#include "parse/uast.h"
#include "lex/ulex.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * UParser — stack-allocated by the caller; initialized by uparse_init.
 * Borrows both ULexer and UArena; both must outlive the UParser and any
 * UAstNode returned from uparse_next_statement.
 */
typedef struct {
    ULexer *lex;
    UArena *arena;
    UToken peek;
    bool have_peek;
    /* Second-token lookahead.  Used by T41 (get/set parse sugar) to detect
     * `get IDENT (` / `set IDENT (` patterns without an irreversible commit:
     * after the current token (peek), peek2() returns the token AFTER it.
     * Filled lazily by peek2(); consumed alongside peek by consume(). */
    UToken peek2;
    bool have_peek2;
    /* Set by parse_at while parsing the condition expression inside `at(...)`.
     * When true, the postfix `?` handler in parse_expression passes through
     * the token (does not error) so parse_at can detect it after the fact. */
    bool at_event_cond;
} UParser;

/* Initialize.  No allocation.  Both lex and arena must outlive p. */
void uparse_init(UParser *p, ULexer *lex, UArena *arena);

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
UAstNode *uparse_next_statement(UParser *p);

/* Return a static string such as "PARSE_EXPECTED_RPAREN" for the given
   code.  Debug helper; bounds-guarded (out-of-range returns
   "PARSE_UNKNOWN").  Never allocates. */
const char *uparse_error_name(UParseError code);

#ifdef __cplusplus
}
#endif

#endif
