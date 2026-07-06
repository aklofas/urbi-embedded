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
    AST_NARY    = 7,        /* outer-tier: ;-or-,-joined sequence */
    AST_BIN_SEP = 8,        /* inner-tier: |-or-&-joined pair    */
    AST_NOOP    = 9,        /* singleton; legacy compat (see separator spec §3) */

    /* declarations + scope */
    AST_VAR_DECL  = 10,     /* var x = expr; locals registered in FuncState */
    AST_LOCAL_REF = 11,     /* resolved local reference (parser produces AST_IDENT;
                               emit converts to AST_LOCAL_REF after FuncState lookup) */
    AST_BLOCK     = 12,     /* { stmt; stmt; ... } */

    /* control flow */
    AST_IF      = 13,       /* if (cond) then-block [else else-block] */
    AST_WHILE   = 14,       /* while (cond) body */
    AST_COMPARE = 15,       /* ==, !=, <, <=, >, >= */

    /* functions */
    AST_FUNCTION   = 16,    /* function (params) { body } */
    AST_CALL       = 17,    /* callee(args) */
    AST_RETURN     = 18,    /* return [expr] */
    AST_PARAM      = 19,    /* formal parameter (eager, no `lazy`) */
    AST_LAZY_PARAM = 20,    /* formal parameter (`lazy x`) */

    /* assignment */
    AST_ASSIGN     = 21,    /* x = expr; assignment to existing local/upvalue */

    /* control transfer */
    AST_TRY        = 22,    /* try { body } [catch (e) { handler }] [finally { cleanup }] */
    AST_THROW      = 23,    /* throw expr */

    /* tag scope */
    AST_TAG_PREFIX = 24,    /* mytag: { body } — tag-scope syntax; tag-prefix
                               onleave clause is v1.x (PARSE-033 closure) */

    /* slot member access */
    AST_MEMBER_GET = 25,    /* obj.x         — recv + name */
    AST_MEMBER_SET = 26,    /* obj.x = v     — recv + name + value */
    AST_PROP_GET   = 27,    /* obj.x->prop   — recv + prop_name */
    AST_PROP_SET   = 28,    /* obj.x->prop = v — recv + prop_name + value */

    /* reactive constructs */
    AST_WATCHER      = 29,  /* at / at sync / whenever — mode discriminator in
                             * u.watcher.mode (UWATCHER_AT, UWATCHER_AT_SYNC,
                             * UWATCHER_WHENEVER); also carries optional onleave.
                             * spec #2 §3.10. Emits OP_AT_INSTALL / OP_AT_SYNC_INSTALL
                             * / OP_WHENEVER_INSTALL depending on mode. */
    AST_WAITUNTIL    = 30,  /* waituntil (cond) — structurally distinct cond-only node.
                             * spec #2. Emits OP_WAITUNTIL_INSTALL. */
    AST_AT_EVENT     = 31,  /* at (e?) / at sync (e?) — event-subscribe form.
                             * spec #3. Distinct from AST_WATCHER because dispatch goes
                             * through OP_AT_EVENT_INSTALL (=42), not OP_AT_INSTALL. */
    AST_AT_SLOT_CHANGE = 32, /* at (obj.x.changed?) / sync variant — slot-change subscribe.
                             * spec #4. Install needs OP_GETSLOT_CHANGE_EVENT (=44) prefix
                             * followed by OP_AT_EVENT_INSTALL. */

    /* string literal */
    AST_STR     = 33,       /* string literal — escape-resolved + adjacent-concat
                             * folded view into an arena-allocated buffer.  Emit
                             * routes through OP_LOADK with a UVAL_STR constant
                             * (interning happens at emit time, not parse time,
                             * matching the AST_IDENT pattern). */

    /* class declaration */
    AST_CLASS_DECL = 34,    /* class Foo [: public A, B] { body }
                             * Carries the class name (zero-copy lexeme view),
                             * an optional declaration-order proto array, and
                             * the body block.  Per S-class-name-scope, the
                             * class name is NOT in scope while protos and
                             * body parse — `class a : public a { ... }`
                             * resolves the proto `a` to the outer binding.
                             * Per S-mro-declaration-order, the proto array
                             * preserves left-to-right declaration order;
                             * emit reverses during insertFront so the chain
                             * ends up [P1, P2, Object] for `: public P1, P2`. */

    /* get/set parse sugar */
    AST_PROPERTY_DECL = 35, /* get name() { body } / set name(v) { body }
                             * Parse-only desugar — emit installs the closure
                             * as the slot's `oget` (URBI_SLOT_FLAG_OGET) or
                             * `oset` (URBI_SLOT_FLAG_OSET) property.  The
                             * runtime slot-property dispatch path is the IC-sites
                             * baseline; this feature only adds the parse sugar.
                             *
                             * `recv` is NULL when the property-decl appears
                             * at the start of a class body — emit treats the
                             * implicit receiver as the class object.  When
                             * `recv` is non-NULL (e.g. `Foo.get value() {}`)
                             * the receiver is emitted explicitly. */

    /* v0.6.2 Phase 1 — float literal (Gap #5) */
    AST_FLOAT_LIT = 36,     /* floating-point literal — 1.5, .5, 1.5e3, 1e3.
                             * Parsed from TOK_FLOAT; emit routes through
                             * OP_LOADK with a UVAL_FLOAT constant. */

    /* v0.6.2 Phase 2 — this keyword (Gap #3) */
    AST_THIS = 37,          /* `this` keyword — resolves to receiver (R0) in
                             * method bodies.  Carries no payload; line+col
                             * are inherited from the base node.  Top-level
                             * `this` (lobby alias) is deferred to v1.x;
                             * emitter raises EMIT_NO_THIS_OUTSIDE_METHOD when
                             * fs->parent == NULL. */

    /* === v0.10.5: assert keyword === */
    AST_ASSERT = 38,        /* assert(expr) / assert { block }
                             * Lowered to: if (!expr) throw "assertion failed: <src>"
                             * No new opcode needed.  src_text/src_len is the
                             * zero-copy source span of the expression (paren form);
                             * NULL/0 for block form.
                             * Ruling: implemented (v0.10.5, legacy F9). */

    /* === v0.10.5: list/dict literals + subscript + var-obj-slot === */
    AST_LIST_LIT = 39,      /* [e1, e2, e3]
                             * Lowered to: List.new(e1, e2, e3)
                             * No new opcode needed.
                             * Ruling: implemented (v0.10.5, legacy F14). */
    AST_DICT_LIT = 40,      /* ["a" => 1, "b" => 2]
                             * Lowered to: var _d = Dict.new(); _d.set("a", 1); ...
                             * No new opcode needed.
                             * Ruling: implemented (v0.10.5, legacy F14). */
    AST_SUBSCRIPT_GET = 41, /* l[i]  → l.get(i)
                             * No new opcode needed.
                             * Ruling: implemented (v0.10.5, legacy F14). */
    AST_SUBSCRIPT_SET = 42, /* l[i] = v  → l.set(i, v)
                             * l[i] += v  → tmp=recv, tmpi=idx, tmp.set(tmpi, tmp.get(tmpi)+v)
                             *              recv and idx evaluated exactly once (v0.10.7).
                             * No new opcode needed.
                             * Ruling: implemented (v0.10.5, legacy F14);
                             * single-eval fix (Wave 7 v0.10.7, closes audit-1 F4). */
    /* === end v0.10.5 === */

    /* === v0.10.5: control flow === */
    AST_FOR_EACH = 43,  /* for (var x : iter) body  / for (var x in iter) body
                         * Lowered to a while loop using list.length() + list.get(i).
                         * Also handles for (var x : list_expr) where list_expr is
                         * evaluated once before the loop.  No new opcode needed.
                         * Ruling: implemented (v0.10.5, legacy F2). */
    AST_BREAK    = 44,  /* break — exits innermost for/while loop.
                         * Lowered to OP_JMP with the exit address patched after the loop.
                         * No new opcode needed.
                         * Ruling: implemented (v0.10.5, legacy F2). */
    AST_CONTINUE = 45,  /* continue — jumps to next iteration of innermost for/while.
                         * Lowered to OP_JMP with the continue address patched after the loop.
                         * No new opcode needed.
                         * Ruling: implemented (v0.10.5, legacy F2). */
    AST_SWITCH   = 46,  /* switch (expr) { case v1: body1; case v2: body2; }
                         * Equality-based dispatch only (no pattern matching).
                         * Lowered to a chain of if (expr == vN) { bodyN }.
                         * No new opcode needed.
                         * Ruling: implemented (v0.10.5, legacy F2). */
    /* === end v0.10.5: control flow === */

    /* === v0.10.7: synthetic register-reference leaf === */
    AST_REG_REF  = 47   /* synthetic emit-only: reference to a previously-allocated
                         * register.  Never produced by the parser; created inside
                         * urbi_emit_subscript_set_arm to pin recv/index temps so the
                         * compound-subscript lowering evaluates each exactly once.
                         * Lowers to OP_MOVE (or no-op when target == source).
                         * Not serialised; not visible to the parser. */
    /* === end v0.10.7 === */
    ,
    /* === v1.0-rc stdlib-completeness: short-circuit logical operators === */
    AST_LOGICAL  = 48   /* a && b / a || b — short-circuit.  Distinct from
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

    /* v0.5.7 additions */
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

    /* === v0.10.5: list/dict literal + subscript errors === */
    PARSE_EXPECTED_RBRACKET,    /* missing `]` in list/dict literal or subscript */
    PARSE_DICT_EXPECTED_FAT_ARROW, /* dict literal: `key` not followed by `=>` */
    PARSE_SUBSCRIPT_EXPECTED_RBRACKET, /* `l[i` missing `]` */
    PARSE_VAR_OBJ_SLOT_NO_INIT,        /* `var obj.slot` with no `= value` */
    PARSE_SUBSCRIPT_COMPOUND_OP_V1X,   /* compound subscript op other than +=
                                        * (e.g. -=, *=) — deferred to v1.x */
    /* === end v0.10.5 === */

    /* === v0.10.5: control flow === */
    PARSE_FOR_EXPECTED_VAR,            /* for loop header missing `var` keyword */
    PARSE_FOR_EXPECTED_COLON_OR_IN,    /* for (var x ...) — missing `:` or `in` */
    PARSE_BREAK_OUTSIDE_LOOP,          /* `break` not inside a for/while loop */
    PARSE_CONTINUE_OUTSIDE_LOOP,       /* `continue` not inside a for/while loop */
    PARSE_SWITCH_EXPECTED_CASE,        /* switch body contains non-case statement */
    PARSE_SWITCH_EXPECTED_COLON,       /* case label missing trailing `:` */
    /* === v0.13.5: switch default arm === */
    PARSE_SWITCH_DUPLICATE_DEFAULT,    /* switch body has more than one default: arm */
    /* === end v0.13.5: switch default arm === */
    /* === end v0.10.5: control flow === */

    /* === v0.10.5: event payload binding === */
    PARSE_EVENT_PAYLOAD_BIND_EXPECTED_VAR,    /* `at (e?(x))` — must be `(var x)` */
    PARSE_EVENT_PAYLOAD_BIND_EXPECTED_IDENT,  /* `at (e?(var))` — identifier missing */
    PARSE_EVENT_PAYLOAD_BIND_EXPECTED_RPAREN  /* `at (e?(var x` — missing `)` */
    /* === end v0.10.5 === */
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
 *   u.tag_prefix  — AST_TAG_PREFIX: tag-scope (mytag: { body }); onleave is v1.x
 *   u.member      — AST_MEMBER_GET, AST_MEMBER_SET: slot read / slot assignment
 *   u.prop        — AST_PROP_GET, AST_PROP_SET: slot-property read / assignment
 *   u.watcher     — AST_WATCHER:         at/at sync/whenever + optional onleave
 *   u.waituntil   — AST_WAITUNTIL:       cond-only waituntil
 *   u.at_event    — AST_AT_EVENT:        at (e?) event-subscribe form
 *   u.at_slot_change — AST_AT_SLOT_CHANGE: at (obj.x.changed?) slot-change form
 *   u.str_lit     — AST_STR:             escape-resolved string bytes view
 *   u.class_decl  — AST_CLASS_DECL:      class name + protos + body block
 *   u.property_decl — AST_PROPERTY_DECL: get/set sugar — receiver + slot
 *                                        name + getter/setter kind + params
 *                                        + body
 *   u.assert_stmt — AST_ASSERT:          expression/block + source text span
 *   u.for_each    — AST_FOR_EACH:        var name + iterable + body block
 *   [none]        — AST_BREAK:           no payload (exits innermost loop)
 *   [none]        — AST_CONTINUE:        no payload (next iteration)
 *   u.switch_stmt — AST_SWITCH:          expr + parallel arrays of vals + bodies
 *   u.list_lit    — AST_LIST_LIT:        arena array of element nodes
 *   u.dict_lit    — AST_DICT_LIT:        arena arrays of key + value nodes
 *   u.subscript   — AST_SUBSCRIPT_GET, AST_SUBSCRIPT_SET:
 *                                        recv + index + (SET: value + compound_op)
 *
 * Slot/prop name storage: zero-copy lexeme view (name_start + name_len), as
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
            UAstNode   *default_expr;      /* `= expr` default value or NULL
                                              (v0.13.5 legacy formal defaults;
                                              evaluated at call time in the
                                              callee scope when the caller
                                              omits the argument) */
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
        struct {                                            /* AST_PROP_GET, AST_PROP_SET */
            UAstNode   *recv;              /* the obj.x sub-expression (typically AST_MEMBER_GET) */
            const char *prop_name_start;   /* zero-copy lexeme view */
            int         prop_name_len;
            UAstNode   *value;             /* SET only; NULL for GET */
        } prop;
        struct {                                            /* AST_WATCHER */
            UAstNode *cond;
            UAstNode *body;
            UAstNode *onleave;             /* nullable */
            UAstNode *else_body;           /* nullable; fires on falling edge
                                            * (WHENEVER mode only).  NULL for AT/AT_SYNC.
                                            * When non-NULL, takes precedence over onleave
                                            * as the alt closure passed to OP_WHENEVER_INSTALL. */
            int       mode;               /* UWATCHER_AT / UWATCHER_AT_SYNC /
                                           * UWATCHER_WHENEVER — int for now;
                                           * UWatcherMode enum lands */
        } watcher;
        struct {                                            /* AST_WAITUNTIL */
            UAstNode *cond;
            bool      is_event_form;       /* True when `waituntil (e?)` form */
            const char *payload_var_name;  /* User-given name from `(var x)`, or NULL */
            int         payload_var_len;
        } waituntil;
        struct {                                            /* AST_AT_EVENT */
            UAstNode *event_expr;          /* the `e` in `at (e?)` or `whenever (e?)` */
            UAstNode *body;
            UAstNode *onleave;             /* nullable */
            bool      is_sync;             /* `at sync (e?)` */
            bool      is_whenever;         /* whenever (e?) — re-fires on each emission;
                                            * no one-shot teardown (vs at (e?) which
                                            * fires once per emission but does not reset
                                            * cond state).  v0.10.2. */
            const char *payload_var_name;  /* User-given name from `at (e?(var x))`,
                                            * or NULL (body param name defaults to __payload) */
            int         payload_var_len;
        } at_event;
        struct {                                            /* AST_AT_SLOT_CHANGE */
            UAstNode   *receiver;          /* the `obj` in `at (obj.x.changed?)` */
            const char *slot_name;         /* zero-copy lexeme view */
            size_t      slot_name_len;
            UAstNode   *body;
            UAstNode   *onleave;           /* nullable */
            bool        is_sync;           /* `at sync (obj.x.changed?)` */
        } at_slot_change;
        struct {                                            /* AST_STR */
            const char *bytes;             /* arena-allocated escape-resolved
                                            * + concat-folded buffer; NOT
                                            * NUL-terminated; lifetime bound
                                            * to the parser's UArena */
            int         len;               /* byte count (excluding any NUL) */
        } str_lit;
        struct {                                            /* AST_CLASS_DECL */
            const char *name_start;        /* zero-copy lexeme view of class name */
            int         name_len;
            UAstNode  **protos;            /* arena array; NULL when no protos */
            int         proto_count;       /* number of protos in declaration order */
            UAstNode   *body;              /* AST_BLOCK */
        } class_decl;
        struct {                                            /* AST_PROPERTY_DECL */
            UAstNode      *recv;           /* explicit receiver (e.g. `Foo.`)
                                            * or NULL for class-body implicit-self */
            const char    *name_start;     /* slot name (zero-copy lexeme view) */
            int            name_len;
            UAstMethodKind kind;           /* UAST_METHOD_GETTER / UAST_METHOD_SETTER */
            UAstNode      *func;           /* AST_FUNCTION carrying params + body */
        } property_decl;
        /* === v0.10.5: assert keyword === */
        struct {                                            /* AST_ASSERT */
            UAstNode   *expr;              /* expression or block to assert */
            const char *src_text;          /* zero-copy source span (paren form);
                                            * NULL for block form */
            int         src_len;           /* byte count; 0 for block form */
        } assert_stmt;
        /* === v0.10.5: control flow === */
        struct {                                            /* AST_FOR_EACH */
            const char *var_name_start;  /* zero-copy lexeme view of loop variable */
            int         var_name_len;
            UAstNode   *iter_expr;       /* iterable expression (evaluated once) */
            UAstNode   *body;            /* AST_BLOCK — loop body */
        } for_each;
        /* AST_BREAK and AST_CONTINUE carry no payload beyond line/col */
        struct {                                            /* AST_SWITCH */
            UAstNode   *expr;            /* switch expression (evaluated once) */
            UAstNode  **case_vals;       /* arena array of case value expressions */
            UAstNode  **case_bodies;     /* arena array of case body blocks */
            int         case_count;
            UAstNode   *default_body;    /* catch-all arm body; NULL if absent */
        } switch_stmt;
        /* === end v0.10.5: control flow === */

        /* === v0.10.5: list/dict literals + subscript === */
        struct {                                            /* AST_LIST_LIT */
            UAstNode  **elems;             /* arena array of element expressions */
            int         count;             /* number of elements (0 for []) */
        } list_lit;
        struct {                                            /* AST_DICT_LIT */
            UAstNode  **keys;              /* arena array of key expressions */
            UAstNode  **vals;              /* arena array of value expressions */
            int         count;             /* number of key-value pairs (0 for [=>]) */
        } dict_lit;
        struct {                           /* AST_SUBSCRIPT_GET, AST_SUBSCRIPT_SET */
            UAstNode   *recv;              /* the list/dict expression */
            UAstNode   *index;             /* the subscript index */
            UAstNode   *value;             /* SET only: rhs value; NULL for GET */
            bool        is_compound_add;   /* true when desugared from `l[i] += v` */
        } subscript;
        /* === end v0.10.5 === */

        /* === v0.10.7: synthetic register-reference leaf === */
        struct {                           /* AST_REG_REF */
            uint8_t reg;                   /* register index to reference */
        } reg_ref;
        /* === end v0.10.7 === */
    } u;
};

#ifdef __cplusplus
}
#endif

#endif
