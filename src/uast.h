/* SPDX-License-Identifier: BSD-3-Clause */
/* AST node types shared between parser, desugar, and emit. */

#ifndef UAST_H
#define UAST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Five node kinds for the M1 walking-skeleton parser. */
typedef enum {
    AST_INT = 0,
    AST_IDENT,
    AST_UNARY,
    AST_BINARY,
    AST_ERROR
} UAstKind;

typedef enum {
    UOP_NEG = 0
} UAstUnaryOp;

typedef enum {
    BOP_ADD = 0,
    BOP_SUB,
    BOP_MUL,
    BOP_DIV
} UAstBinaryOp;

typedef enum {
    PARSE_OK = 0,                  /* sentinel; never emitted */
    PARSE_UNEXPECTED_TOKEN,
    PARSE_UNEXPECTED_EOF,
    PARSE_EXPECTED_EXPRESSION,
    PARSE_EXPECTED_RPAREN,
    PARSE_LEX_ERROR,
    PARSE_OOM
} UParseError;

/*
 * UAstNode — tagged union, arena-allocated by the parser.
 *
 * Lifetime: the `ident.start` pointer is a non-owning view into the
 * caller's source buffer, which MUST outlive the AST.  `err.message`
 * points into a compile-time string table and is always valid.
 *
 * Union invariants (active member per kind):
 *   u.i      — AST_INT:    parsed integer value
 *   u.ident  — AST_IDENT:  zero-copy lexeme view
 *   u.unary  — AST_UNARY:  prefix operator + operand pointer
 *   u.binary — AST_BINARY: infix operator + two operand pointers
 *   u.err    — AST_ERROR:  UParseError + static message string
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
        int64_t i;
        struct {
            const char *start;
            int len;
        } ident;
        struct {
            UAstUnaryOp op;
            UAstNode *operand;
        } unary;
        struct {
            UAstBinaryOp op;
            UAstNode *lhs;
            UAstNode *rhs;
        } binary;
        struct {
            int code;
            const char *message;
        } err;
    } u;
};

#ifdef __cplusplus
}
#endif

#endif
