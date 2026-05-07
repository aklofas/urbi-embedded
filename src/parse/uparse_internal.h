/* SPDX-License-Identifier: BSD-3-Clause */
/* uparse_internal.h — private inter-TU API for the parse subsystem.
 * Consumed only by src/parse/ translation units. */

#ifndef UPARSE_INTERNAL_H
#define UPARSE_INTERNAL_H

#include "uparse.h"
#include "uast.h"

/* --- OOM sentinel (defined in uparse.c residual). --- */
extern const UAstNode uparser_oom_sentinel;

/* --- Error-message table (defined in uparse.c residual). --- */
extern const char * const kErrorMessages[];

/* --- Token helpers (defined in uparse.c residual). --- */
UToken peek(UParser *p);
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
UAstNode *parse_outer_tier(UParser *p);

/* --- Statement parser (defined in uparse_stmt.c). --- */
UAstNode *parse_statement_or_expr(UParser *p);
UAstNode *parse_block(UParser *p);
UAstNode *parse_var_decl(UParser *p);
UAstNode *parse_assign_from_ident(UParser *p, UToken name);
UAstNode *parse_if(UParser *p);
UAstNode *parse_while(UParser *p);
UAstNode *parse_function(UParser *p);
UAstNode *parse_return(UParser *p);
UAstNode *parse_throw(UParser *p);
UAstNode *parse_try(UParser *p);

/* --- Reactive parser (defined in uparse_react.c). --- */
UAstNode *desugar_postfix_emit(UParser *p, UAstNode *recv, UToken bang_tok);
UAstNode *parse_at(UParser *p);
UAstNode *parse_whenever(UParser *p);
UAstNode *parse_waituntil(UParser *p);
UAstNode *parse_tag_prefix(UParser *p, UToken name_tok);

/* --- Entry points + recovery (defined in uparse_top.c). --- */
void sync_to_statement_boundary(UParser *p);

#endif /* UPARSE_INTERNAL_H */
