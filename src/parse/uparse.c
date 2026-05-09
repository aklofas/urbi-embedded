/* SPDX-License-Identifier: BSD-3-Clause */
/* Streaming Pratt parser implementation. */

#include "parse/uparse.h"
#include "parse/uparse_internal.h"
#include <stddef.h>
#include "lex/ulex.h"
#include "parse/uast.h"
#include "value/uarena.h"
#include <stdint.h>

/* Local string helper — compare an (unterminated) lexeme against a literal.
 * Returns non-zero when bytes[0..len) == literal (all ASCII, no NUL in bytes). */
int ident_equals(const char *bytes, int len, const char *literal, int lit_len) {
    if (len != lit_len) return 0;
    int i;
    for (i = 0; i < len; i++) {
        if (bytes[i] != literal[i]) return 0;
    }
    return 1;
}

/* --- Static error-message table.  Indices must match UParseError. --- */

const char * const kErrorMessages[] = {
    "ok",
    "unexpected token",
    "unexpected end of input",
    "expected expression",
    "expected ')'",
    "lex error",                    /* overridden by pass-through lexer message */
    "out of memory during parsing",
    "expected '}'",
    "expected '{'",
    "expected '('",
    "expected identifier",
    "expected '='",
    "expected ';', '|', or end of statement",
    "bare 'function { body }' is retired at v1.0; use 'function() { body }' (add parens)",
    "the 'closure' keyword is retired at v1.0; use 'function' instead. MIGRATION TRAP: 'closure' bound 'this' lexically; 'function' binds at call site. See REVIVAL §14 L14",
    "trailing '&' is illegal",
    "'lazy' keyword only allowed in parameter lists",
    "'try' requires at least one of 'catch' or 'finally'",
    "reserved keyword used as variable name (M5 reactive runtime); rename the variable",
    "postfix '?' is only valid inside at(...); use 'at (e?) body' for event-subscribe",
    "multi-arg e!(x, y, z) is reserved for M6 (UList auto-boxing); use e!(x) with one arg",
    "bare '.changed' outside at(...) is a slot-change event; use: at (obj.x.changed?) body",
    "slot-change event cannot be emitted; use slot assignment to trigger subscribers",
    "named-function declarations are not supported at v1.0; use 'var name = function(...){...}'",
    "'onleave' is not allowed with 'at sync' — at sync has no leave edge; use 'at (cond) body onleave handler'"
};

static const char * const kErrorNames[] = {
    "PARSE_OK",
    "PARSE_UNEXPECTED_TOKEN",
    "PARSE_UNEXPECTED_EOF",
    "PARSE_EXPECTED_EXPRESSION",
    "PARSE_EXPECTED_RPAREN",
    "PARSE_LEX_ERROR",
    "PARSE_OOM",
    "PARSE_EXPECTED_RBRACE",
    "PARSE_EXPECTED_LBRACE",
    "PARSE_EXPECTED_LPAREN",
    "PARSE_EXPECTED_IDENT",
    "PARSE_EXPECTED_EQ",
    "PARSE_EXPECTED_SEMI_OR_PIPE",
    "PARSE_BARE_FUNCTION",
    "PARSE_CLOSURE_KEYWORD",
    "PARSE_TRAILING_AMP",
    "PARSE_LAZY_OUT_OF_PARAM_LIST",
    "PARSE_TRY_NEEDS_CATCH_OR_FINALLY",
    "PARSE_RESERVED_KEYWORD_AS_IDENT",
    "PARSE_QUESTION_OUTSIDE_AT",
    "PARSE_EMIT_MULTI_ARG_V1",
    "PARSE_SLOT_CHANGED_BARE_V1",
    "PARSE_SLOT_CHANGED_EMIT_V1",
    "PARSE_NAMED_FUNCTION_NOT_SUPPORTED",
    "PARSE_AT_SYNC_DOES_NOT_SUPPORT_ONLEAVE"
};

#define N_PARSE_ERROR_CODES ((int)(sizeof kErrorNames / sizeof kErrorNames[0]))

/* Compile-time parity check: the kErrorNames / kErrorMessages tables
 * are indexed by UParseError, so their length must equal the count of
 * UParseError enumerators.  PARSE_SLOT_CHANGED_EMIT_V1 is the last
 * enumerator (added in M5 spec #4); update both forms together when
 * adding a new code.  Closes PARSE-017. */
_Static_assert(N_PARSE_ERROR_CODES == (int)PARSE_AT_SYNC_DOES_NOT_SUPPORT_ONLEAVE + 1,
               "kErrorNames length must match UParseError enum count");
_Static_assert((int)(sizeof kErrorMessages / sizeof kErrorMessages[0])
               == (int)PARSE_AT_SYNC_DOES_NOT_SUPPORT_ONLEAVE + 1,
               "kErrorMessages length must match UParseError enum count");

