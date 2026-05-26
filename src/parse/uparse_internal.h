/* SPDX-License-Identifier: BSD-3-Clause */
/* uparse_internal.h — private inter-TU API for the parse subsystem.
 * Consumed only by src/parse/ translation units. */

#ifndef UPARSE_INTERNAL_H
#define UPARSE_INTERNAL_H

#include "uparse.h"
#include "uast.h"

/* OOM sentinel convention (closes PARSE-003 / PARSE-005):
 *
 * Internal AST allocators (make_node and friends) return NULL on
 * arena OOM. Pratt/recursive-descent parsers propagate NULL upward
 * unchanged; the top-level entrypoint (uparse_next_statement in
 * uparse_top.c) converts NULL → uparser_oom_sentinel at its single
 * exit by re-checking p->arena->oom. Sibling parsers MUST NOT
 * pre-convert NULL → sentinel within the recursion (avoids
 * double-reporting and keeps the OOM surface single-sourced).
 *
 * Historical note: a few interior sites currently return the sentinel
 * directly (e.g. parse_outer_tier on arena_grow_node_array failure).
 * This is harmless — the top-level recheck collapses both forms — but
 * standardizing on plain NULL inside the recursion is a Wave 5 fix
 * target.  New code should follow the NULL-propagation rule. */

/* --- OOM sentinel (defined in uparse.c residual). --- */
extern const UAstNode uparser_oom_sentinel;

/* Postfix precedence level — `expr(args)`, `expr.x`, `expr->x`,
 * `expr!`, `expr?` all bind tighter than any infix operator
 * (multiplicative=6 is the tightest infix; postfix=7 sits above).
 * Used as the `min_prec <=` ceiling check in parse_expression_cont. */
#define PARSE_PREC_POSTFIX 7

/* Method name used by the postfix `e!` desugar (`e!` → `e.emit()`).
 * Single source of truth, referenced by AST_MEMBER_GET node construction
 * in uparse_react.c.  Defined in uparse.c.  Length declared explicitly
 * so callers can use kEmitMethodNameLen without strlen at runtime
 * (the array's `extern char kEmitMethodName[];` form has incomplete
 * type at the use site, so sizeof is unavailable). */
extern const char kEmitMethodName[];
#define kEmitMethodNameLen 4  /* strlen("emit") */

/* --- Error-message table (defined in uparse.c residual). --- */
extern const char * const kErrorMessages[];

/* --- Token helpers (defined in uparse.c residual). --- */
UToken peek(UParser *p);
UToken peek2(UParser *p);
UToken consume(UParser *p);

/* --- Identifier comparison (defined in uparse.c residual). --- */
int ident_equals(const char *bytes, int len, const char *literal, int lit_len);

/* --- AST constructors (defined in uparse.c residual). --- */
UAstNode *make_node(UParser *p, UAstKind k, int line, int col);
UAstNode *make_int(UParser *p, int64_t v, int line, int col);
UAstNode *make_ident(UParser *p, const char *start, int len, int line, int col);
UAstNode *make_unary(UParser *p, UAstUnaryOp op, UAstNode *operand,
                     int line, int col);
UAstNode *make_binary(UParser *p, UAstBinaryOp op, UAstNode *lhs, UAstNode *rhs,
                      int line, int col);
UAstNode *make_error(UParser *p, UParseError code, const char *msg,
                     int line, int col);

/* --- Arena-array growth helper + expression parser (defined in uparse_expr.c). --- */
bool arena_grow_node_array(UParser *p, UAstNode ***arr, int *cap, int count);
UAstNode *make_compare(UParser *p, UAstCompareOp op, UAstNode *lhs, UAstNode *rhs,
                       int line, int col);
UAstNode *make_bool_node(UParser *p, bool value, int line, int col);
UAstNode *make_nil_node(UParser *p, int line, int col);
UAstNode *make_this_node(UParser *p, int line, int col);
UAstNode *parse_expression_cont(UParser *p, UAstNode *lhs, int min_prec);
UAstNode *parse_expression(UParser *p, int min_prec);
UAstNode *parse_prefix(UParser *p);
UAstNode *parse_atom(UParser *p);
UAstNode *parse_call_args(UParser *p, UAstNode *callee);
UAstNode *parse_member_access(UParser *p, UAstNode *recv, bool *out_is_assign);
int infix_prec(UTokenType t);
UAstBinaryOp infix_binop(UTokenType t);
bool is_compare_token(UTokenType t);
UAstCompareOp compare_op(UTokenType t);

/* --- Separator loop (defined in uparse_separators.c). --- */
bool at_statement_end(UParser *p);
UAstNode *parse_inner_tier(UParser *p);
UAstNode *parse_inner_tier_from_lhs(UParser *p, UAstNode *lhs);
UAstNode *parse_outer_tier(UParser *p);

/* --- Statement parser (defined in uparse_stmt.c). --- */
UAstNode *parse_statement_or_expr(UParser *p);
UAstNode *parse_block(UParser *p);
UAstNode *parse_var_decl(UParser *p);
UAstNode *parse_assign_after_eq_peek(UParser *p, UToken name);
UAstNode *parse_if(UParser *p);
UAstNode *parse_while(UParser *p);
UAstNode *parse_function(UParser *p);
UAstNode *parse_return(UParser *p);
UAstNode *parse_throw(UParser *p);
UAstNode *parse_try(UParser *p);
UAstNode *parse_class_declaration(UParser *p);
/* W3/v0.10.5: assert keyword — parse_assert handles both paren and block forms. */
UAstNode *parse_assert(UParser *p);
/* T41 — property declaration helper.  `recv` is the explicit receiver
 * (or NULL for class-body / implicit self).  `name_tok` is the slot-
 * name IDENT (already consumed by the caller).  The next token must be
 * `(` — this helper parses params, body, and builds AST_PROPERTY_DECL.
 * Caller is responsible for verifying the `get`/`set` lookahead pattern
 * via peek+peek2 and consuming the leading IDENT (`get`/`set`). */
UAstNode *parse_property_decl(UParser *p, UAstNode *recv, UToken name_tok,
                              UAstMethodKind kind, int line, int col);

/* --- Reactive parser (defined in uparse_react.c). --- */
UAstNode *desugar_postfix_emit(UParser *p, UAstNode *recv, UToken bang_tok);
UAstNode *parse_at(UParser *p);
UAstNode *parse_whenever(UParser *p);
UAstNode *parse_waituntil(UParser *p);
UAstNode *parse_every(UParser *p);
UAstNode *parse_tag_prefix(UParser *p, UToken name_tok);

/* --- Entry points + recovery (defined in uparse_top.c). --- */
void sync_to_statement_boundary(UParser *p);

#endif /* UPARSE_INTERNAL_H */
