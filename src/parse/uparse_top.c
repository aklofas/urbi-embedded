/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * uparse_top.c — parser entry points and error-recovery helpers.
 *
 * Exposes urbi_parse() and urbi_parse_chunk() as the public API surface;
 * handles statement-boundary sync on parse errors.  Split from uparse.c
 * to keep each file to a coherent slice of the parser's responsibility.
 */

#include "parse/uparse.h"
#include "parse/uparse_internal.h"
#include "lex/ulex.h"
#include "parse/uast.h"
#include "value/uarena.h"
#include <stddef.h>

/* Advance the lexer until urbi_parse_peek is TOK_PIPE or TOK_EOF.  If we land on
   TOK_PIPE, urbi_parse_consume it so the next statement starts clean. */
static void sync_to_statement_boundary(UParser *p) {
    for (;;) {
        UToken t = urbi_parse_peek(p);
        if (t.type == TOK_PIPE) { urbi_parse_consume(p); return; }
        if (t.type == TOK_EOF) return;
        urbi_parse_consume(p);
    }
}

void uparse_init(UParser *p, ULexer *lex, UArena *arena) {
    p->lex = lex;
    p->arena = arena;
    p->have_peek = false;
    p->have_peek2 = false;
    p->at_event_cond = false;
    p->class_body_depth = 0;
    p->loop_depth   = 0;
    p->switch_depth = 0;

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

    UToken t = urbi_parse_peek(p);
    if (t.type == TOK_EOF) return NULL;

    UAstNode *stmt = urbi_parse_outer_tier(p);
    if (!stmt || p->arena->oom) return (UAstNode *)&uparser_oom_sentinel;

    if (stmt->kind == AST_ERROR) {
        sync_to_statement_boundary(p);
        return stmt;
    }

    /* Consume trailing `|` (REPL statement-boundary convention). */
    if (urbi_parse_peek(p).type == TOK_PIPE) {
        urbi_parse_consume(p);
        return stmt;
    }
    if (urbi_parse_peek(p).type == TOK_EOF) {
        return stmt;
    }

    /* Unexpected trailing token — discard the valid subtree per the
       "no partial ASTs on error" rule, emit a single error, and sync. */
    UToken term = urbi_parse_peek(p);
    UAstNode *err = urbi_parse_make_error(p, PARSE_UNEXPECTED_TOKEN, NULL,
                               term.line, term.col);
    if (!err || p->arena->oom) return (UAstNode *)&uparser_oom_sentinel;
    sync_to_statement_boundary(p);
    return err;
}
