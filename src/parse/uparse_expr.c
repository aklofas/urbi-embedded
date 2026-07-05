/* SPDX-License-Identifier: BSD-3-Clause */
/* uparse_expr.c — Pratt expression parser (infix tables, prefix, atom, call,
 * member-access, parse_expression).
 * Extracted from uparse.c during v0.5.4-decompose (PARSE-021 #6). */

#include "parse/uparse.h"
#include "parse/uparse_internal.h"
#include "lex/ulex.h"
#include "lex/ulex_internal.h"
#include "parse/uast.h"
#include "value/uarena.h"
#include <stddef.h>
#include <stdint.h>

/* v0.10.11 / W3: `<<` shift-write selector.  File-scope (with
 * URBI_STATIC_ASSERT length guard) matching the kEmitMethodName pattern
 * in uparse.c; declared extern in uparse_internal.h for visibility. */
const char kLShiftSelector[] = "<<";
URBI_STATIC_ASSERT(sizeof kLShiftSelector - 1U == kLShiftSelectorLen,
               "kLShiftSelectorLen must equal strlen(kLShiftSelector)");

/* v1.0-rc stdlib-completeness: `%` modulo selector.  Like `<<`, `%`
 * desugars to a method call `lhs.'%'(rhs)` rather than a dedicated opcode;
 * the `'%'` slot lives on Integer/Float (added by the atoms stdlib). */
static const char kModSelector[] = "%";
#define kModSelectorLen 1  /* strlen("%") */
URBI_STATIC_ASSERT(sizeof kModSelector - 1U == (size_t)kModSelectorLen,
               "kModSelectorLen must equal strlen(kModSelector)");

/* Return the left-binding precedence of an infix token, or 0 if not
   an infix operator (terminates the Pratt climb).
   Logical operators bind loosest; comparison binds looser than arithmetic:
     1 = logical OR  (||)
     2 = logical AND (&&)
     3 = streaming / write (<<)
     4 = equality (==, !=)
     5 = relational (<, <=, >, >=)
     6 = additive (+, -)
     7 = multiplicative (*, /, %)
     9 = postfix (call, member, `!`, `?`) — see PARSE_PREC_POSTFIX
         in uparse_internal.h; not produced by this function (handled
         directly in parse_expression_cont). */
int infix_prec(UTokenType t) {
    switch (t) {
    /* === v1.0-rc stdlib-completeness: short-circuit logical operators === */
    case TOK_PIPEPIPE: return 1;
    case TOK_AMPAMP:   return 2;
    /* === end v1.0-rc stdlib-completeness === */
    /* === W3/v0.10.11: << method-call desugar (below equality) === */
    case TOK_LSHIFT: return 3;
    /* === end W3/v0.10.11 === */
    case TOK_EQEQ:
    case TOK_NEQ:   return 4;
    case TOK_LT:
    case TOK_LE:
    case TOK_GT:
    case TOK_GE:    return 5;
    case TOK_PLUS:
    case TOK_MINUS: return 6;
    case TOK_STAR:
    case TOK_SLASH:
    /* === v1.0-rc stdlib-completeness: % at multiplicative level === */
    case TOK_PERCENT: return 7;
    /* === end v1.0-rc stdlib-completeness === */
    default:        return 0;
    }
}

UAstBinaryOp infix_binop(UTokenType t) {
    switch (t) {
    case TOK_PLUS:  return BOP_ADD;
    case TOK_MINUS: return BOP_SUB;
    case TOK_STAR:  return BOP_MUL;
    case TOK_SLASH: return BOP_DIV;
    default:        return BOP_ADD; /* unreachable when prec > 0 */
    }
}

/* True when t is a comparison operator token. */
bool is_compare_token(UTokenType t) {
    return t == TOK_EQEQ || t == TOK_NEQ
        || t == TOK_LT   || t == TOK_LE
        || t == TOK_GT   || t == TOK_GE;
}

UAstCompareOp compare_op(UTokenType t) {
    switch (t) {
    case TOK_EQEQ: return CMP_EQ;
    case TOK_NEQ:  return CMP_NEQ;
    case TOK_LT:   return CMP_LT;
    case TOK_LE:   return CMP_LE;
    case TOK_GT:   return CMP_GT;
    case TOK_GE:   return CMP_GE;
    default:       return CMP_EQ; /* unreachable */
    }
}

UAstNode *make_compare(UParser *p, UAstCompareOp op,
                       UAstNode *lhs, UAstNode *rhs,
                       int line, int col) {
    UAstNode *n = make_node(p, AST_COMPARE, line, col);
    if (!n) return NULL;
    n->u.cmp.op  = op;
    n->u.cmp.lhs = lhs;
    n->u.cmp.rhs = rhs;
    return n;
}

UAstNode *make_bool_node(UParser *p, bool value, int line, int col) {
    UAstNode *n = make_node(p, AST_BOOL, line, col);
    if (!n) return NULL;
    n->u.b = value;
    return n;
}

UAstNode *make_nil_node(UParser *p, int line, int col) {
    return make_node(p, AST_NIL, line, col);
}

