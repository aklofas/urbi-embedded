/* SPDX-License-Identifier: BSD-3-Clause */
/* uparse_internal.h — private inter-TU API for the parse subsystem.
 * Consumed only by src/parse/ translation units. */

#ifndef UPARSE_INTERNAL_H
#define UPARSE_INTERNAL_H

#include "uparse.h"
#include "uast.h"

/* OOM sentinel convention (closes PARSE-003 / PARSE-005):
 *
 * Internal AST allocators (urbi_parse_make_node and friends) return NULL on
 * arena OOM. Pratt/recursive-descent parsers propagate NULL upward
 * unchanged; the top-level entrypoint (uparse_next_statement in
 * uparse_top.c) converts NULL → uparser_oom_sentinel at its single
 * exit by re-checking p->arena->oom. Sibling parsers MUST NOT
 * pre-convert NULL → sentinel within the recursion (avoids
 * double-reporting and keeps the OOM surface single-sourced).
 *
 * Historical note: a few interior sites currently return the sentinel
 * directly (e.g. urbi_parse_outer_tier on urbi_parse_arena_grow_node_array failure).
 * This is harmless — the top-level recheck collapses both forms — but
 * standardizing on plain NULL inside the recursion is a Wave 5 fix
 * target.  New code should follow the NULL-propagation rule. */

/* --- OOM sentinel (defined in uparse.c residual). --- */
extern const UAstNode uparser_oom_sentinel;

/* Postfix precedence level — `expr(args)`, `expr.x`, `expr->x`,
 * `expr!`, `expr?`, `expr[i]` all bind tighter than any infix operator
 * (multiplicative=7 is the tightest infix; postfix=9 sits above).
 * Used as the `min_prec <=` ceiling check in urbi_parse_expression_cont.
 * Must stay strictly above the highest infix level (multiplicative=7)
 * so postfix ops bind tighter than any infix even when an infix right
 * operand parses at multiplicative+1=8. */
#define PARSE_PREC_POSTFIX 9

/* Method name used by the postfix `e!` desugar (`e!` → `e.emit()`).
 * Single source of truth, referenced by AST_MEMBER_GET node construction
 * in uparse_react.c.  Defined in uparse.c.  Length declared explicitly
 * so callers can use kEmitMethodNameLen without strlen at runtime
 * (the array's `extern char urbi_parse_kEmitMethodName[];` form has incomplete
 * type at the use site, so sizeof is unavailable). */
extern const char urbi_parse_kEmitMethodName[];
#define kEmitMethodNameLen 4  /* strlen("emit") */

/* v0.10.11 / W3: length of the `<<` selector used in uparse_expr.c.
 * kLShiftSelector is static in uparse_expr.c (single-TU). */
#define kLShiftSelectorLen 2  /* strlen("<<") */

/* --- Error-message table (defined in uparse.c residual). --- */
extern const char * const urbi_parse_kErrorMessages[];

/* --- Token helpers (defined in uparse.c residual). --- */
UToken urbi_parse_peek(UParser *p);
UToken urbi_parse_peek2(UParser *p);
UToken urbi_parse_consume(UParser *p);

/* --- Identifier comparison (defined in uparse.c residual). --- */
int urbi_parse_ident_equals(const char *bytes, int len, const char *literal, int lit_len);

/* --- AST constructors (defined in uparse.c residual). --- */
UAstNode *urbi_parse_make_node(UParser *p, UAstKind k, int line, int col);
UAstNode *urbi_parse_make_int(UParser *p, int64_t v, int line, int col);
UAstNode *urbi_parse_make_ident(UParser *p, const char *start, int len, int line, int col);
UAstNode *urbi_parse_make_unary(UParser *p, UAstUnaryOp op, UAstNode *operand,
                     int line, int col);
UAstNode *urbi_parse_make_binary(UParser *p, UAstBinaryOp op, UAstNode *lhs, UAstNode *rhs,
                      int line, int col);
UAstNode *urbi_parse_make_error(UParser *p, UParseError code, const char *msg,
                     int line, int col);
