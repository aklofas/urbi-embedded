/* SPDX-License-Identifier: BSD-3-Clause */
/* uparse_separators.c — separator-loop dispatch (`;` `|` `,` `&`).
 * Extracted from uparse.c during v0.5.4-decompose (PARSE-021 #3). */

#include "parse/uparse.h"
#include "parse/uparse_internal.h"

/* True when the next token is a statement-terminating position:
   end-of-block, end-of-paren, EOF, or the REPL-boundary '|'.
   Used by the trailing-drop rules for outer-tier ';'/','  and
   inner-tier '|'/'&'. */
bool at_statement_end(UParser *p) {
    UTokenType t = peek(p).type;
    return t == TOK_EOF || t == TOK_RBRACE || t == TOK_RPAREN
        || t == TOK_PIPE;
}

/* pipe_amp_fold: left-fold `|` and `&` binops starting from an already-parsed
   lhs.  Shared by parse_inner_tier and parse_inner_tier_from_lhs. */
static UAstNode *pipe_amp_fold(UParser *p, UAstNode *lhs) {
    for (;;) {
        UToken sep = peek(p);
        if (sep.type != TOK_PIPE && sep.type != TOK_AMP) break;
        consume(p);
        UAstSeparator s = (sep.type == TOK_PIPE) ? SEP_PIPE : SEP_AMP;

        /* Trailing-drop: pipe at end → drop silently and return lhs.
           Trailing amp at end → parse error per spec §3.
           Also treat an immediately following '|' as a trailing boundary
           so that the REPL's appended " |" doesn't hide a trailing '&'. */
        bool trail = at_statement_end(p)
                  || peek(p).type == TOK_SEMI
                  || peek(p).type == TOK_COMMA
                  || peek(p).type == TOK_PIPE;
        if (trail) {
            if (s == SEP_PIPE) return lhs;
            return make_error(p, PARSE_TRAILING_AMP, NULL,
                              sep.line, sep.col);
        }

        UAstNode *rhs = parse_expression(p, 0);
        if (!rhs) return NULL;
        if (rhs->kind == AST_ERROR) return rhs;

        UAstNode *node = make_node(p, AST_BIN_SEP, sep.line, sep.col);
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
UAstNode *parse_inner_tier(UParser *p) {
    UAstNode *lhs = parse_expression(p, 0);
    if (!lhs) return NULL;
    if (lhs->kind == AST_ERROR) return lhs;
    return pipe_amp_fold(p, lhs);
}

/* parse_inner_tier_from_lhs: resume inner-tier parsing from an already-parsed
   lhs node (e.g. an IDENT already consumed by the statement dispatcher).
   Runs parse_expression_cont(p, lhs, 0) to finish the Pratt climb, then
   pipe_amp_fold for the | / & separator loop. */
UAstNode *parse_inner_tier_from_lhs(UParser *p, UAstNode *lhs) {
    lhs = parse_expression_cont(p, lhs, 0);
    if (!lhs) return NULL;
    if (lhs->kind == AST_ERROR) return lhs;
    return pipe_amp_fold(p, lhs);
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
UAstNode *parse_outer_tier(UParser *p) {
    UAstNode *first = parse_statement_or_expr(p);
    if (!first) return NULL;
    if (first->kind == AST_ERROR) return first;

    /* If no outer-tier separator follows, return the single node unwrapped. */
    UToken sep0 = peek(p);
    if (sep0.type != TOK_SEMI && sep0.type != TOK_COMMA) return first;

    consume(p);
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
        UAstNode *child = parse_statement_or_expr(p);
        if (!child) return NULL;
        if (child->kind == AST_ERROR) return child;

        if (count == cap) {
            if (!arena_grow_node_array(p, &children, &cap, count))
                return (UAstNode *)&uparser_oom_sentinel;
        }
        children[count++] = child;

        UToken s = peek(p);
        if (s.type != TOK_SEMI && s.type != TOK_COMMA) break;

        UAstSeparator s_kind = (s.type == TOK_SEMI) ? SEP_SEMI : SEP_COMMA;
        if (s_kind != sep_kind) {
            consume(p);
            return make_error(p, PARSE_UNEXPECTED_TOKEN,
                              "mixing ';' and ',' in same group is ambiguous; group with parens",
                              s.line, s.col);
        }
        consume(p);

        /* Trailing-drop: separator at statement-end → stop collecting. */
        if (at_statement_end(p)) break;
    }

    UAstNode *node = make_node(p, AST_NARY, first->line, first->col);
    if (!node) return NULL;
    node->u.nary.separator = sep_kind;
    node->u.nary.children  = children;
    node->u.nary.count     = count;
    return node;
}
