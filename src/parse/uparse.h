/* SPDX-License-Identifier: BSD-3-Clause */
/* Streaming Pratt parser over the lexer's token stream. */

#ifndef UPARSE_H
#define UPARSE_H

#include <stdbool.h>

#include "value/uarena.h"
#include "parse/uast.h"
#include "lex/ulex.h"
#include "urbi/types.h"   /* UCompileBudget (v0.9.1) */

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
    /* T41 (Wave 2): nesting depth of `class { ... }` bodies currently being
     * parsed.  Bumped by parse_class_declaration around its parse_block call.
     * Statement-start `get name() {...}` / `set name(v) {...}` is rejected
     * at parse time when this is zero — the implicit-receiver form has no
     * v1.0 resolver outside a class body (deferred to v1.x implicit-this). */
    int class_body_depth;
    /* === W1/v0.10.5: control flow ===
     * Nesting depth of for/while loops currently being parsed.  Bumped
     * around each loop body parse so that break/continue can be detected
     * outside any loop (PARSE_BREAK_OUTSIDE_LOOP / PARSE_CONTINUE_OUTSIDE_LOOP).
     * Checked at parse time in parse_break / parse_continue; does not affect
     * emit (the emitter independently tracks break/continue patch lists). */
    int loop_depth;

    /* === v0.9.1 compile-budget guard ===
     *
     * budget — borrowed pointer to a UCompileBudget supplied by the caller
     *   (typically realm->compile_budget when urbi_repl_eval drives the
     *   parser under a REPL realm).  NULL = unlimited (default).
     *
     * cur_depth — current recursive-descent depth.  Bumped by
     *   uparse_budget_enter on every entry into a recursive parser entry
     *   point; decremented by uparse_budget_leave.  Compared against
     *   budget->max_parser_depth.
     *
     * node_count — running tally of every make_node() success.  Compared
     *   against budget->max_ast_nodes.
     *
     * budget_exceeded — sticky latch set when any limit is first crossed.
     *   Once set, make_node() and uparse_budget_enter return failure for
     *   every subsequent call, so the parse cleanly aborts.  The specific
     *   error is recorded in budget_err for the caller (urbi_repl_eval)
     *   to translate into the right UErrCode.
     *
     * budget_err — one of URBI_ERR_COMPILE_BUDGET_{DEPTH,NODES,SOURCE}.
     *   URBI_OK while no limit is exceeded. */
    const UCompileBudget *budget;
    uint32_t cur_depth;
    uint32_t node_count;
    bool     budget_exceeded;
    int      budget_err;
} UParser;

/* Initialize.  No allocation.  Both lex and arena must outlive p.
 * Initializes budget to NULL (unlimited); caller may set it after init. */
void uparse_init(UParser *p, ULexer *lex, UArena *arena);

/* === v0.9.1 budget helpers ============================================
 *
 * uparse_set_budget — install a borrowed UCompileBudget pointer (NULL =
 *   unlimited).  Must be called BEFORE the first uparse_next_statement
 *   call.  Caller owns the budget storage; UParser does not copy.
 *
 * uparse_budget_enter — recursive-descent depth check.  Returns true if
 *   the caller may proceed (depth was bumped); false if the limit is
 *   exceeded (budget_err set; subsequent calls also return false).
 *
 * uparse_budget_leave — pop one level of depth.  Always safe; ignored if
 *   no budget is installed.
 *
 * uparse_budget_err — return URBI_OK if no limit was exceeded, else the
 *   sticky URBI_ERR_COMPILE_BUDGET_* code recorded at first trip. */
void uparse_set_budget(UParser *p, const UCompileBudget *budget);
bool uparse_budget_enter(UParser *p);
void uparse_budget_leave(UParser *p);
int  uparse_budget_err(const UParser *p);

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