/* expect: urbi_parse_peek next token; if it matches type, urbi_parse_consume and return true.
 * On mismatch, write *err = urbi_parse_make_error(p, code, urbi_parse_kErrorMessages[code], ...)
 * and return false.  On OOM urbi_parse_make_error returns NULL; callers propagate via
 * `return *err` which yields NULL (OOM sentinel path).
 * static inline: internal helper, must not export an archive symbol. */
static inline bool expect(UParser *p, UTokenType type, UParseError code,
                          UAstNode **err) {
    UToken tok = urbi_parse_peek(p);
    if (tok.type != type) {
        *err = urbi_parse_make_error(p, code, urbi_parse_kErrorMessages[code], tok.line, tok.col);
        return false;
    }
    urbi_parse_consume(p);
    return true;
}

/* --- Arena-array growth helper + expression parser (defined in uparse_expr.c). --- */
bool urbi_parse_arena_grow_node_array(UParser *p, UAstNode ***arr, int *cap, int count);
UAstNode *urbi_parse_make_nil_node(UParser *p, int line, int col);
UAstNode *urbi_parse_expression_cont(UParser *p, UAstNode *lhs, int min_prec);
UAstNode *urbi_parse_expression(UParser *p, int min_prec);
UAstNode *urbi_parse_prefix(UParser *p);
UAstNode *urbi_parse_atom(UParser *p);

/* --- Separator loop (defined in uparse_separators.c). --- */
/* urbi_parse_pipe_amp_fold: left-fold `|` / `&` from an already-parsed lhs.
 * W8/v0.10.5: promoted from static to allow parse_assign_or_expr to call
 * it directly after intercepting the member-expr tag-prefix form. */
UAstNode *urbi_parse_pipe_amp_fold(UParser *p, UAstNode *lhs);
UAstNode *urbi_parse_inner_tier(UParser *p);
UAstNode *urbi_parse_outer_tier(UParser *p);

/* --- Statement parser (defined in uparse_stmt.c). --- */
UAstNode *urbi_parse_statement_or_expr(UParser *p);
UAstNode *urbi_parse_block(UParser *p);
UAstNode *urbi_parse_if(UParser *p);
UAstNode *urbi_parse_while(UParser *p);
UAstNode *urbi_parse_function(UParser *p);
UAstNode *urbi_parse_throw(UParser *p);
UAstNode *urbi_parse_try(UParser *p);
/* W3/v0.10.5: assert keyword — urbi_parse_assert handles both paren and block forms. */
UAstNode *urbi_parse_assert(UParser *p);
/* T41 — property declaration helper.  `recv` is the explicit receiver
 * (or NULL for class-body / implicit self).  `name_tok` is the slot-
 * name IDENT (already consumed by the caller).  The next token must be
 * `(` — this helper parses params, body, and builds AST_PROPERTY_DECL.
 * Caller is responsible for verifying the `get`/`set` lookahead pattern
 * via urbi_parse_peek+urbi_parse_peek2 and consuming the leading IDENT (`get`/`set`). */
UAstNode *urbi_parse_property_decl(UParser *p, UAstNode *recv, UToken name_tok,
                              UAstMethodKind kind, int line, int col);

/* --- Reactive parser (defined in uparse_react.c). --- */
UAstNode *urbi_parse_desugar_postfix_emit(UParser *p, UAstNode *recv, UToken bang_tok);
UAstNode *urbi_parse_at(UParser *p);
UAstNode *urbi_parse_whenever(UParser *p);
UAstNode *urbi_parse_waituntil(UParser *p);
UAstNode *urbi_parse_every(UParser *p);
UAstNode *urbi_parse_tag_prefix(UParser *p, UToken name_tok);
/* W8/v0.10.5: member-expr tag form `expr: body` — called when a postfix
 * chain ends in `:` at statement level.  `:` not yet consumed. */
UAstNode *urbi_parse_tag_prefix_from_expr(UParser *p, UAstNode *tag_expr);

#endif /* UPARSE_INTERNAL_H */
