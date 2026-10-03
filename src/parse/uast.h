/* SPDX-License-Identifier: BSD-3-Clause */
/* AST node types shared between parser, desugar, and emit. */

#ifndef UAST_H
#define UAST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Watcher modes — the value AST_WATCHER carries in `u.watcher.mode`.
 * Combined with `u.watcher.source` (UAstWatchSource, below) this is the
 * only thing the emitter needs to pick an install opcode.  The runtime
 * does not read either field: each (mode, source) pair has its own
 * opcode, and src/rt/uwatch.h keeps its own representation of what an
 * installed watcher is. */
#define UWATCHER_AT        1   /* at (cond) body       — edge-triggered  */
#define UWATCHER_WHENEVER  2   /* whenever (cond) body — level-triggered */
#define UWATCHER_AT_SYNC   3   /* at sync (cond) body  — synchronous     */
#define UWATCHER_WAITUNTIL 4   /* waituntil (cond)     — one-shot strand-block */

/* AST_WATCHER's source discriminator — what `u.watcher.cond` actually
 * holds, and (with `mode`) which install opcode the emitter selects:
 *   COND:        cond is the boolean condition expression.
 *   EVENT:       cond is the event expression (the `e` in `e?`).
 *   SLOT_CHANGE: cond is the receiver (the `obj` in `obj.x.changed?`);
 *                `slot_name`/`slot_name_len` name the watched slot. */
typedef enum { UWSRC_COND = 0, UWSRC_EVENT = 1, UWSRC_SLOT_CHANGE = 2 } UAstWatchSource;

/* Node kinds. */
typedef enum {
    /* atomic + arithmetic */
    AST_INT     = 0,
    AST_IDENT   = 1,
    AST_UNARY   = 2,
    AST_BINARY  = 3,
    AST_ERROR   = 4,

    /* literal extensions */
    AST_BOOL    = 5,
    AST_NIL     = 6,

    /* separators */
    AST_SEQ     = 7,        /* a separator-joined sequence: `;` `,` `|` `&` */
    AST_NOOP    = 8,        /* singleton; legacy compat (see separator spec §3) */

    /* declarations + scope */
    AST_VAR_DECL  = 9,      /* var x = expr; locals registered in FuncState */
    AST_BLOCK     = 10,     /* { stmt; stmt; ... } */

    /* control flow */
    AST_IF      = 11,       /* if (cond) then-block [else else-block] */
    AST_WHILE   = 12,       /* while (cond) body */
    AST_COMPARE = 13,       /* ==, !=, <, <=, >, >= */

    /* functions */
    AST_FUNCTION   = 14,    /* function (params) { body } */
    AST_CALL       = 15,    /* callee(args) */
    AST_RETURN     = 16,    /* return [expr] */
    AST_PARAM      = 17,    /* formal parameter; u.param.is_lazy flags `lazy x` */

    /* assignment */
    AST_ASSIGN     = 19,    /* x = expr; assignment to existing local/upvalue */

    /* control transfer */
    AST_TRY        = 20,    /* try { body } [catch (e) { handler }] [finally { cleanup }] */
    AST_THROW      = 21,    /* throw expr */

    /* tag scope */
    AST_TAG_PREFIX = 22,    /* mytag: { body } — tag-scope syntax; tag-prefix
                               onleave clause is v1.x (PARSE-033 closure) */

    /* slot member access */
    AST_MEMBER_GET = 23,    /* obj.x         — recv + name */
    AST_MEMBER_SET = 24,    /* obj.x = v     — recv + name + value */

    /* reactive constructs */
    AST_WATCHER      = 25,  /* at / at sync / whenever / waituntil — one node
                             * for every reactive form.  `u.watcher.mode`
                             * (UWATCHER_AT, UWATCHER_AT_SYNC, UWATCHER_WHENEVER,
                             * UWATCHER_WAITUNTIL) and `u.watcher.source`
                             * (UWSRC_COND, UWSRC_EVENT, UWSRC_SLOT_CHANGE)
                             * together select the OP_INSTALL mode: AT /
                             * AT_SYNC / WHENEVER on a condition (COND) or on
                             * an event (EVENT); the same EVENT modes after an
                             * OP_GETSLOT_CHANGE_EVENT (SLOT_CHANGE); WAITUNTIL
                             * (WAITUNTIL + COND) or a desugared `e.waituntil()` call
                             * (WAITUNTIL + EVENT).  spec #2 §3.10, spec #3,
                             * spec #4. */

    /* string literal */
    AST_STR     = 29,       /* string literal — escape-resolved + adjacent-concat
                             * folded view into an arena-allocated buffer.  Emit
                             * routes through OP_LOADK with a UVAL_STR constant
                             * (interning happens at emit time, not parse time,
                             * matching the AST_IDENT pattern). */

    AST_FLOAT_LIT = 30,     /* floating-point literal — 1.5, .5, 1.5e3, 1e3.
                             * Parsed from TOK_FLOAT; emit routes through
                             * OP_LOADK with a UVAL_FLOAT constant. */

    AST_THIS = 31,          /* `this` keyword — resolves to receiver (R0) in
                             * method bodies.  Carries no payload; line+col
                             * are inherited from the base node.  Top-level
                             * `this` (lobby alias) is deferred to v1.x;
                             * emitter raises EMIT_NO_THIS_OUTSIDE_METHOD when
                             * fs->parent == NULL. */

    AST_BREAK    = 33,  /* break — exits innermost for/while loop.
                         * Lowered to OP_JMP with the exit address patched after the loop.
                         * No new opcode needed. */
    AST_CONTINUE = 34,  /* continue — jumps to next iteration of innermost for/while.
                         * Lowered to OP_JMP with the continue address patched after the loop.
                         * No new opcode needed. */
    AST_SWITCH   = 35,  /* switch (expr) { case v1: body1; case v2: body2; }
                         * Equality-based dispatch only (no pattern matching).
                         * Lowered to a chain of if (expr == vN) { bodyN }.
                         * No new opcode needed. */

    /* === v1.0-rc stdlib-completeness: short-circuit logical operators === */
    AST_LOGICAL  = 36   /* a && b / a || b — short-circuit.  Distinct from
                         * AST_BINARY (eager both operands).  Lowers to
                         * OP_TESTSET + OP_JMP; RHS skipped when LHS settles
                         * the result.  is_or selects && vs ||. No new opcode. */
    /* === end v1.0-rc stdlib-completeness === */
} UAstKind;

