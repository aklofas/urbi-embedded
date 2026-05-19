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

    /* v0.9.1 compile-budget guard — uninstalled by default. */
    p->budget          = NULL;
    p->cur_depth       = 0U;
    p->node_count      = 0U;
    p->budget_exceeded = false;
    p->budget_err      = URBI_OK;
}

/* === v0.9.1 budget helpers ============================================ */

void uparse_set_budget(UParser *p, const UCompileBudget *budget) {
    if (!p) return;
    p->budget          = budget;
    p->cur_depth       = 0U;
    p->node_count      = 0U;
    p->budget_exceeded = false;
    p->budget_err      = URBI_OK;
}

bool uparse_budget_enter(UParser *p) {
    if (!p) return false;
    if (p->budget_exceeded) return false;
    if (p->budget != NULL && p->budget->max_parser_depth > 0U
            && p->cur_depth >= p->budget->max_parser_depth) {
        p->budget_exceeded = true;
        p->budget_err      = URBI_ERR_COMPILE_BUDGET_DEPTH;
        return false;
    }
    p->cur_depth++;
    return true;
}

void uparse_budget_leave(UParser *p) {
    if (!p) return;
    if (p->cur_depth > 0U) p->cur_depth--;
}

int uparse_budget_err(const UParser *p) {
    if (!p) return URBI_OK;
    return p->budget_err;
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
