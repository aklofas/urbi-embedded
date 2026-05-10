/* SPDX-License-Identifier: BSD-3-Clause */
/* uparse_top.c — parser entry points and error-recovery helpers.
 * Extracted from uparse.c during v0.5.4-decompose (PARSE-021 #6). */

#include "parse/uparse.h"
#include "parse/uparse_internal.h"
#include "lex/ulex.h"
#include "parse/uast.h"
#include "value/uarena.h"
#include <stddef.h>

/* Advance the lexer until peek is TOK_PIPE or TOK_EOF.  If we land on
   TOK_PIPE, consume it so the next statement starts clean. */
void sync_to_statement_boundary(UParser *p) {
    for (;;) {
        UToken t = peek(p);
        if (t.type == TOK_PIPE) { consume(p); return; }
        if (t.type == TOK_EOF) return;
        consume(p);
    }
}

void uparse_init(UParser *p, ULexer *lex, UArena *arena) {
    p->lex = lex;
    p->arena = arena;
    p->have_peek = false;
    p->have_peek2 = false;
    p->at_event_cond = false;
    p->class_body_depth = 0;
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
    UAstNode *err = make_error(p, PARSE_UNEXPECTED_TOKEN, NULL,
                               term.line, term.col);
    if (!err || p->arena->oom) return (UAstNode *)&uparser_oom_sentinel;
    sync_to_statement_boundary(p);
    return err;
}