/* Method/property-decl kind discriminator (getter/setter). */
typedef enum {
    UAST_METHOD_PLAIN  = 0,
    UAST_METHOD_GETTER = 1,
    UAST_METHOD_SETTER = 2
} UAstMethodKind;

typedef enum {
    UOP_NEG = 0,
    UOP_NOT = 1            /* logical not — lowered via OP_TEST/OP_LOADBOOL (FE-03) */
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

    /* declaration additions */
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

    /* control-transfer additions */
    PARSE_TRY_NEEDS_CATCH_OR_FINALLY, /* `try { }` with neither catch nor finally */

    /* reactive-runtime additions */
    PARSE_RESERVED_KEYWORD_AS_IDENT,  /* `var at = 1`: hard keyword used as variable name */
    PARSE_QUESTION_OUTSIDE_AT,        /* postfix `?` is only valid inside at(...) */
    PARSE_EMIT_MULTI_ARG_V1,          /* `e!(x, y, z)` — multi-arg emit reserved for v1.x */

    /* spec #4 additions */
    PARSE_SLOT_CHANGED_BARE_V1,       /* `obj.x.changed` outside at(?) — use at(obj.x.changed?) */
    PARSE_SLOT_CHANGED_EMIT_V1,       /* `obj.x.changed!` — slot-change event cannot be emitted */

    PARSE_NAMED_FUNCTION_NOT_SUPPORTED, /* `function name(...){...}` — v1.0 has no
                                            named-function decls; use
                                            `var name = function(...){...}` */
    PARSE_AT_SYNC_DOES_NOT_SUPPORT_ONLEAVE, /* `at sync (cond) body onleave h` —
                                              at sync fires inline on the
                                              changed thread and has no leave
                                              edge to hook (spec §3) */

    /* getter/setter additions */
    PARSE_TOPLEVEL_GETSET_NOT_SUPPORTED, /* `get name() {...}` /
                                            `set name(v) {...}` at statement
                                            start.  The implicit-receiver form
                                            has no v1.0 resolver outside a
                                            class body; deferred to v1.x
                                            implicit-this. */

    PARSE_EXPECTED_RBRACKET,    /* missing `]` in list/dict literal or subscript */
    PARSE_DICT_EXPECTED_FAT_ARROW, /* dict literal: `key` not followed by `=>` */
    PARSE_SUBSCRIPT_EXPECTED_RBRACKET, /* `l[i` missing `]` */
    PARSE_VAR_OBJ_SLOT_NO_INIT,        /* `var obj.slot` with no `= value` */
    PARSE_SUBSCRIPT_COMPOUND_OP_V1X,   /* compound subscript op other than +=
                                        * (e.g. -=, *=) — deferred to v1.x */

    PARSE_FOR_EXPECTED_VAR,            /* for loop header missing `var` keyword */
    PARSE_FOR_EXPECTED_COLON_OR_IN,    /* for (var x ...) — missing `:` or `in` */
    PARSE_BREAK_OUTSIDE_LOOP,          /* `break` not inside a for/while loop */
    PARSE_CONTINUE_OUTSIDE_LOOP,       /* `continue` not inside a for/while loop */
    PARSE_SWITCH_EXPECTED_CASE,        /* switch body contains non-case statement */
    PARSE_SWITCH_EXPECTED_COLON,       /* case label missing trailing `:` */
    PARSE_SWITCH_DUPLICATE_DEFAULT,    /* switch body has more than one default: arm */

    PARSE_EVENT_PAYLOAD_BIND_EXPECTED_VAR,    /* `at (e?(x))` — must be `(var x)` */
    PARSE_EVENT_PAYLOAD_BIND_EXPECTED_IDENT,  /* `at (e?(var))` — identifier missing */
    PARSE_EVENT_PAYLOAD_BIND_EXPECTED_RPAREN, /* `at (e?(var x` — missing `)` */

    PARSE_CLASS_BODY_STATEMENT /* class body statement is neither a var
                                   declaration nor a getter/setter */
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
 *   u.f           — AST_FLOAT_LIT: parsed double value
 *   u.ident       — AST_IDENT:      zero-copy lexeme view
 *   u.unary       — AST_UNARY:      prefix operator + operand pointer
 *   u.binary      — AST_BINARY:     infix operator + two operand pointers
 *   u.logical     — AST_LOGICAL:    short-circuit && / || (lhs, rhs, is_or)
 *   u.err         — AST_ERROR:      UParseError + static message string
 *   u.b           — AST_BOOL:       boolean value
 *   [none]        — AST_NIL:        no payload (sentinel type)
 *   u.seq         — AST_SEQ:        separator + ordered array of children
 *   [none]        — AST_NOOP:       no payload (identity; legacy compat)
 *   u.var_decl    — AST_VAR_DECL:   variable declaration with init
 *   u.block       — AST_BLOCK:      scoped sequence of statements
 *   u.if_stmt     — AST_IF:         conditional statement
 *   u.while_stmt  — AST_WHILE:      iterative loop
 *   u.cmp         — AST_COMPARE:    comparison operator
 *   u.func        — AST_FUNCTION:   function definition
 *   u.call        — AST_CALL:       function application
 *   u.ret         — AST_RETURN:     early exit with value
 *   u.param       — AST_PARAM: formal parameter (is_lazy flags `lazy x`)
 *   u.assign      — AST_ASSIGN: assignment to existing local/upvalue
 *   u.try_stmt    — AST_TRY:    try body + optional catch/finally
 *   u.throw_expr  — AST_THROW:  value expression to throw
 *   u.tag_prefix  — AST_TAG_PREFIX: tag-scope (mytag: { body }); onleave is v1.x
 *   u.member      — AST_MEMBER_GET, AST_MEMBER_SET: slot read / slot assignment
 *   u.watcher     — AST_WATCHER: at/at sync/whenever/waituntil, every source
 *                   form (cond/event/slot-change), + optional onleave
 *   u.str_lit     — AST_STR:             escape-resolved string bytes view
 *   [none]        — AST_BREAK:           no payload (exits innermost loop)
 *   [none]        — AST_CONTINUE:        no payload (next iteration)
 *   u.switch_stmt — AST_SWITCH:          expr + parallel arrays of vals + bodies
 *
 * Slot name storage: zero-copy lexeme view (name_start + name_len), as
 * with var_decl/assign/param.  The parser has no UVM and therefore cannot
 * intern; emit will canonicalize via ustr_intern when it has VM access.
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
        double  f;                                          /* AST_FLOAT_LIT */
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
        struct {                                            /* AST_LOGICAL */
            UAstNode *lhs;
            UAstNode *rhs;
            int       is_or;   /* 1 = ||, 0 = && */
        } logical;
        struct {                                            /* AST_ERROR */
            int code;
            const char *message;
        } err;

        bool b;                                             /* AST_BOOL */
        /* AST_NIL has no payload */
        /* AST_NOOP has no payload (singleton in arena) */

        struct {                                            /* AST_SEQ */
            UAstSeparator separator;        /* SEP_SEMI, SEP_COMMA, SEP_PIPE, SEP_AMP */
            UAstNode    **children;         /* arena array, count >= 2 */
            int           count;
        } seq;
        struct {                                            /* AST_VAR_DECL */
            const char *name_start;        /* zero-copy lexeme view */
            int         name_len;
            UAstNode   *init;              /* may be NULL — `var x;` not legal at v1.0 */
        } var_decl;
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
            UAstNode  **params;            /* AST_PARAM */
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
        struct {                                            /* AST_PARAM */
            const char *name_start;
            int         name_len;
            UAstNode   *default_expr;      /* `= expr` default value or NULL
                                              (v0.13.5 legacy formal defaults;
                                              evaluated at call time in the
                                              callee scope when the caller
                                              omits the argument) */
            bool        is_lazy;           /* `lazy x` — caller-side thunk
                                              instead of eager evaluation */
        } param;
        struct {                                            /* AST_ASSIGN */
            const char *name_start;        /* zero-copy lexeme view */
            int         name_len;
            UAstNode   *value;
        } assign;
        struct {                                            /* AST_TRY */
            UAstNode   *body;              /* AST_BLOCK — the try body */
            /* catch clause — all NULL when no catch */
            const char *catch_var_start;   /* zero-copy catch variable name */
            int         catch_var_len;
            UAstNode   *catch_body;        /* AST_BLOCK or NULL */
            UAstNode   *catch_guard;       /* guard expr from `catch (var e if cond)`, or NULL */
            /* else clause — runs when try body completes without exception */
            UAstNode   *else_body;         /* AST_BLOCK or NULL */
            /* finally clause */
            UAstNode   *finally_body;      /* AST_BLOCK or NULL */
        } try_stmt;
        struct {                                            /* AST_THROW */
            UAstNode   *value;             /* expression to throw */
        } throw_expr;
        struct {                                            /* AST_TAG_PREFIX */
            UAstNode   *tag_expr;          /* the tag identifier (AST_IDENT) */
            UAstNode   *body;              /* AST_BLOCK — the tag-scoped body */
            UAstNode   *onleave;           /* onleave body for `tag: { body }
                                              onleave handler` syntax.  NULL
                                              today and the parser does not
                                              consume an `onleave` clause on
                                              AST_TAG_PREFIX — that surface is
                                              v1.x scope (PARSE-033 closure).
                                              The field is kept on the union
                                              variant so the v1.x parse + emit
                                              sites land as a code addition
                                              rather than an AST shape break.
                                              `at (cond) body onleave handler`
                                              uses AST_WATCHER.onleave instead;
                                              that form IS supported today. */
        } tag_prefix;
        struct {                                            /* AST_MEMBER_GET, AST_MEMBER_SET */
            UAstNode   *recv;              /* receiver expression */
            const char *name_start;        /* zero-copy lexeme view */
            int         name_len;
            UAstNode   *value;             /* SET only; NULL for GET */
        } member;
        struct {                                            /* AST_WATCHER */
            int             mode;            /* UWATCHER_* */
            UAstWatchSource source;
            UAstNode       *cond;            /* COND: the condition; EVENT: the event expression; SLOT_CHANGE: the receiver */
            const char     *slot_name;       /* SLOT_CHANGE only */
            int             slot_name_len;
            UAstNode       *body;            /* NULL for WAITUNTIL */
            UAstNode       *else_body;       /* WHENEVER only, nullable; fires on falling
                                              * edge.  When non-NULL, takes precedence over
                                              * onleave as the alt closure passed to
                                              * OP_WHENEVER_INSTALL. */
            UAstNode       *onleave;         /* nullable; rejected only for an
                                              * AT_SYNC watcher over a COND
                                              * source (parse_at_cond_form) —
                                              * AT_SYNC + EVENT/SLOT_CHANGE
                                              * does accept onleave */
            const char     *payload_var;     /* EVENT forms: `(var x)` name or NULL;
                                              * also set for SLOT_CHANGE (merged-node
                                              * behaviour — the baseline dropped it there) */
            int             payload_var_len;
        } watcher;
        struct {                                            /* AST_STR */
            const char *bytes;             /* arena-allocated escape-resolved
                                            * + concat-folded buffer; NOT
                                            * NUL-terminated; lifetime bound
                                            * to the parser's UArena */
            int         len;               /* byte count (excluding any NUL) */
        } str_lit;
        /* AST_BREAK and AST_CONTINUE carry no payload beyond line/col */
        struct {                                            /* AST_SWITCH */
            UAstNode   *expr;            /* switch expression (evaluated once) */
            UAstNode  **case_vals;       /* arena array of case value expressions */
            UAstNode  **case_bodies;     /* arena array of case body blocks */
            int         case_count;
            UAstNode   *default_body;    /* catch-all arm body; NULL if absent */
        } switch_stmt;
    } u;
};

#ifdef __cplusplus
}
#endif

#endif