/* --- Postfix-emit method name.  Promoted to file scope so the postfix
 * `e!` desugar in uparse_react.c does not duplicate the literal.
 * Closes PARSE-016. --- */
const char kEmitMethodName[] = "emit";
_Static_assert(sizeof kEmitMethodName - 1U == kEmitMethodNameLen,
               "kEmitMethodNameLen must equal strlen(kEmitMethodName)");

/* --- OOM sentinel.  Returned whenever the arena is in OOM state. --- */

/* Read-only OOM error sentinel returned by parse functions when arena
 * allocation fails. Declared `static const` to satisfy the per-VM
 * audit (see tools/audit-globals.sh + pre-M2 multi-VM-audit spec):
 * functionally immutable, but the public AST API uses `UAstNode *`
 * (non-const), so callers cast away const at return sites. The cast
 * is safe because the sentinel is never mutated by anyone — its
 * contents are inspected only via the const-correct read path
 * (kind == AST_ERROR && u.err.code == PARSE_OOM). */
const UAstNode uparser_oom_sentinel = {
    AST_ERROR,
    0,
    0,
    { .err = { PARSE_OOM, "out of memory during parsing" } }
};

/* --- ULexer lookahead helpers. --- */

UToken peek(UParser *p) {
    if (!p->have_peek) {
        p->peek = ulex_next(p->lex);
        p->have_peek = true;
    }
    return p->peek;
}

UToken consume(UParser *p) {
    UToken t = peek(p);
    p->have_peek = false;
    return t;
}

/* --- AST constructors.  Return NULL on arena OOM. --- */

UAstNode *make_node(UParser *p, UAstKind k, int line, int col) {
    UAstNode *n = uarena_alloc(p->arena, sizeof *n);
    if (!n) return NULL;
    n->kind = k;
    n->line = line;
    n->col = col;
    return n;
}

UAstNode *make_int(UParser *p, int64_t v, int line, int col) {
    UAstNode *n = make_node(p, AST_INT, line, col);
    if (!n) return NULL;
    n->u.i = v;
    return n;
}

UAstNode *make_ident(UParser *p, const char *start, int len, int line, int col) {
    UAstNode *n = make_node(p, AST_IDENT, line, col);
    if (!n) return NULL;
    n->u.ident.start = start;
    n->u.ident.len = len;
    return n;
}

UAstNode *make_unary(UParser *p, UAstUnaryOp op, UAstNode *operand,
                     int line, int col) {
    UAstNode *n = make_node(p, AST_UNARY, line, col);
    if (!n) return NULL;
    n->u.unary.op = op;
    n->u.unary.operand = operand;
    return n;
}

UAstNode *make_binary(UParser *p, UAstBinaryOp op, UAstNode *lhs, UAstNode *rhs,
                      int line, int col) {
    UAstNode *n = make_node(p, AST_BINARY, line, col);
    if (!n) return NULL;
    n->u.binary.op = op;
    n->u.binary.lhs = lhs;
    n->u.binary.rhs = rhs;
    return n;
}

/* make_error: build a UParseError record from an error code + line/col.
 *
 * msg parameter convention:
 *   - NULL → look up message text from kErrorMessages[code].  Used by
 *            error sites that report a bare code with no contextual prose.
 *   - non-NULL → use the caller-provided message verbatim.  Used by
 *            error sites that need to interpolate context not encoded in
 *            the bare error code (e.g. "expected ')' after ',' at column 17").
 *
 * Call-site convention is inconsistent across parse TUs: some pass NULL,
 * others pass kErrorMessages[code] explicitly even when the value is
 * the same.  Both patterns are supported and produce identical output;
 * standardization across the parse subsystem is a Wave 6 cleanup target. */
UAstNode *make_error(UParser *p, UParseError code, const char *msg,
                     int line, int col) {
    UAstNode *n = make_node(p, AST_ERROR, line, col);
    if (!n) return NULL;
    n->u.err.code = (int)code;
    n->u.err.message = msg ? msg : kErrorMessages[code];
    return n;
}

/* infix_prec / infix_binop / is_compare_token / compare_op /
   make_compare / make_bool_node / make_nil_node /
   parse_prefix / parse_atom / arena_grow_node_array /
   parse_call_args / parse_member_access / parse_expression
   moved to uparse_expr.c (PARSE-021 #6). */

/* at_statement_end / parse_inner_tier moved to uparse_separators.c. */

/* parse_block / parse_while / parse_if / parse_function / parse_return /
   parse_throw / parse_try moved to uparse_stmt.c (PARSE-021 #4). */

/* parse_at / parse_whenever / parse_waituntil moved to uparse_react.c
   (PARSE-021 #5). */

/* parse_outer_tier moved to uparse_separators.c. */

/* --- Public API (moved to uparse_top.c). --- */

const char *uparse_error_name(UParseError code) {
    int i = (int)code;
    if (i < 0 || i >= N_PARSE_ERROR_CODES) return "PARSE_UNKNOWN";
    return kErrorNames[i];
}
