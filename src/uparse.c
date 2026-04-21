/* SPDX-License-Identifier: BSD-3-Clause */
/* Streaming Pratt parser implementation. */

#include "uparse.h"
#include <stddef.h>

/* --- Static error-message table.  Indices must match ParseErrorCode. --- */

static const char * const kErrorMessages[] = {
    "ok",
    "unexpected token; expected '|' or end of input",
    "unexpected end of input",
    "expected expression",
    "expected ')'",
    "lex error",                    /* overridden by pass-through lexer message */
    "out of memory during parsing"
};

static const char * const kErrorNames[] = {
    "PARSE_OK",
    "PARSE_UNEXPECTED_TOKEN",
    "PARSE_UNEXPECTED_EOF",
    "PARSE_EXPECTED_EXPRESSION",
    "PARSE_EXPECTED_RPAREN",
    "PARSE_LEX_ERROR",
    "PARSE_OOM"
};

#define N_PARSE_ERROR_CODES ((int)(sizeof kErrorNames / sizeof kErrorNames[0]))

/* --- OOM sentinel.  Returned whenever the arena is in OOM state. --- */

static AstNode uparser_oom_sentinel = {
    AST_ERROR,
    0,
    0,
    { .err = { PARSE_OOM, "out of memory during parsing" } }
};

/* --- Public API. --- */

void uparse_init(Parser *p, Lexer *lex, Arena *arena) {
    p->lex = lex;
    p->arena = arena;
    p->have_peek = false;
}

AstNode *uparse_next_statement(Parser *p) {
    /* Placeholder — Task 7 onward wires this to the real parse pipeline. */
    (void)p;
    return NULL;
}

const char *uparse_error_name(ParseErrorCode code) {
    int i = (int)code;
    if (i < 0 || i >= N_PARSE_ERROR_CODES) return "PARSE_UNKNOWN";
    return kErrorNames[i];
}
