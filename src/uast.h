/* SPDX-License-Identifier: BSD-3-Clause */
/* AST node types shared between parser, desugar, and emit. */

#ifndef UAST_H
#define UAST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Node kinds for M1 and M2. */
typedef enum {
    /* M1 — atomic + arithmetic */
    AST_INT     = 0,
    AST_IDENT   = 1,
    AST_UNARY   = 2,
    AST_BINARY  = 3,
    AST_ERROR   = 4,

    /* M2 — literal extensions */
    AST_BOOL    = 5,
    AST_NIL     = 6,

    /* M2 — separators */
    AST_NARY    = 7,        /* outer-tier: ;-or-,-joined sequence */
    AST_BIN_SEP = 8,        /* inner-tier: |-or-&-joined pair    */
    AST_NOOP    = 9,        /* singleton; legacy compat (see separator spec §3) */

    /* M2 — declarations + scope */
    AST_VAR_DECL  = 10,     /* var x = expr; locals registered in FuncState */
    AST_LOCAL_REF = 11,     /* resolved local reference (parser produces AST_IDENT;
                               emit converts to AST_LOCAL_REF after FuncState lookup) */
    AST_BLOCK     = 12,     /* { stmt; stmt; ... } */

    /* M2 — control flow */
    AST_IF      = 13,       /* if (cond) then-block [else else-block] */
    AST_WHILE   = 14,       /* while (cond) body */
    AST_COMPARE = 15,       /* ==, !=, <, <=, >, >= */

    /* M2 — functions */
    AST_FUNCTION   = 16,    /* function (params) { body } */
    AST_CALL       = 17,    /* callee(args) */
    AST_RETURN     = 18,    /* return [expr] */
    AST_PARAM      = 19,    /* formal parameter (eager, no `lazy`) */
    AST_LAZY_PARAM = 20,    /* formal parameter (`lazy x`) */

    /* M2 — assignment */
    AST_ASSIGN     = 21,    /* x = expr; assignment to existing local/upvalue */

    /* M3 — control transfer */
    AST_TRY        = 22,    /* try { body } [catch (e) { handler }] [finally { cleanup }] */
    AST_THROW      = 23     /* throw expr */
} UAstKind;

typedef enum {
    UOP_NEG = 0,
    UOP_NOT = 1            /* logical not — reserved at M2; not yet emitted */
} UAstUnaryOp;

typedef enum {
    BOP_ADD = 0,
    BOP_SUB,
    BOP_MUL,
    BOP_DIV
} UAstBinaryOp;

typedef enum {
    SEP_PIPE = 0,           /* | inner-tier */
    SEP_AMP  = 1,           /* & inner-tier */
    SEP_SEMI = 2,           /* ; outer-tier */
    SEP_COMMA = 3           /* , outer-tier */
} UAstSeparator;

typedef enum {
    CMP_EQ = 0,             /* == */
    CMP_NEQ,                /* != */
    CMP_LT,                 /* <  */
    CMP_LE,                 /* <= */
    CMP_GT,                 /* >  */
    CMP_GE                  /* >= */
} UAstCompareOp;

typedef enum {
    PARSE_OK = 0,                  /* sentinel */
    PARSE_UNEXPECTED_TOKEN,
    PARSE_UNEXPECTED_EOF,
    PARSE_EXPECTED_EXPRESSION,
    PARSE_EXPECTED_RPAREN,
    PARSE_LEX_ERROR,
    PARSE_OOM,

    /* M2 additions */
    PARSE_EXPECTED_RBRACE,
    PARSE_EXPECTED_LBRACE,
    PARSE_EXPECTED_LPAREN,
    PARSE_EXPECTED_IDENT,
    PARSE_EXPECTED_EQ,
    PARSE_EXPECTED_SEMI_OR_PIPE,
    PARSE_BARE_FUNCTION,           /* `function name {` without parens */
    PARSE_CLOSURE_KEYWORD,         /* `closure(x){...}` form */
    PARSE_TRAILING_AMP,            /* `expr &` is illegal */
    PARSE_LAZY_OUT_OF_PARAM_LIST,
    PARSE_LAZY_PARAM_DEFAULT,      /* `lazy x = ...` reserved syntax */

    /* M3 additions */
    PARSE_TRY_NEEDS_CATCH_OR_FINALLY  /* `try { }` with neither catch nor finally */
} UParseError;