UAstNode *make_this_node(UParser *p, int line, int col) {
    return make_node(p, AST_THIS, line, col);
}

/* hex_digit_unchecked — convert one ASCII hex digit to its 0..15 value.
 * Caller has already passed the byte through the lexer's
 * validate_unicode_escape pass, which guarantees `c` is in [0-9a-fA-F].
 * No defensive branch — the assumption is load-bearing for the
 * non-failing escape-resolution path in parse_string_literal. */
static int hex_digit_unchecked(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

/* parse_string_literal — consume a TOK_STRING (possibly followed by adjacent
 * TOK_STRING tokens per the L3 adjacent-string-concat rule) and produce AST_STR.
 *
 * Resolves Wave-1 escape sequences (\n / \t / \\ / \") and the v0.6.1
 * Wave-2 \uXXXX / \u{HHHHHH} Unicode escapes into raw bytes (UTF-8 for
 * the latter, via urbi_encode_utf8).  The lexer guarantees only the
 * recognized escape kinds reach the parser (others are already rejected
 * as LEX_INVALID_ESCAPE / LEX_UNICODE_*).  Concatenation is greedy: any
 * number of adjacent TOK_STRING tokens fold into a single AST_STR.
 *
 * The result buffer lives in the parser arena.  Worst-case size is the sum
 * of raw source spans across all concatenated tokens; we allocate that
 * up-front (no growth needed because escape resolution is monotonically
 * non-expansive — every \X is at least 2 source bytes and emits at most
 * 1 byte for the basic forms.  Unicode escapes also stay non-expansive:
 * \uXXXX consumes 6 source bytes and emits at most 3 UTF-8 bytes;
 * \u{HHHHHH} consumes ≥5 source bytes (\u{H}) and emits at most 4 UTF-8
 * bytes (only for code points ≥ U+10000, where the source span is ≥9
 * bytes).  See urbi_encode_utf8 in src/lex/ulex_internal.h.).
 *
 * Caller has already peeked TOK_STRING; this helper consumes it and any
 * adjacent siblings.
 *
 * Returns AST_STR on success, NULL on arena OOM. */
static UAstNode *parse_string_literal(UParser *p) {
    UToken first = consume(p);
    int line = first.line;
    int col = first.col;

    /* Worst-case capacity: sum of all interior spans for first + every
     * adjacent TOK_STRING sibling.  We need to peek ahead to size it before
     * committing the arena allocation; cheaper to allocate per the first
     * token and grow only when concat appends.  Since arena allocations are
     * single-shot (no realloc), we instead measure the total span first by
     * walking the peek + consume loop, but the parser's peek/consume API is
     * one-token lookahead.  Solution: allocate generously for the first
     * token, then reallocate by re-allocating + copy on each concat append.
     * This is O(n^2) in adjacent-concat depth but adjacency is bounded by
     * source size in practice; the arena fast-path handles the realloc.
     *
     * Implementation: we DON'T realloc.  Instead, allocate a fresh wider
     * buffer when concat needs more room and copy from the current one.
     * The discarded buffer space is leaked into the arena but the arena is
     * reset per top-level statement so the total waste is bounded. */
    /* Always allocate at least 1 byte so write paths don't dereference NULL
     * (clang-tidy clang-analyzer-core.NullDereference flags the cap==0
     * branch otherwise; the inner while loop is a no-op for "" so no bytes
     * land in the buffer, but the analyzer can't prove that statically). */
    int cap = first.u.str.len > 0 ? first.u.str.len : 1;
    char *buf = (char *)uarena_alloc(p->arena, (size_t)cap);
    if (buf == NULL) return NULL;
    int len = 0;

    UToken cur = first;
    for (;;) {
        /* Resolve escapes from cur's source span. */
        const char *cs = cur.u.str.start;
        const char *ce = cs + cur.u.str.len;
        while (cs < ce) {
            if (*cs == '\\') {
                cs++;
                if (cs >= ce) break;   /* defensive — lexer rejects mid-EOF */
                char ch;
                switch (*cs) {
                    case 'n':  ch = '\n'; cs++; break;
                    case 't':  ch = '\t'; cs++; break;
                    case '\\': ch = '\\'; cs++; break;
                    case '"':  ch = '"';  cs++; break;
                    case 'u': {
                        /* Lexer's validate_unicode_escape has already
                         * pinned the form (4-hex or {1-6 hex}) and the
                         * surrogate / out-of-range guards.  Re-parse the
                         * hex digits to recover the code point, then
                         * write 1-4 UTF-8 bytes via urbi_encode_utf8. */
                        cs++;   /* past 'u' */
                        uint32_t cp = 0;
                        if (cs < ce && *cs == '{') {
                            cs++;
                            while (cs < ce && *cs != '}') {
                                cp = (cp << 4) | (uint32_t)hex_digit_unchecked(*cs);
                                cs++;
                            }
                            if (cs < ce) cs++;   /* past '}' */
                        } else {
                            for (int i = 0; i < 4 && cs < ce; i++) {
                                cp = (cp << 4) | (uint32_t)hex_digit_unchecked(*cs);
                                cs++;
                            }
                        }
                        unsigned char utf8_buf[4];
                        const int n = urbi_encode_utf8(cp, utf8_buf);
                        for (int i = 0; i < n; i++) {
                            buf[len++] = (char)utf8_buf[i];
                        }
                        continue;   /* skip the trailing buf[len++] = ch path */
                    }
                    default:
                        /* Lexer pre-validates the escape body; this branch
                         * is unreachable.  Fall through to the literal byte
                         * for defensive output. */
                        ch = *cs;
                        cs++;
                        break;
                }
                buf[len++] = ch;
            } else {
                buf[len++] = *cs++;
            }
        }

        /* Peek for adjacent TOK_STRING (L3 adjacent-string-concat).  If found,
         * consume it and grow the buffer to fit. */
        UToken nxt = peek(p);
        if (nxt.type != TOK_STRING) break;
        cur = consume(p);

        if (len + cur.u.str.len > cap) {
            /* Allocate a wider buffer; copy the prefix.  Old buffer is
             * abandoned in the arena (reset per top-level statement). */
            int new_cap = cap + cur.u.str.len;
            char *new_buf = (char *)uarena_alloc(p->arena, (size_t)new_cap);
            if (new_buf == NULL) return NULL;
            for (int i = 0; i < len; i++) new_buf[i] = buf[i];
            buf = new_buf;
            cap = new_cap;
        }
    }

    UAstNode *n = make_node(p, AST_STR, line, col);
    if (n == NULL) return NULL;
    n->u.str_lit.bytes = buf;
    n->u.str_lit.len = len;
    return n;
}

/* --- parse_prefix: unary +/- /! then atom.  Unary '+' is a no-op. --- */

UAstNode *parse_prefix(UParser *p) {
    UToken t = peek(p);
    if (t.type == TOK_PLUS) {
        consume(p);
        return parse_prefix(p);             /* +x is x; no node */
    }
    if (t.type == TOK_MINUS) {
        consume(p);
        UAstNode *operand = parse_prefix(p); /* right-assoc: --3 -> -(-3) */
        if (!operand) return NULL;
        if (operand->kind == AST_ERROR) return operand;
        operand = parse_expression_cont(p, operand, PARSE_PREC_POSTFIX);
        if (!operand) return NULL;
        if (operand->kind == AST_ERROR) return operand;
        return make_unary(p, UOP_NEG, operand, t.line, t.col);
    }
    if (t.type == TOK_BANG) {
        /* Prefix `!x` — logical NOT.  Recognized here (primary position) so
         * postfix `e!` (in the post-primary loop) does not steal it.
         * The operand is parsed through PARSE_PREC_POSTFIX, so a postfix `!`
         * on the operand binds first: `!e!` parses as `!(e!)`. */
        consume(p);
        UAstNode *operand = parse_prefix(p);
        if (!operand) return NULL;
        if (operand->kind == AST_ERROR) return operand;
        operand = parse_expression_cont(p, operand, PARSE_PREC_POSTFIX);
        if (!operand) return NULL;
        if (operand->kind == AST_ERROR) return operand;
        return make_unary(p, UOP_NOT, operand, t.line, t.col);
    }
    return parse_atom(p);
}

/* === W10/v0.10.5: parse_bracket_literal =====================================
 * Parses `[...]` — either a list literal `[e1, e2, e3]` or a dict literal
 * `["k1" => v1, "k2" => v2]`.  Disambiguation: after the first element, if
 * `=>` is present it is a dict; otherwise it is a list.  An empty `[]` is
 * an empty list; `[=>]` is not supported (use `Dict.new()`).
 *
 * Caller has confirmed peek() is TOK_LBRACKET.  Consumes `[` + contents + `]`.
 *
 * List:   AST_LIST_LIT { elems[], count }
 * Dict:   AST_DICT_LIT { keys[], vals[], count }
 * ========================================================================== */

UAstNode *parse_bracket_literal(UParser *p) {
    UToken lbr = consume(p);  /* consume '[' */

    int cap = 4;
    UAstNode **elems = (UAstNode **)uarena_alloc(p->arena,
                                                  (size_t)cap * sizeof(UAstNode *));
    if (!elems) return (UAstNode *)&uparser_oom_sentinel;

    int count = 0;
    bool is_dict = false;    /* determined on first element with `=>` */
    bool dict_decided = false;

    /* keys/vals for dict — lazy-allocated when we discover is_dict. */
    UAstNode **keys = NULL;
    UAstNode **vals = NULL;

    /* Empty list: `[]` */
    if (peek(p).type == TOK_RBRACKET) {
        consume(p);
        UAstNode *n = make_node(p, AST_LIST_LIT, lbr.line, lbr.col);
        if (!n) return NULL;
        n->u.list_lit.elems = elems;  /* empty; valid pointer */
        n->u.list_lit.count = 0;
        return n;
    }

    while (peek(p).type != TOK_RBRACKET && peek(p).type != TOK_EOF) {
        /* Parse the first part of each element (key or element value). */
        UAstNode *first = parse_expression(p, 0);
        if (!first) return (UAstNode *)&uparser_oom_sentinel;
        if (first->kind == AST_ERROR) return first;

        if (!dict_decided) {
            /* Decide list vs dict based on presence of `=>` after first elem. */
            is_dict = (peek(p).type == TOK_FAT_ARROW);
            dict_decided = true;
            if (is_dict) {
                /* Allocate keys/vals from arena (same initial cap). */
                keys = (UAstNode **)uarena_alloc(p->arena,
                                                  (size_t)cap * sizeof(UAstNode *));
                vals = (UAstNode **)uarena_alloc(p->arena,
                                                  (size_t)cap * sizeof(UAstNode *));
                if (!keys || !vals) return (UAstNode *)&uparser_oom_sentinel;
            }
        }

        if (is_dict) {
            /* Dict mode: `key => value`. */
            { UAstNode *err = NULL; if (!expect(p, TOK_FAT_ARROW, PARSE_DICT_EXPECTED_FAT_ARROW, &err)) return err; }  /* consume '=>' */
            UAstNode *val = parse_expression(p, 0);
            if (!val) return (UAstNode *)&uparser_oom_sentinel;
            if (val->kind == AST_ERROR) return val;
            if (count == cap) {
                if (!arena_grow_node_array(p, &keys, &cap, count))
                    return (UAstNode *)&uparser_oom_sentinel;
                /* vals was allocated with same original cap; grow it too. */
                int vals_cap = count; /* before grow */
                if (!arena_grow_node_array(p, &vals, &vals_cap, count))
                    return (UAstNode *)&uparser_oom_sentinel;
            }
            keys[count] = first;
            vals[count] = val;
            count++;
        } else {
            /* List mode. */
            if (count == cap) {
                if (!arena_grow_node_array(p, &elems, &cap, count))
                    return (UAstNode *)&uparser_oom_sentinel;
            }
            elems[count++] = first;
        }

        if (peek(p).type == TOK_COMMA) {
            consume(p);
        } else {
            break;
        }
    }

    { UAstNode *err = NULL; if (!expect(p, TOK_RBRACKET, PARSE_EXPECTED_RBRACKET, &err)) return err; }  /* consume ']' */

    if (is_dict) {
        UAstNode *n = make_node(p, AST_DICT_LIT, lbr.line, lbr.col);
        if (!n) return NULL;
        n->u.dict_lit.keys  = keys;
        n->u.dict_lit.vals  = vals;
        n->u.dict_lit.count = count;
        return n;
    } else {
        UAstNode *n = make_node(p, AST_LIST_LIT, lbr.line, lbr.col);
        if (!n) return NULL;
        n->u.list_lit.elems = elems;
        n->u.list_lit.count = count;
        return n;
    }
}
/* === end W10/v0.10.5: parse_bracket_literal === */

/* --- parse_atom: INT | IDENT | true | false | nil | ( expr ) | error.
 *
 * PARSE-032 closure (doc-only): time-literal suffixes (`100ms`, `1s`, `1d`)
 * + angle suffixes (`180deg`, `2pi`, `200grad`) are absorbed at the lexer
 * (`src/lex/ulex.c` rolls suffix into TOK_INT.u.i — microseconds for time,
 * milli-radians or fixed-point for angle).  parse_atom
 * intentionally only handles the bare TOK_INT here — the audit was filed
 * because the parser surface looked incomplete; the apparent gap is the
 * lex-side absorption.  When v1.x adds suffix overloading for non-int
 * receivers (`Decimal(0.5s)` etc.), this comment + the lex translation
 * site are the canonical change-points. */

UAstNode *parse_atom(UParser *p) {
    UToken t = peek(p);
    switch (t.type) {
    case TOK_INT:
        consume(p);
        return make_int(p, t.u.i, t.line, t.col);
    case TOK_FLOAT: {
        consume(p);
        UAstNode *n = make_node(p, AST_FLOAT_LIT, t.line, t.col);
        if (!n) return NULL;
        n->u.f = t.u.f;
        return n;
    }
    case TOK_STRING:
        return parse_string_literal(p);
    case TOK_IDENT:
        consume(p);
        return make_ident(p, t.u.str.start, t.u.str.len, t.line, t.col);
    case TOK_KW_TRUE:
        consume(p);
        return make_bool_node(p, true, t.line, t.col);
    case TOK_KW_FALSE:
        consume(p);
        return make_bool_node(p, false, t.line, t.col);
    case TOK_KW_NIL:
        consume(p);
        return make_nil_node(p, t.line, t.col);
    case TOK_KW_THIS: {
        UToken tok = consume(p);
        return make_this_node(p, tok.line, tok.col);
    }
    case TOK_LPAREN: {
        consume(p);
        UAstNode *inner = parse_expression(p, 0);
        if (!inner) return NULL;
        if (inner->kind == AST_ERROR) return inner;
        { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }
        return inner;
    }
    case TOK_KW_FUNCTION:
        return parse_function(p);
    case TOK_KW_TRY:
        return parse_try(p);
    case TOK_KW_THROW:
        return parse_throw(p);
    /* W9/v0.10.5: waituntil(e?) used as expression (e.g. `var r = waituntil(e?)`).
     * parse_atom is the expression-parser entry; parse_statement_or_expr also
     * handles it at statement-start level.  Adding it here allows waituntil
     * to appear on the right-hand side of assignments and inside function bodies. */
    case TOK_KW_WAITUNTIL:
        return parse_waituntil(p);
    case TOK_KW_CLOSURE:
        consume(p);
        return make_error(p, PARSE_CLOSURE_KEYWORD,
                          kErrorMessages[PARSE_CLOSURE_KEYWORD],
                          t.line, t.col);
    /* === W10/v0.10.5: list/dict literals === */
    case TOK_LBRACKET:
        return parse_bracket_literal(p);
    /* === end W10/v0.10.5 === */
    case TOK_EOF:
        return make_error(p, PARSE_UNEXPECTED_EOF,
                          kErrorMessages[PARSE_UNEXPECTED_EOF],
                          t.line, t.col);
    case TOK_ERROR:
        /* Note: do NOT consume — the statement-level recovery loop owns lexer advance. */
        return make_error(p, PARSE_LEX_ERROR,
                          t.u.err.message ? t.u.err.message
                                          : kErrorMessages[PARSE_LEX_ERROR],
                          t.line, t.col);
    default:
        return make_error(p, PARSE_EXPECTED_EXPRESSION,
                          kErrorMessages[PARSE_EXPECTED_EXPRESSION],
                          t.line, t.col);
    }
}

/* --- arena_grow_node_array: double a UAstNode* arena array when full.
   Called when count == cap.  Allocates a new block of cap*2 entries from the
   arena, copies the existing entries, and updates *arr and *cap.
   Returns true on success, false on arena OOM (caller should return the
   uparser_oom_sentinel). */
bool arena_grow_node_array(UParser *p, UAstNode ***arr, int *cap, int count) {
    int new_cap = (*cap) * 2;
    UAstNode **bigger = (UAstNode **)uarena_alloc(p->arena,
                                                   (size_t)new_cap * sizeof(UAstNode *));
    if (!bigger) return false;
    for (int i = 0; i < count; i++) bigger[i] = (*arr)[i];
    *arr = bigger;
    *cap = new_cap;
    return true;
}

/* --- parse_call_args: parse `(` arg, arg, ... `)` after a callee expression.
   Returns an AST_CALL node. callee is already parsed. --- */

UAstNode *parse_call_args(UParser *p, UAstNode *callee) {
    UToken lparen = consume(p);  /* consume '(' */

    int cap = 4;
    UAstNode **args = (UAstNode **)uarena_alloc(p->arena,
                                                (size_t)cap * sizeof(UAstNode *));
    if (!args) return (UAstNode *)&uparser_oom_sentinel;
    int count = 0;

    while (peek(p).type != TOK_RPAREN && peek(p).type != TOK_EOF) {
        UAstNode *arg = parse_inner_tier(p);
        if (!arg) return (UAstNode *)&uparser_oom_sentinel;
        if (arg->kind == AST_ERROR) return arg;

        if (count == cap) {
            if (!arena_grow_node_array(p, &args, &cap, count))
                return (UAstNode *)&uparser_oom_sentinel;
        }
        args[count++] = arg;

        if (peek(p).type == TOK_COMMA) {
            consume(p);
        } else {
            break;
        }
    }

    { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }  /* consume ')' */

    UAstNode *node = make_node(p, AST_CALL, lparen.line, lparen.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.call.callee    = callee;
    node->u.call.args      = args;
    node->u.call.arg_count = count;
    return node;
}

/* --- parse_member_access: handle a single `.IDENT` or `->IDENT` postfix.
   Caller has confirmed peek() is TOK_DOT or TOK_ARROW; this function
   consumes the operator, the IDENT, and (optionally) `= rhs`.

   Shape produced:
     obj.x        → AST_MEMBER_GET
     obj.x = v    → AST_MEMBER_SET   (consumes `= v`)
     obj.x->y     → AST_PROP_GET
     obj.x->y = v → AST_PROP_SET     (consumes `= v`)

   The caller's postfix loop should `break` after a SET arm (assignment
   terminates further chaining) and `continue` after a GET arm so that
   `obj.x.y` and `obj.x()` keep building on the result.

   *out_is_assign is set to true when the SET form was produced.

   Returns the new node, or an AST_ERROR / OOM sentinel on failure. --- */

UAstNode *parse_member_access(UParser *p, UAstNode *recv,
                                     bool *out_is_assign) {
    UToken op = consume(p);  /* TOK_DOT or TOK_ARROW */
    *out_is_assign = false;

    UToken name = peek(p);
    { UAstNode *err = NULL; if (!expect(p, TOK_IDENT, PARSE_EXPECTED_IDENT, &err)) return err; }

    const bool is_arrow = (op.type == TOK_ARROW);

    /* T41: `Foo.get value(...)` / `Foo.set value(...)` getter/setter sugar.
     * Only triggers on dot-access (not arrow), where the consumed IDENT is
     * `get` or `set`, AND the next two tokens are IDENT followed by `(`.
     * Outside that strict shape, `get` / `set` remain plain slot names. */
    if (!is_arrow
        && (ident_equals(name.u.str.start, name.u.str.len, "get", 3) ||
            ident_equals(name.u.str.start, name.u.str.len, "set", 3))
        && peek(p).type == TOK_IDENT && peek2(p).type == TOK_LPAREN) {
        UAstMethodKind kind =
            ident_equals(name.u.str.start, name.u.str.len, "get", 3)
                ? UAST_METHOD_GETTER : UAST_METHOD_SETTER;
        UToken slot_name = consume(p);  /* consume the slot-name IDENT */
        UAstNode *pd = parse_property_decl(p, recv, slot_name, kind,
                                           op.line, op.col);
        /* Property-decl is a side-effecting installation; treat as an
         * "assign" so the Pratt postfix loop stops climbing. */
        *out_is_assign = true;
        return pd;
    }

    if (peek(p).type == TOK_EQ) {
        consume(p);  /* consume '=' */
        /* S48 (2026-05-16): parse RHS as a Pratt expression, NOT
         * parse_inner_tier — the latter absorbs `|` / `&` separators
         * into the assignment value, so `Realm.a = 1 | Realm.b = 2`
         * mis-parses as `Realm.a = (1 | (Realm.b = 2))` instead of
         * `(Realm.a = 1) | (Realm.b = 2)`.  The mis-parse produces
         * nested MEMBER_SETs whose emit only writes the last slot
         * before fataling.  Hardware-observed on eye_demo blob_seen
         * handler 2026-05-16; host repro confirms.  parse_expression
         * stops at `|` / `&` since they aren't in the Pratt table,
         * leaving the separator for the outer inner-tier fold. */
        UAstNode *value = parse_expression(p, 0);
        if (!value) return NULL;
        if (value->kind == AST_ERROR) return value;
        UAstNode *node = make_node(p, is_arrow ? AST_PROP_SET : AST_MEMBER_SET,
                                   op.line, op.col);
        if (!node) return NULL;
        if (is_arrow) {
            node->u.prop.recv            = recv;
            node->u.prop.prop_name_start = name.u.str.start;
            node->u.prop.prop_name_len   = name.u.str.len;
            node->u.prop.value           = value;
        } else {
            node->u.member.recv       = recv;
            node->u.member.name_start = name.u.str.start;
            node->u.member.name_len   = name.u.str.len;
            node->u.member.value      = value;
        }
        *out_is_assign = true;
        return node;
    }

    UAstNode *node = make_node(p, is_arrow ? AST_PROP_GET : AST_MEMBER_GET,
                               op.line, op.col);
    if (!node) return NULL;
    if (is_arrow) {
        node->u.prop.recv            = recv;
        node->u.prop.prop_name_start = name.u.str.start;
        node->u.prop.prop_name_len   = name.u.str.len;
        node->u.prop.value           = NULL;
    } else {
        node->u.member.recv       = recv;
        node->u.member.name_start = name.u.str.start;
        node->u.member.name_len   = name.u.str.len;
        node->u.member.value      = NULL;
    }
    return node;
}

/* --- parse_expression_cont: Pratt infix/postfix loop starting from lhs.
   Called by parse_expression (after parse_prefix) and by
   parse_inner_tier_from_lhs (after an already-produced lhs node). --- */

UAstNode *parse_expression_cont(UParser *p, UAstNode *lhs, int min_prec) {
    for (;;) {
        UToken op = peek(p);

        /* Postfix call: `expr(args)` — highest precedence (postfix). */
        if (op.type == TOK_LPAREN && min_prec <= PARSE_PREC_POSTFIX) {
            lhs = parse_call_args(p, lhs);
            if (!lhs) return NULL;
            if (lhs->kind == AST_ERROR) return lhs;
            continue;
        }

        /* Postfix member-access: `expr.IDENT [= rhs]` and `expr->IDENT [= rhs]`.
           Same precedence tier as the postfix call.  After a SET form, stop
           climbing — assignment terminates the postfix chain.  After a GET,
           keep looping so chains like `a.b.c` and `a.b()` keep building. */
        if ((op.type == TOK_DOT || op.type == TOK_ARROW) && min_prec <= PARSE_PREC_POSTFIX) {
            bool is_assign = false;
            lhs = parse_member_access(p, lhs, &is_assign);
            if (!lhs) return NULL;
            if (lhs->kind == AST_ERROR) return lhs;
            if (is_assign) break;
            /* Spec #4 §4.4–§4.6: bare/emit `.changed` outside at(...). */
            if (!p->at_event_cond
                && lhs->kind == AST_MEMBER_GET
                && ident_equals(lhs->u.member.name_start,
                                lhs->u.member.name_len,
                                "changed", 7)) {
                UToken nxt = peek(p);
                if (nxt.type == TOK_BANG) {
                    return make_error(p, PARSE_SLOT_CHANGED_EMIT_V1,
                                      kErrorMessages[PARSE_SLOT_CHANGED_EMIT_V1],
                                      nxt.line, nxt.col);
                }
                return make_error(p, PARSE_SLOT_CHANGED_BARE_V1,
                                  kErrorMessages[PARSE_SLOT_CHANGED_BARE_V1],
                                  lhs->line, lhs->col);
            }
            continue;
        }

        /* Postfix `e!` — desugar to `e.emit([arg])`.
           `e!`        → AST_CALL { callee=lhs, method="emit", args=[] }
           `e!(p)`     → AST_CALL { callee=lhs, method="emit", args=[p] }
           `e!(x,y,z)` → PARSE_EMIT_MULTI_ARG_V1 error */
        if (op.type == TOK_BANG && min_prec <= PARSE_PREC_POSTFIX) {
            consume(p);  /* consume '!' */
            lhs = desugar_postfix_emit(p, lhs, op);
            if (!lhs) return NULL;
            if (lhs->kind == AST_ERROR) return lhs;
            continue;
        }

        /* === W10/v0.10.5: subscript `l[i]`, `l[i] = v`, `l[i] += v` ===
         * Postfix `[index]` — subscript access.  Lowers to:
         *   l[i]      → AST_SUBSCRIPT_GET  (emit: l.get(i))
         *   l[i] = v  → AST_SUBSCRIPT_SET  (emit: l.set(i, v))
         *   l[i] += v → AST_SUBSCRIPT_SET  (emit: l.set(i, l.get(i) + v),
         *                                   is_compound_add=true)
         * No new opcode needed; same precedence tier as member/call. */
        if (op.type == TOK_LBRACKET && min_prec <= PARSE_PREC_POSTFIX) {
            consume(p);  /* consume '[' */
            UAstNode *index = parse_expression(p, 0);
            if (!index) return NULL;
            if (index->kind == AST_ERROR) return index;
            { UAstNode *err = NULL; if (!expect(p, TOK_RBRACKET, PARSE_SUBSCRIPT_EXPECTED_RBRACKET, &err)) return err; }  /* consume ']' */
            /* Peek for assignment or compound-assign. */
            UToken nxt = peek(p);
            if (nxt.type == TOK_EQ) {
                consume(p);  /* consume '=' */
                UAstNode *val = parse_expression(p, 0);
                if (!val) return NULL;
                if (val->kind == AST_ERROR) return val;
                UAstNode *ss = make_node(p, AST_SUBSCRIPT_SET, op.line, op.col);
                if (!ss) return NULL;
                ss->u.subscript.recv            = lhs;
                ss->u.subscript.index           = index;
                ss->u.subscript.value           = val;
                ss->u.subscript.is_compound_add = false;
                lhs = ss;
                break;  /* assignment terminates postfix chain */
            }
            if (nxt.type == TOK_PLUS_EQ) {
                consume(p);  /* consume '+=' */
                UAstNode *rhs = parse_expression(p, 0);
                if (!rhs) return NULL;
                if (rhs->kind == AST_ERROR) return rhs;
                UAstNode *ss = make_node(p, AST_SUBSCRIPT_SET, op.line, op.col);
                if (!ss) return NULL;
                ss->u.subscript.recv            = lhs;
                ss->u.subscript.index           = index;
                ss->u.subscript.value           = rhs;
                ss->u.subscript.is_compound_add = true;
                lhs = ss;
                break;  /* assignment terminates postfix chain */
            }
            /* Plain get. */
            UAstNode *sg = make_node(p, AST_SUBSCRIPT_GET, op.line, op.col);
            if (!sg) return NULL;
            sg->u.subscript.recv            = lhs;
            sg->u.subscript.index           = index;
            sg->u.subscript.value           = NULL;
            sg->u.subscript.is_compound_add = false;
            lhs = sg;
            continue;
        }
        /* === end W10/v0.10.5: subscript === */

        /* Postfix `?` — only valid inside at(...) condition.
         * When at_event_cond is set, pass through (parse_at will consume it).
         * Otherwise it is an error. */
        if (op.type == TOK_QUESTION && min_prec <= PARSE_PREC_POSTFIX) {
            if (p->at_event_cond) break;  /* let parse_at consume it */
            consume(p);
            return make_error(p, PARSE_QUESTION_OUTSIDE_AT,
                              kErrorMessages[PARSE_QUESTION_OUTSIDE_AT],
                              op.line, op.col);
        }

        int prec = infix_prec(op.type);
        if (prec < min_prec || prec == 0) break;

        consume(p);
        /* Comparison operators are left-associative; use prec+1 for right
           so that `a == b == c` is rejected as ambiguous (each side parses
           as atoms, and chained comparisons are a parse error in urbiscript). */
        UAstNode *right = parse_expression(p, prec + 1);
        if (!right) return NULL;
        if (right->kind == AST_ERROR) return right;

        /* === W3/v0.10.11: << desugars to lhs.'<<'(rhs) method call ===
         *
         * Builds:  AST_CALL { callee = AST_MEMBER_GET(lhs, "<<"), args=[rhs] }
         *
         * The member name "<<" is a quoted-ident selector — the runtime
         * dispatches it via the normal OP_GETSLOT + OP_CALL pipeline for
         * any object that defines a '<<' slot (e.g. Channel, or any class).
         * Left-associative (prec+1 on right above), so:
         *   cout << a << b  →  (cout << a) << b
         * which is the standard streaming / chaining convention. */
        if (op.type == TOK_LSHIFT) {
            /* Selector: the two-byte string "<<" (quoted-ident style).
             * kLShiftSelector is the file-scope const above. */
            UAstNode *member = make_node(p, AST_MEMBER_GET, op.line, op.col);
            if (!member) return NULL;
            member->u.member.recv       = lhs;
            member->u.member.name_start = kLShiftSelector;
            member->u.member.name_len   = kLShiftSelectorLen;
            member->u.member.value      = NULL;
            UAstNode **args = (UAstNode **)uarena_alloc(p->arena, sizeof(UAstNode *));
            if (!args) return (UAstNode *)&uparser_oom_sentinel;
            args[0] = right;
            UAstNode *call = make_node(p, AST_CALL, op.line, op.col);
            if (!call) return NULL;
            call->u.call.callee    = member;
            call->u.call.args      = args;
            call->u.call.arg_count = 1;
            lhs = call;
            continue;
        }
        /* === end W3/v0.10.11 === */

        /* === v1.0-rc stdlib-completeness: % desugars to lhs.'%'(rhs) ===
         *
         * Mirrors the `<<` desugar above: builds
         *   AST_CALL { callee = AST_MEMBER_GET(lhs, "%"), args=[rhs] }
         * The `'%'` slot lives on Integer/Float (atoms stdlib).  Multiplicative
         * precedence (7), left-associative (prec+1 on right). */
        if (op.type == TOK_PERCENT) {
            UAstNode *member = make_node(p, AST_MEMBER_GET, op.line, op.col);
            if (!member) return NULL;
            member->u.member.recv       = lhs;
            member->u.member.name_start = kModSelector;
            member->u.member.name_len   = kModSelectorLen;
            member->u.member.value      = NULL;
            UAstNode **args = (UAstNode **)uarena_alloc(p->arena, sizeof(UAstNode *));
            if (!args) return (UAstNode *)&uparser_oom_sentinel;
            args[0] = right;
            UAstNode *call = make_node(p, AST_CALL, op.line, op.col);
            if (!call) return NULL;
            call->u.call.callee    = member;
            call->u.call.args      = args;
            call->u.call.arg_count = 1;
            lhs = call;
            continue;
        }

        /* === v1.0-rc stdlib-completeness: && / || short-circuit logical ===
         *
         * Builds AST_LOGICAL { lhs, rhs, is_or }; emit lowers it to an
         * OP_TESTSET + OP_JMP short-circuit (RHS skipped when LHS settles
         * the result).  Distinct from AST_BINARY (which evaluates both
         * operands eagerly).  Left-associative via prec+1 on `right`. */
        if (op.type == TOK_AMPAMP || op.type == TOK_PIPEPIPE) {
            UAstNode *node = make_node(p, AST_LOGICAL, op.line, op.col);
            if (!node) return NULL;
            node->u.logical.lhs   = lhs;
            node->u.logical.rhs   = right;
            node->u.logical.is_or = (op.type == TOK_PIPEPIPE) ? 1 : 0;
            lhs = node;
            continue;
        }
        /* === end v1.0-rc stdlib-completeness === */

        if (is_compare_token(op.type)) {
            lhs = make_compare(p, compare_op(op.type), lhs, right,
                               op.line, op.col);
        } else {
            lhs = make_binary(p, infix_binop(op.type), lhs, right,
                              op.line, op.col);
        }
        if (!lhs) return NULL;
    }
    return lhs;
}

/* --- parse_expression: Pratt precedence climbing over parse_prefix. --- */

UAstNode *parse_expression(UParser *p, int min_prec) {
    /* v0.9.1: depth-guarded entry.  parse_expression is the canonical
     * recursive descent site for nested expressions like `(((1)))` or
     * deep operator chains — a pathological compile bomb (e.g. 1000
     * nested parens) trips here before stack exhaust.  parse_prefix and
     * parse_expression_cont call back into parse_expression for grouped
     * sub-expressions, so guarding the entry is sufficient. */
    if (!uparse_budget_enter(p)) return NULL;
    UAstNode *lhs = parse_prefix(p);
    if (!lhs) {
        uparse_budget_leave(p);
        return NULL;
    }
    if (lhs->kind == AST_ERROR) {
        uparse_budget_leave(p);
        return lhs;
    }
    UAstNode *r = parse_expression_cont(p, lhs, min_prec);
    uparse_budget_leave(p);
    return r;
}
