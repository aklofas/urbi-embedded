/* SPDX-License-Identifier: BSD-3-Clause */
/* uparse_separators.c — separator-loop dispatch (`;` `|` `,` `&`).
 * Extracted from uparse.c during v0.5.4-decompose (PARSE-021 #3). */

#include "parse/uparse.h"
#include "parse/uparse_internal.h"
#include "lex/ulex.h"
#include "parse/uast.h"
#include "value/uarena.h"
#include <stddef.h>

/* True when the next token is a statement-terminating position:
   end-of-block, end-of-paren, EOF, or the REPL-boundary '|'.
   Used by the trailing-drop rules for outer-tier ';'/','  and
   inner-tier '|'/'&'. */
static bool at_statement_end(UParser *p) {
    UTokenType t = urbi_parse_peek(p).type;
    return t == TOK_EOF || t == TOK_RBRACE || t == TOK_RPAREN
        || t == TOK_PIPE;
}

/* parse_sep_operand: parse one & / | operand.
 * Accepts a block (`TOK_LBRACE`), `if`/`while` statement forms, or falls
 * back to urbi_parse_expression.
 *
 * This enables the signature parallel-composition idiom `{a} & {b}` at
 * the inner tier.  Parallel var-declare (`var a = 1 & var b = 2`) stays
 * rejected — `var` is absent from this dispatch (deferred-v1.x).
 *
 * Note: the caller (urbi_parse_pipe_amp_fold) is a loop, so chaining is handled
 * there; this helper returns a single self-contained node per call. */
static UAstNode *parse_sep_operand(UParser *p) {
    UToken t = urbi_parse_peek(p);
    if (t.type == TOK_LBRACE)   return urbi_parse_block(p);
    if (t.type == TOK_KW_IF)    return urbi_parse_if(p);
    if (t.type == TOK_KW_WHILE) return urbi_parse_while(p);
    return urbi_parse_expression(p, 0);
}

/* urbi_parse_pipe_amp_fold: left-fold `|` and `&` binops starting from an already-parsed
   lhs.  Shared by urbi_parse_inner_tier and parse_assign_or_expr
   (v0.10.5 member-expr tag form). */
UAstNode *urbi_parse_pipe_amp_fold(UParser *p, UAstNode *lhs) {
    for (;;) {
        UToken sep = urbi_parse_peek(p);
        if (sep.type != TOK_PIPE && sep.type != TOK_AMP) break;
        urbi_parse_consume(p);
        UAstSeparator s = (sep.type == TOK_PIPE) ? SEP_PIPE : SEP_AMP;

        /* Trailing-drop: pipe at end → drop silently and return lhs.
           Trailing amp at end → parse error per spec §3.
           Also treat an immediately following '|' as a trailing boundary
           so that the REPL's appended " |" doesn't hide a trailing '&'. */
        bool trail = at_statement_end(p)
                  || urbi_parse_peek(p).type == TOK_SEMI
                  || urbi_parse_peek(p).type == TOK_COMMA
                  || urbi_parse_peek(p).type == TOK_PIPE;
        if (trail) {
            if (s == SEP_PIPE) return lhs;
            return urbi_parse_make_error(p, PARSE_TRAILING_AMP, NULL,
                              sep.line, sep.col);
        }

        UAstNode *rhs = parse_sep_operand(p);
        if (!rhs) return NULL;
        if (rhs->kind == AST_ERROR) return rhs;

        UAstNode *node = urbi_parse_make_node(p, AST_BIN_SEP, sep.line, sep.col);
        if (!node) return NULL;
        node->u.bin_sep.separator = s;
        node->u.bin_sep.lhs = lhs;
        node->u.bin_sep.rhs = rhs;
        lhs = node;
    }
    return lhs;
}

/* Inner-tier: parse an arithmetic expression, then left-fold `|` and `&`
   binops at equal precedence (left-associative).
   Trailing `|` at statement-end is silently dropped.
   Trailing `&` at statement-end is a parse error (PARSE_TRAILING_AMP). */
UAstNode *urbi_parse_inner_tier(UParser *p) {
    UAstNode *lhs = urbi_parse_expression(p, 0);
    if (!lhs) return NULL;
    if (lhs->kind == AST_ERROR) return lhs;
    return urbi_parse_pipe_amp_fold(p, lhs);
}

/* Outer-tier: parse one or more inner-tier expressions joined by `;` or `,`.
   Returns a single node (no Nary) if only one inner-tier child exists.
   Trailing `;` or `,` at statement-end is silently dropped.
   Mixing `;` and `,` in the same outer-tier group is an error.
   OOM convention: returns NULL on child OOM (preferred), but a few
   sites currently return uparser_oom_sentinel directly — see the
   OOM-sentinel comment at the top of uparse_internal.h. The top-level
   uparse_next_statement collapses both forms via the arena->oom
   recheck (closes PARSE-005). */
UAstNode *urbi_parse_outer_tier(UParser *p) {
    UAstNode *first = urbi_parse_statement_or_expr(p);
    if (!first) return NULL;
    if (first->kind == AST_ERROR) return first;

    /* If no outer-tier separator follows, return the single node unwrapped. */
    UToken sep0 = urbi_parse_peek(p);
    if (sep0.type != TOK_SEMI && sep0.type != TOK_COMMA) return first;

    urbi_parse_consume(p);
    UAstSeparator sep_kind = (sep0.type == TOK_SEMI) ? SEP_SEMI : SEP_COMMA;

    /* Trailing-drop: separator immediately at statement-end → return first. */
    if (at_statement_end(p)) return first;

    /* Build children array.  Use a fixed-capacity initial block; grow by
       doubling (all via arena — no free, arena owns all memory). */
    int cap = 8;
    UAstNode **children = (UAstNode **)uarena_alloc(p->arena,
                                                     (size_t)cap * sizeof(UAstNode *));
    if (!children) return (UAstNode *)&uparser_oom_sentinel;
    children[0] = first;
    int count = 1;

    for (;;) {
        UAstNode *child = urbi_parse_statement_or_expr(p);
        if (!child) return NULL;
        if (child->kind == AST_ERROR) return child;

        if (count == cap) {
            if (!urbi_parse_arena_grow_node_array(p, &children, &cap, count))
                return (UAstNode *)&uparser_oom_sentinel;
        }
        children[count++] = child;

        UToken s = urbi_parse_peek(p);
        if (s.type != TOK_SEMI && s.type != TOK_COMMA) break;

        UAstSeparator s_kind = (s.type == TOK_SEMI) ? SEP_SEMI : SEP_COMMA;
        if (s_kind != sep_kind) {
            urbi_parse_consume(p);
            return urbi_parse_make_error(p, PARSE_UNEXPECTED_TOKEN,
                              "mixing ';' and ',' in same group is ambiguous; group with parens",
                              s.line, s.col);
        }
        urbi_parse_consume(p);

        /* Trailing-drop: separator at statement-end → stop collecting. */
        if (at_statement_end(p)) break;
    }

    UAstNode *node = urbi_parse_make_node(p, AST_NARY, first->line, first->col);
    if (!node) return NULL;
    node->u.nary.separator = sep_kind;
    node->u.nary.children  = children;
    node->u.nary.count     = count;
    return node;
}