/*
 * UAstNode — tagged union, arena-allocated by the parser.
 *
 * Lifetime: the `ident.start` pointer is a non-owning view into the
 * caller's source buffer, which MUST outlive the AST.  `err.message`
 * points into a compile-time string table and is always valid.
 *
 * Union invariants (active member per kind):
 *   u.i           — AST_INT:        parsed integer value
 *   u.ident       — AST_IDENT:      zero-copy lexeme view
 *   u.unary       — AST_UNARY:      prefix operator + operand pointer
 *   u.binary      — AST_BINARY:     infix operator + two operand pointers
 *   u.err         — AST_ERROR:      UParseError + static message string
 *   u.b           — AST_BOOL:       boolean value
 *   [none]        — AST_NIL:        no payload (sentinel type)
 *   u.nary        — AST_NARY:       separator + ordered array of children
 *   u.bin_sep     — AST_BIN_SEP:    binary separator node
 *   [none]        — AST_NOOP:       no payload (identity; legacy compat)
 *   u.var_decl    — AST_VAR_DECL:   variable declaration with init
 *   u.local_ref   — AST_LOCAL_REF:  resolved local binding
 *   u.block       — AST_BLOCK:      scoped sequence of statements
 *   u.if_stmt     — AST_IF:         conditional statement
 *   u.while_stmt  — AST_WHILE:      iterative loop
 *   u.cmp         — AST_COMPARE:    comparison operator
 *   u.func        — AST_FUNCTION:   function definition
 *   u.call        — AST_CALL:       function application
 *   u.ret         — AST_RETURN:     early exit with value
 *   u.param       — AST_PARAM, AST_LAZY_PARAM: formal parameters
 *   u.assign      — AST_ASSIGN: assignment to existing local/upvalue
 *   u.try_stmt    — AST_TRY:    try body + optional catch/finally
 *   u.throw_expr  — AST_THROW:  value expression to throw
 *
 * Position fields line/col are 1-based, matching the lexer.  For
 * AST_BINARY the position points at the operator token; for AST_ERROR
 * the position points at the detection site.
 */
typedef struct UAstNode UAstNode;
struct UAstNode {
    UAstKind kind;
    int line;
    int col;
    union {
        int64_t i;                                          /* AST_INT */
        struct {                                            /* AST_IDENT */
            const char *start;
            int len;
        } ident;
        struct {                                            /* AST_UNARY */
            UAstUnaryOp op;
            UAstNode *operand;
        } unary;
        struct {                                            /* AST_BINARY */
            UAstBinaryOp op;
            UAstNode *lhs;
            UAstNode *rhs;
        } binary;
        struct {                                            /* AST_ERROR */
            int code;
            const char *message;
        } err;

        bool b;                                             /* AST_BOOL */
        /* AST_NIL has no payload */
        /* AST_NOOP has no payload (singleton in arena) */

        struct {                                            /* AST_NARY */
            UAstSeparator separator;        /* SEP_SEMI or SEP_COMMA */
            UAstNode    **children;         /* arena array */
            int           count;
        } nary;
        struct {                                            /* AST_BIN_SEP */
            UAstSeparator separator;        /* SEP_PIPE or SEP_AMP */
            UAstNode    *lhs;
            UAstNode    *rhs;
        } bin_sep;
        struct {                                            /* AST_VAR_DECL */
            const char *name_start;        /* zero-copy lexeme view */
            int         name_len;
            UAstNode   *init;              /* may be NULL — `var x;` not legal at v1.0 */
        } var_decl;
        struct {                                            /* AST_LOCAL_REF */
            const char *name_start;        /* zero-copy lexeme view */
            int         name_len;
            int         slot;              /* set by FuncState resolver; -1 = unresolved */
        } local_ref;
        struct {                                            /* AST_BLOCK */
            UAstNode  **stmts;
            int         count;
        } block;
        struct {                                            /* AST_IF */
            UAstNode *cond;
            UAstNode *then_block;          /* AST_BLOCK */
            UAstNode *else_block;          /* AST_BLOCK or NULL */
        } if_stmt;
        struct {                                            /* AST_WHILE */
            UAstNode *cond;
            UAstNode *body;                /* AST_BLOCK */
        } while_stmt;
        struct {                                            /* AST_COMPARE */
            UAstCompareOp op;
            UAstNode *lhs;
            UAstNode *rhs;
        } cmp;
        struct {                                            /* AST_FUNCTION */
            UAstNode  **params;            /* AST_PARAM or AST_LAZY_PARAM */
            int         param_count;
            UAstNode   *body;              /* AST_BLOCK */
        } func;
        struct {                                            /* AST_CALL */
            UAstNode  *callee;
            UAstNode **args;
            int        arg_count;
        } call;
        struct {                                            /* AST_RETURN */
            UAstNode *value;               /* may be NULL — `return;` returns void */
        } ret;
        struct {                                            /* AST_PARAM, AST_LAZY_PARAM */
            const char *name_start;
            int         name_len;
        } param;
        struct {                                            /* AST_ASSIGN */
            const char *name_start;        /* zero-copy lexeme view */
            int         name_len;
            UAstNode   *value;
        } assign;
        struct {                                            /* AST_TRY */
            UAstNode   *body;              /* AST_BLOCK — the try body */
            /* catch clause — both NULL when no catch */
            const char *catch_var_start;   /* zero-copy catch variable name */
            int         catch_var_len;
            UAstNode   *catch_body;        /* AST_BLOCK or NULL */
            /* finally clause */
            UAstNode   *finally_body;      /* AST_BLOCK or NULL */
        } try_stmt;
        struct {                                            /* AST_THROW */
            UAstNode   *value;             /* expression to throw */
        } throw_expr;
    } u;
};

#ifdef __cplusplus
}
#endif

#endif
