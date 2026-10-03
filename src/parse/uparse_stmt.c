/* SPDX-License-Identifier: BSD-3-Clause */

#include "parse/uparse.h"
#include "parse/uparse_internal.h"
#include "lex/ulex.h"
#include "parse/uast.h"
#include "util/uarena.h"
#include "util/umacros.h"   /* urbi_memcpy */
#include <stddef.h>

/* Forward declarations for static helpers defined later in this file. */
static UAstNode *parse_return(UParser *p);
static UAstNode *parse_for(UParser *p);
static UAstNode *parse_break(UParser *p);
static UAstNode *parse_continue(UParser *p);
static UAstNode *parse_switch(UParser *p);

/* --- parse_var_decl: `var x = expr` --- */

static UAstNode *parse_var_decl(UParser *p) {
    UToken kw = urbi_parse_consume(p);          /* urbi_parse_consume TOK_KW_VAR */
    UToken name = urbi_parse_peek(p);

    if (name.type == TOK_KW_AT       || name.type == TOK_KW_WHENEVER  ||
        name.type == TOK_KW_WAITUNTIL || name.type == TOK_KW_ONLEAVE  ||
        name.type == TOK_KW_SYNC      || name.type == TOK_KW_ASYNC) {
        return urbi_parse_make_error(p, PARSE_RESERVED_KEYWORD_AS_IDENT,
                          urbi_parse_kErrorMessages[PARSE_RESERVED_KEYWORD_AS_IDENT],
                          name.line, name.col);
    }

    { UAstNode *err = NULL; if (!expect(p, TOK_IDENT, PARSE_EXPECTED_IDENT, &err)) return err; }

    /* If the token after the IDENT is TOK_DOT (not TOK_EQ), this is the
     * legacy slot-install form `var obj.slot = value`.  Desugar to
     * `obj.slot = value` (AST_MEMBER_SET): OP_SETSLOT installs the slot
     * when absent, so no new opcode is needed.
     *
     * Handles arbitrarily deep chains: `var a.b.c = v` →
     *   temp = a.b  (AST_MEMBER_GET)
     *   temp.c = v  (AST_MEMBER_SET)
     * achieved naturally by building a full `a.b.c = v` AST_MEMBER_SET
     * tree where the receiver is AST_MEMBER_GET for `a.b`.
     */
    if (urbi_parse_peek(p).type == TOK_DOT) {
        /* Build receiver node from the already-consumed IDENT. */
        UAstNode *recv = urbi_parse_make_ident(p, name.u.str.start, name.u.str.len,
                                    name.line, name.col);
        if (!recv) return NULL;
        /* Parse one or more `.slot` member-access suffixes.  After the final
         * DOT we expect `IDENT = value`; intermediate DOTs extend the chain. */
        for (;;) {
            urbi_parse_consume(p);  /* urbi_parse_consume TOK_DOT */
            UToken slot_name = urbi_parse_peek(p);
            { UAstNode *err = NULL; if (!expect(p, TOK_IDENT, PARSE_EXPECTED_IDENT, &err)) return err; }
            if (urbi_parse_peek(p).type == TOK_DOT) {
                /* Intermediate: build MEMBER_GET and continue. */
                UAstNode *mg = urbi_parse_make_node(p, AST_MEMBER_GET,
                                         slot_name.line, slot_name.col);
                if (!mg) return NULL;
                mg->u.member.recv       = recv;
                mg->u.member.name_start = slot_name.u.str.start;
                mg->u.member.name_len   = slot_name.u.str.len;
                mg->u.member.value      = NULL;
                recv = mg;
                continue;
            }
            /* Final slot: urbi_parse_consume `=` and parse RHS, produce MEMBER_SET. */
            { UAstNode *err = NULL; if (!expect(p, TOK_EQ, PARSE_VAR_OBJ_SLOT_NO_INIT, &err)) return err; }
            UAstNode *val = urbi_parse_expression(p, 0);
            if (!val) return NULL;
            if (val->kind == AST_ERROR) return val;
            UAstNode *ms = urbi_parse_make_node(p, AST_MEMBER_SET,
                                      slot_name.line, slot_name.col);
            if (!ms) return NULL;
            ms->u.member.recv       = recv;
            ms->u.member.name_start = slot_name.u.str.start;
            ms->u.member.name_len   = slot_name.u.str.len;
            ms->u.member.value      = val;
            return ms;
        }
    }

    UToken eq = urbi_parse_peek(p);
    UAstNode *init;
    if (eq.type != TOK_EQ) {
        /* No initializer — `var x;` declares to nil.
         * The reference initializes to void; we have no void model. */
        init = urbi_parse_make_nil_node(p, name.line, name.col);
        if (!init) return NULL;
    } else {
        urbi_parse_consume(p);
        init = urbi_parse_expression(p, 0);
        if (!init) return NULL;
        if (init->kind == AST_ERROR) return init;
    }

    UAstNode *node = urbi_parse_make_node(p, AST_VAR_DECL, kw.line, kw.col);
    if (!node) return NULL;
    node->u.var_decl.name_start = name.u.str.start;
    node->u.var_decl.name_len   = name.u.str.len;
    node->u.var_decl.init       = init;
    return node;
}

/* --- parse_assign_after_eq_peek: `x = expr`.
 *
 * Caller contract:
 *   - `name` is the already-consumed IDENT token (passed by value).
 *   - The next lexer token MUST be TOK_EQ; the caller has already
 *     peeked and confirmed it.  This function consumes the TOK_EQ
 *     and parses the RHS.  Calling it without that hidden lookahead
 *     state would mis-parse the expression.
 *
 * The function name encodes that lexer-state precondition explicitly —
 * earlier name `parse_assign_from_ident` did not. --- */

static UAstNode *parse_assign_after_eq_peek(UParser *p, UToken name) {
    /* TOK_EQ already peeked/confirmed by caller; urbi_parse_consume it. */
    urbi_parse_consume(p);

    /* Parse RHS as a Pratt expression, NOT urbi_parse_inner_tier.
     * `x = 1 | y = 2` should parse as `(x = 1) | (y = 2)`.  Without
     * this, urbi_parse_inner_tier absorbs the `|` into the assign RHS. */
    UAstNode *value = urbi_parse_expression(p, 0);
    if (!value) return NULL;
    if (value->kind == AST_ERROR) return value;

    UAstNode *node = urbi_parse_make_node(p, AST_ASSIGN, name.line, name.col);
    if (!node) return NULL;
    node->u.assign.name_start = name.u.str.start;
    node->u.assign.name_len   = name.u.str.len;
    node->u.assign.value      = value;
    return node;
}

/* parse_assign_or_expr_impl: IDENT already consumed as `name`.
   Handles assignment (`x = expr`), tag-prefix (`mytag: { body }` and
   `expr: { body }`), and expression statements that start with an
   identifier (call chains, member accesses, arithmetic).
   The non-assign/non-tag path runs urbi_parse_expression_cont to
   finish the Pratt climb, then urbi_parse_pipe_amp_fold for `|`/`&`.

   fold: when false, skip the trailing urbi_parse_pipe_amp_fold so that any `|`/`&`
   after the expression is left for the enclosing statement-level fold.
   Used from parse_arm_stmt (arm context — `&` must bind
   OUTSIDE the unbraced arm per ugrammar.y :374-378 cstmt tier). */
static UAstNode *parse_assign_or_expr_impl(UParser *p, UToken name, bool fold) {
    /* Getter/setter statement-start sugar: `get IDENT (...)` or
     * `set IDENT (...)` desugars to a `setProperty` call (see
     * urbi_parse_property_decl).  Recognized only in the strict 3-token
     * shape `get|set IDENT (`; outside that pattern, `get`/`set` remain
     * plain identifiers (no keyword reservation breakage).  The
     * receiver is implicit (NULL); urbi_parse_property_decl resolves it
     * from the enclosing class body's hidden receiver name. */
    if ((urbi_parse_ident_equals(name.u.str.start, name.u.str.len, "get", 3) ||
         urbi_parse_ident_equals(name.u.str.start, name.u.str.len, "set", 3))
        && urbi_parse_peek(p).type == TOK_IDENT && urbi_parse_peek2(p).type == TOK_LPAREN) {
        /* Getter/setter (follow-up): the implicit-receiver form is legal
         * only inside a `class { ... }` body.  At statement start there
         * is no v1.0 resolver for the implicit `this`; reject with a
         * dedicated diagnostic instead of falling through to a generic
         * EMIT_UNSUPPORTED_AST at emit time.  Deferred to v1.x. */
        if (p->class_body_depth == 0) {
            return urbi_parse_make_error(p, PARSE_TOPLEVEL_GETSET_NOT_SUPPORTED,
                              urbi_parse_kErrorMessages[PARSE_TOPLEVEL_GETSET_NOT_SUPPORTED],
                              name.line, name.col);
        }
        UAstMethodKind kind =
            urbi_parse_ident_equals(name.u.str.start, name.u.str.len, "get", 3)
                ? UAST_METHOD_GETTER : UAST_METHOD_SETTER;
        UToken slot_name = urbi_parse_consume(p);  /* urbi_parse_consume the slot-name IDENT */
        return urbi_parse_property_decl(p, /*recv=*/NULL, slot_name, kind,
                                   name.line, name.col);
    }

    if (urbi_parse_peek(p).type == TOK_EQ) {
        return parse_assign_after_eq_peek(p, name);
    }
    /* Tag-prefix: `mytag: { body }`.  At statement level, `:` has no
     * other meaning (not an infix operator, not a separator), so seeing
     * IDENT followed by COLON unambiguously introduces a tag scope. */
    if (urbi_parse_peek(p).type == TOK_COLON) {
        return urbi_parse_tag_prefix(p, name);
    }
    UAstNode *lhs = urbi_parse_make_ident(p, name.u.str.start, name.u.str.len,
                               name.line, name.col);
    if (!lhs) return NULL;
    /* Pratt climb: builds full postfix chain from the leading IDENT. */
    lhs = urbi_parse_expression_cont(p, lhs, 0);
    if (!lhs) return NULL;
    if (lhs->kind == AST_ERROR) return lhs;
    if (urbi_parse_peek(p).type == TOK_COLON) {
        return urbi_parse_tag_prefix_from_expr(p, lhs);
    }
    /* Normal expression statement: fold `|` and `&` separators (unless
     * called from an arm context, where the enclosing fold handles them). */
    if (fold) return urbi_parse_pipe_amp_fold(p, lhs);
    return lhs;
}

/* Public wrapper — always folds (statement-level behaviour). */
static UAstNode *parse_assign_or_expr(UParser *p, UToken name) {
    return parse_assign_or_expr_impl(p, name, /*fold=*/true);
}

/* Flatten `;`/`,`/`|`/`&`-folded statements (AST_SEQ, any separator) in
 * a class body into one leaf-statement list, growing *out via the
 * arena as needed.  Mirrors the recursion the old hand-rolled class
 * emitter did over AST_BIN_SEP/AST_NARY, now unified as one AST_SEQ
 * kind. */
static bool flatten_class_body_stmt(UParser *p, UAstNode *stmt,
                                     UAstNode ***out, int *count, int *cap) {
    if (stmt->kind == AST_SEQ) {
        for (int i = 0; i < stmt->u.seq.count; i++) {
            if (!flatten_class_body_stmt(p, stmt->u.seq.children[i], out, count, cap))
                return false;
        }
        return true;
    }
    if (*count == *cap) {
        if (!urbi_parse_arena_grow_node_array(p, out, cap, *count)) return false;
    }
    (*out)[(*count)++] = stmt;
    return true;
}

/* True when `n` is the CALL shape urbi_parse_property_decl produces:
 * recv.setProperty(name, "oget"|"oset", function). */
static bool is_setproperty_call(const UAstNode *n) {
    if (n->kind != AST_CALL || n->u.call.arg_count != 3) return false;
    const UAstNode *callee = n->u.call.callee;
    return callee->kind == AST_MEMBER_GET
        && urbi_parse_ident_equals(callee->u.member.name_start,
                                    callee->u.member.name_len,
                                    "setProperty", 11);
}

/* --- parse_class_declaration: `class Name [: public P1, P2, ...] { body }`.
 *
 * Lowers to:
 *   var Name = BLOCK {
 *     var $cls = Object.clone();
 *     $cls.protos.insertFront(Pn); ...; $cls.protos.insertFront(P1);
 *     <one statement per body leaf, $cls as receiver>;
 *     $cls
 *   }
 *
 * `$cls` is a hidden per-declaration name (urbi_parse_hidden_name), not
 * the class's own source name — every proto and body reference inside
 * this BLOCK goes through $cls, never through `Name`.  `Name` is bound
 * exactly once, by the OUTER var-decl, to the block's value — the block's
 * last statement is the bare `$cls` reference, so the finished object is
 * what `Name` ends up holding.
 *
 * Both halves of that split matter:
 *   - Per S-class-name-scope, `class a : public a { ... }` must resolve
 *     the proto `a` to the OUTER `a`.  The initializer (the whole BLOCK,
 *     built entirely against $cls) is fully evaluated before the outer
 *     var-decl binds `Name`, so the outer `a` stays visible while the
 *     inner one is under construction.
 *   - `var Name = <block>` is an ordinary variable declaration — at
 *     chunk top it becomes a realm global through the same write path
 *     every other top-level `var` in stdlib.u already uses; inside a
 *     function it is a plain local.  Neither path depends on the
 *     identifier `Realm` resolving to anything, so a class can declare
 *     itself even while stdlib.u itself is still booting (no realm
 *     exists yet).
 *
 * The `public` keyword is required after the colon for syntactic
 * compatibility with legacy urbi 2.x (which had access modifiers); v1.0
 * has no access semantics but the keyword stays as a layout marker.
 *
 * Proto identifiers parse as full primary expressions so that future
 * v1.x extensions (e.g. `class C : public M.Inner`) just work; today
 * we expect AST_IDENT but do not gate on it. --- */
static UAstNode *parse_class_declaration(UParser *p) {
    UToken kw = urbi_parse_consume(p);  /* urbi_parse_consume TOK_KW_CLASS */

    /* Class name. */
    UToken name = urbi_parse_peek(p);
    { UAstNode *err = NULL; if (!expect(p, TOK_IDENT, PARSE_EXPECTED_IDENT, &err)) return err; }

    /* Optional `: public P1[, P2, ...]`. */
    UAstNode **protos = NULL;
    int proto_count = 0;
    if (urbi_parse_peek(p).type == TOK_COLON) {
        urbi_parse_consume(p);  /* urbi_parse_consume ':' */
        UToken pub_tok = urbi_parse_peek(p);
        if (pub_tok.type != TOK_KW_PUBLIC) {
            return urbi_parse_make_error(p, PARSE_UNEXPECTED_TOKEN,
                              "expected 'public' after ':' in class declaration",
                              pub_tok.line, pub_tok.col);
        }
        urbi_parse_consume(p);  /* urbi_parse_consume 'public' */

        /* Seed an initial proto array.  urbi_parse_prefix gives a single primary
         * expression — not a separator-tier or comma-tier expression — so
         * the comma stays available as the proto-list separator. */
        int proto_cap = 4;
        protos = (UAstNode **)uarena_alloc(p->arena,
                                           (size_t)proto_cap * sizeof(UAstNode *));
        if (protos == NULL) return (UAstNode *)&uparser_oom_sentinel;

        for (;;) {
            UAstNode *proto = urbi_parse_prefix(p);
            if (proto == NULL) return NULL;
            if (proto->kind == AST_ERROR) return proto;

            if (proto_count == proto_cap) {
                if (!urbi_parse_arena_grow_node_array(p, &protos, &proto_cap,
                                           proto_count)) {
                    return (UAstNode *)&uparser_oom_sentinel;
                }
            }
            protos[proto_count++] = proto;

            if (urbi_parse_peek(p).type != TOK_COMMA) break;
            urbi_parse_consume(p);  /* urbi_parse_consume ',' */
        }
    }

    /* Body — block.  class_body_name is the hidden $cls receiver every
     * implicit get/set sugar inside the body resolves against (see
     * urbi_parse_property_decl); class_body_depth gates the
     * implicit-receiver form itself (illegal outside a class body).
     * Both are saved/restored around the body parse so a nested class
     * declaration layers correctly. */
    UToken body_tok = urbi_parse_peek(p);
    if (body_tok.type != TOK_LBRACE) {
        return urbi_parse_make_error(p, PARSE_EXPECTED_LBRACE,
                          urbi_parse_kErrorMessages[PARSE_EXPECTED_LBRACE],
                          body_tok.line, body_tok.col);
    }

    int cls_len;
    const char *cls_name = urbi_parse_hidden_name(p, "cls", &cls_len);
    if (!cls_name) return (UAstNode *)&uparser_oom_sentinel;

    const char *saved_name_start = p->class_body_name_start;
    int         saved_name_len   = p->class_body_name_len;
    p->class_body_name_start = cls_name;
    p->class_body_name_len   = cls_len;
    p->class_body_depth++;
    UAstNode *body = urbi_parse_block(p);
    p->class_body_depth--;
    p->class_body_name_start = saved_name_start;
    p->class_body_name_len   = saved_name_len;
    if (body == NULL) return NULL;
    if (body->kind == AST_ERROR) return body;

    int leaf_cap = 8, leaf_count = 0;
    UAstNode **leaves = (UAstNode **)uarena_alloc(p->arena,
                                                   (size_t)leaf_cap * sizeof(UAstNode *));
    if (!leaves) return (UAstNode *)&uparser_oom_sentinel;
    for (int i = 0; i < body->u.block.count; i++) {
        if (!flatten_class_body_stmt(p, body->u.block.stmts[i], &leaves, &leaf_count, &leaf_cap))
            return (UAstNode *)&uparser_oom_sentinel;
    }

    int total = 1 + proto_count + leaf_count + 1;
    UAstNode **out = (UAstNode **)uarena_alloc(p->arena, (size_t)total * sizeof(UAstNode *));
    if (!out) return (UAstNode *)&uparser_oom_sentinel;
    int oi = 0;

    /* var $cls = Object.clone(); */
    {
        UAstNode *obj_ident = urbi_parse_desugar_ident(p, "Object", 6, kw.line, kw.col);
        if (!obj_ident) return NULL;
        UAstNode *clone_mg = urbi_parse_desugar_member_get(p, obj_ident, "clone", 5,
                                                            kw.line, kw.col);
        if (!clone_mg) return NULL;
        UAstNode *clone_call = urbi_parse_desugar_call(p, clone_mg, NULL, 0, kw.line, kw.col);
        if (!clone_call) return NULL;
        UAstNode *var_cls = urbi_parse_desugar_var_decl(p, cls_name, cls_len, clone_call,
                                                         kw.line, kw.col);
        if (!var_cls) return NULL;
        out[oi++] = var_cls;
    }

    /* $cls.protos().insertFront(Pn); ...; $cls.protos().insertFront(P1);
     * (reverse declaration order so the final chain reads [P1, P2, ...]).
     * `protos` is a native method (returns the synthetic proto-list
     * object) — it must be CALLED, not read as a plain slot. */
    for (int i = proto_count - 1; i >= 0; i--) {
        UAstNode *cls_ref = urbi_parse_desugar_ident(p, cls_name, cls_len, kw.line, kw.col);
        if (!cls_ref) return NULL;
        UAstNode *protos_mg = urbi_parse_desugar_member_get(p, cls_ref, "protos", 6,
                                                             kw.line, kw.col);
        if (!protos_mg) return NULL;
        UAstNode *protos_call = urbi_parse_desugar_call(p, protos_mg, NULL, 0, kw.line, kw.col);
        if (!protos_call) return NULL;
        UAstNode *insert_mg = urbi_parse_desugar_member_get(p, protos_call, "insertFront", 11,
                                                             kw.line, kw.col);
        if (!insert_mg) return NULL;
        UAstNode **args = (UAstNode **)uarena_alloc(p->arena, sizeof(UAstNode *));
        if (!args) return (UAstNode *)&uparser_oom_sentinel;
        args[0] = protos[i];
        UAstNode *call = urbi_parse_desugar_call(p, insert_mg, args, 1, kw.line, kw.col);
        if (!call) return NULL;
        out[oi++] = call;
    }

    /* Body leaves: `var x = e` -> $cls.x = e; `get`/`set` sugar already
     * built the setProperty call against $cls (urbi_parse_property_decl)
     * and passes through unchanged; anything else is not allowed here. */
    for (int i = 0; i < leaf_count; i++) {
        UAstNode *leaf = leaves[i];
        if (leaf->kind == AST_VAR_DECL) {
            UAstNode *cls_ref = urbi_parse_desugar_ident(p, cls_name, cls_len,
                                                          leaf->line, leaf->col);
            if (!cls_ref) return NULL;
            UAstNode *ms = urbi_parse_desugar_member_set(p, cls_ref,
                                                          leaf->u.var_decl.name_start,
                                                          leaf->u.var_decl.name_len,
                                                          leaf->u.var_decl.init,
                                                          leaf->line, leaf->col);
            if (!ms) return NULL;
            out[oi++] = ms;
        } else if (is_setproperty_call(leaf)) {
            out[oi++] = leaf;
        } else {
            return urbi_parse_make_error(p, PARSE_CLASS_BODY_STATEMENT,
                              urbi_parse_kErrorMessages[PARSE_CLASS_BODY_STATEMENT],
                              leaf->line, leaf->col);
        }
    }

    /* $cls — the block's value is the finished object; the outer
     * var-decl built below binds `Name` to it. */
    {
        UAstNode *cls_ref = urbi_parse_desugar_ident(p, cls_name, cls_len, kw.line, kw.col);
        if (!cls_ref) return NULL;
        out[oi++] = cls_ref;
    }

    UAstNode *inner_block = urbi_parse_desugar_block(p, out, total, kw.line, kw.col);
    if (!inner_block) return NULL;
    return urbi_parse_desugar_var_decl(p, name.u.str.start, name.u.str.len,
                                        inner_block, kw.line, kw.col);
}

/* --- urbi_parse_statement_or_expr: var-decl, assign, or inner-tier expression.
   Returns an inner-tier result (arithmetic expression, possibly with
   | / & separators). Used as the child-entry point for both
   uparse_next_statement and the outer-tier loop.

   NOTE: parse_arm_stmt (below) mirrors this keyword dispatch for unbraced
   if/while/else arms, minus the urbi_parse_pipe_amp_fold calls.  A new statement
   keyword added here must be added there too. --- */

UAstNode *urbi_parse_statement_or_expr(UParser *p) {
    UToken t = urbi_parse_peek(p);

    switch (t.type) {
    /* After parsing a block or block-like statement form, fold any following
     * `|` / `&` separator so that `{a} & {b}` and `if (c) {b} & {e}` parse
     * as AST_SEQ nodes.  Parallel var-declare (`var a = 1 & var b = 2`)
     * stays rejected — var is not in this set. */
    case TOK_KW_WHILE: {
        UAstNode *node = urbi_parse_while(p);
        if (!node || node->kind == AST_ERROR) return node;
        return urbi_parse_pipe_amp_fold(p, node);
    }
    case TOK_KW_IF: {
        UAstNode *node = urbi_parse_if(p);
        if (!node || node->kind == AST_ERROR) return node;
        return urbi_parse_pipe_amp_fold(p, node);
    }
    case TOK_KW_VAR:      return parse_var_decl(p);
    case TOK_KW_RETURN:   return parse_return(p);
    case TOK_KW_TRY:      return urbi_parse_try(p);
    case TOK_KW_THROW:    return urbi_parse_throw(p);
    /* at/whenever/every each parse their body (and onleave/else) through
     * urbi_parse_arm_stmt now — fold-free, same as an if/while arm — so a
     * trailing `|`/`&` is left for THIS fold to bind outside the watcher,
     * exactly like the TOK_KW_IF/TOK_KW_WHILE cases above. */
    case TOK_KW_AT: {
        UAstNode *node = urbi_parse_at(p);
        if (!node || node->kind == AST_ERROR) return node;
        return urbi_parse_pipe_amp_fold(p, node);
    }
    case TOK_KW_WHENEVER: {
        UAstNode *node = urbi_parse_whenever(p);
        if (!node || node->kind == AST_ERROR) return node;
        return urbi_parse_pipe_amp_fold(p, node);
    }
    case TOK_KW_WAITUNTIL: return urbi_parse_waituntil(p);
    case TOK_KW_EVERY: {
        UAstNode *node = urbi_parse_every(p);
        if (!node || node->kind == AST_ERROR) return node;
        return urbi_parse_pipe_amp_fold(p, node);
    }
    case TOK_KW_CLASS:    return parse_class_declaration(p);
    case TOK_KW_ASSERT:   return urbi_parse_assert(p);
    case TOK_KW_FOR:      return parse_for(p);
    case TOK_KW_BREAK:    return parse_break(p);
    case TOK_KW_CONTINUE: return parse_continue(p);
    case TOK_KW_SWITCH:   return parse_switch(p);
    /* detach/disown are primary-exp atoms (see uparse_expr.c); listed here
     * explicitly, same as TOK_KW_WAITUNTIL above, rather than relying on
     * the default case to reach them through urbi_parse_atom. */
    case TOK_KW_DETACH:   return urbi_parse_detach(p);
    case TOK_KW_DISOWN:   return urbi_parse_disown(p);
    case TOK_LBRACE: {
        UAstNode *block = urbi_parse_block(p);
        if (!block || block->kind == AST_ERROR) return block;
        return urbi_parse_pipe_amp_fold(p, block);
    }
    case TOK_IDENT: {
        /* x = expr — detect by consuming IDENT then peeking for TOK_EQ.
           mytag: { body } — detect by consuming IDENT then peeking for TOK_COLON.
           If neither, continue as a Pratt expression starting with this IDENT. */
        UToken name = urbi_parse_consume(p);
        return parse_assign_or_expr(p, name);
    }
    default:
        return urbi_parse_inner_tier(p);
    }
}

/* --- urbi_parse_block: `{` stmts `}` → AST_BLOCK.
   Each statement inside is a full outer-tier parse (including `;` chains).
   Statements are separated by `;` or `|`; a missing separator ends the block.
   Used by if/else; while and function will reuse this. --- */
UAstNode *urbi_parse_block(UParser *p) {
    UToken lbrace = urbi_parse_peek(p);
    { UAstNode *err = NULL; if (!expect(p, TOK_LBRACE, PARSE_EXPECTED_LBRACE, &err)) return err; }

    int cap = 4;
    UAstNode **stmts = (UAstNode **)uarena_alloc(p->arena,
                                                  (size_t)cap * sizeof(UAstNode *));
    if (!stmts) return (UAstNode *)&uparser_oom_sentinel;
    int count = 0;

    while (urbi_parse_peek(p).type != TOK_RBRACE && urbi_parse_peek(p).type != TOK_EOF) {
        UAstNode *s = urbi_parse_outer_tier(p);
        if (!s) return (UAstNode *)&uparser_oom_sentinel;
        if (s->kind == AST_ERROR) return s;

        if (count == cap) {
            if (!urbi_parse_arena_grow_node_array(p, &stmts, &cap, count))
                return (UAstNode *)&uparser_oom_sentinel;
        }
        stmts[count++] = s;

        /* Statements within a block are separated by `;` or `|`.
         * `|` acts as the REPL-boundary convention inside blocks too.
         * If neither is present, the block ends (next token is `}` or
         * an expression starting another statement — stop and expect `}`).
         */
        UToken sep = urbi_parse_peek(p);
        if (sep.type == TOK_SEMI || sep.type == TOK_PIPE) {
            urbi_parse_consume(p);
        } else {
            break;
        }
    }

    { UAstNode *err = NULL; if (!expect(p, TOK_RBRACE, PARSE_EXPECTED_RBRACE, &err)) return err; }

    UAstNode *node = urbi_parse_make_node(p, AST_BLOCK, lbrace.line, lbrace.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.block.stmts = stmts;
    node->u.block.count = count;
    return node;
}

/* parse_arm_stmt: parse exactly ONE statement in an unbraced if/while/else
 * arm context WITHOUT folding any trailing `|`/`&` separators.
 *
 * In the reference grammar (aldebaran-urbi/src/parser/ugrammar.y), the body
 * of if/while/else is a `stmt` (:771/:968/:848), while `|`/`&` compose only
 * at the looser `cstmt` tier (:374-378).  Both assignment forms and
 * expression statements are `exp` → `stmt`, so `&`/`|` always bind OUTSIDE
 * the arm regardless of LHS kind.  For example:
 *
 *   `if (c) o.x = 1 & b`  ≡  `(if (c) o.x = 1) & b`
 *   `if (c) x = 1 & b`    ≡  `(if (c) x = 1) & b`
 *
 * urbi_parse_statement_or_expr cannot be used directly here because it calls
 * urbi_parse_pipe_amp_fold on if/while/block results and parse_assign_or_expr calls it
 * on the expression path — both would absorb `&`/`|` into the arm.
 * parse_arm_stmt skips those folds so the enclosing urbi_parse_statement_or_expr
 * call (which wraps the whole if/while node) performs the fold instead.
 *
 * Exported as urbi_parse_arm_stmt: uparse_react.c reuses it for every
 * at/whenever/every body, onleave and else arm, for the same reason —
 * the reactive construct's own enclosing urbi_parse_statement_or_expr case
 * applies the fold, so a trailing `|`/`&` binds OUTSIDE the body instead
 * of being absorbed into it. */
UAstNode *urbi_parse_arm_stmt(UParser *p) {
    UToken t = urbi_parse_peek(p);
    switch (t.type) {
    /* Nested control-flow: return the node directly without urbi_parse_pipe_amp_fold.
     * The enclosing statement-level fold handles any trailing `|`/`&`. */
    case TOK_KW_WHILE: return urbi_parse_while(p);
    case TOK_KW_IF:    return urbi_parse_if(p);
    /* var / return / throw / try: never call urbi_parse_pipe_amp_fold — no change needed. */
    case TOK_KW_VAR:      return parse_var_decl(p);
    case TOK_KW_RETURN:   return parse_return(p);
    case TOK_KW_TRY:      return urbi_parse_try(p);
    case TOK_KW_THROW:    return urbi_parse_throw(p);
    case TOK_KW_AT:       return urbi_parse_at(p);
    case TOK_KW_WHENEVER: return urbi_parse_whenever(p);
    case TOK_KW_WAITUNTIL: return urbi_parse_waituntil(p);
    case TOK_KW_EVERY:    return urbi_parse_every(p);
    case TOK_KW_CLASS:    return parse_class_declaration(p);
    case TOK_KW_ASSERT:   return urbi_parse_assert(p);
    case TOK_KW_FOR:      return parse_for(p);
    case TOK_KW_BREAK:    return parse_break(p);
    case TOK_KW_CONTINUE: return parse_continue(p);
    case TOK_KW_SWITCH:   return parse_switch(p);
    /* detach/disown are primary-exp atoms (see uparse_expr.c); listed here
     * explicitly, same as TOK_KW_WAITUNTIL above. */
    case TOK_KW_DETACH:   return urbi_parse_detach(p);
    case TOK_KW_DISOWN:   return urbi_parse_disown(p);
    /* Braced block: return without urbi_parse_pipe_amp_fold (same as TOK_KW_IF above).
     * Defensive only — every callsite (urbi_parse_if then/else arms, urbi_parse_while
     * body) routes TOK_LBRACE to urbi_parse_block directly and never enters
     * parse_single_stmt_as_block; kept so parse_arm_stmt's contract does
     * not silently depend on that callsite urbi_parse_peek check. */
    case TOK_LBRACE:   return urbi_parse_block(p);
    case TOK_IDENT: {
        UToken name = urbi_parse_consume(p);
        /* fold=false: leave `|`/`&` for the enclosing statement fold. */
        return parse_assign_or_expr_impl(p, name, /*fold=*/false);
    }
    default:
        /* Literal, prefix op, parenthesized expression, etc.
         * NOT urbi_parse_inner_tier — that one calls urbi_parse_pipe_amp_fold and would
         * absorb `&`/`|` into the arm (`if (false) 42 & { b }` folded
         * inside pre-fix).  urbi_parse_expression is the fold-free Pratt tier,
         * matching the fold=false IDENT path above. */
        return urbi_parse_expression(p, 0);
    }
}

/* parse_single_stmt_as_block: parse ONE statement and wrap it in a synthetic
 * single-element AST_BLOCK.  Used for unbraced if/while/else arms:
 *   `if (c) stmt`        →  `if (c) { stmt }`  (same AST downstream)
 *   `while (c) stmt`     →  `while (c) { stmt }`
 *   `else stmt`          →  `else { stmt }`
 *
 * "One statement" means exactly what parse_arm_stmt returns — a single
 * assignment, expression, or nested control-flow construct.
 *
 * Separator binding: trailing `|`/`&` after the arm are left unconsumed by
 * parse_arm_stmt (every dispatch path in it is fold-free).  The caller's
 * urbi_parse_pipe_amp_fold (wrapping the whole if/while node in urbi_parse_statement_or_expr)
 * folds them OUTSIDE the arm, matching the reference grammar's stmt-vs-cstmt
 * tiering (ugrammar.y :374-378).  This applies to ALL arm kinds —
 * simple-assign, member-assign, call, literal, prefix-op, parenthesized,
 * and nested control-flow.
 *
 * Dangling else: always binds to the nearest if (standard C-style) because
 * the recursive urbi_parse_if inside parse_arm_stmt consumes the else before
 * returning. */
static UAstNode *parse_single_stmt_as_block(UParser *p) {
    UToken pos = urbi_parse_peek(p);
    UAstNode *stmt = urbi_parse_arm_stmt(p);
    if (!stmt) return (UAstNode *)&uparser_oom_sentinel;
    if (stmt->kind == AST_ERROR) return stmt;

    UAstNode **stmts = (UAstNode **)uarena_alloc(p->arena, sizeof(UAstNode *));
    if (!stmts) return (UAstNode *)&uparser_oom_sentinel;
    stmts[0] = stmt;

    UAstNode *block = urbi_parse_make_node(p, AST_BLOCK, pos.line, pos.col);
    if (!block) return (UAstNode *)&uparser_oom_sentinel;
    block->u.block.stmts = stmts;
    block->u.block.count = 1;
    return block;
}

/* --- urbi_parse_while: `while` `(` cond `)` body-block --- */
UAstNode *urbi_parse_while(UParser *p) {
    UToken kw = urbi_parse_consume(p);  /* urbi_parse_consume TOK_KW_WHILE */

    { UAstNode *err = NULL; if (!expect(p, TOK_LPAREN, PARSE_EXPECTED_LPAREN, &err)) return err; }

    UAstNode *cond = urbi_parse_inner_tier(p);
    if (!cond) return (UAstNode *)&uparser_oom_sentinel;
    if (cond->kind == AST_ERROR) return cond;

    { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }

    p->loop_depth++;
    UAstNode *body = (urbi_parse_peek(p).type == TOK_LBRACE)
        ? urbi_parse_block(p)
        : parse_single_stmt_as_block(p);
    p->loop_depth--;
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    UAstNode *node = urbi_parse_make_node(p, AST_WHILE, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.while_stmt.cond = cond;
    node->u.while_stmt.body = body;
    return node;
}

/* --- urbi_parse_if: `if` `(` cond `)` then-block [`else` else-block] --- */
UAstNode *urbi_parse_if(UParser *p) {
    UToken kw = urbi_parse_consume(p);  /* urbi_parse_consume TOK_KW_IF */

    { UAstNode *err = NULL; if (!expect(p, TOK_LPAREN, PARSE_EXPECTED_LPAREN, &err)) return err; }

    UAstNode *cond = urbi_parse_inner_tier(p);
    if (!cond) return (UAstNode *)&uparser_oom_sentinel;
    if (cond->kind == AST_ERROR) return cond;

    { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }

    /* Accept braced or unbraced single-statement then-arm.
     * Dangling else binds nearest if: the recursive urbi_parse_if call inside
     * parse_single_stmt_as_block will urbi_parse_consume the else before returning,
     * so the outer if sees no else (standard C-precedence). */
    UAstNode *then_block = (urbi_parse_peek(p).type == TOK_LBRACE)
        ? urbi_parse_block(p)
        : parse_single_stmt_as_block(p);
    if (!then_block) return (UAstNode *)&uparser_oom_sentinel;
    if (then_block->kind == AST_ERROR) return then_block;

    UAstNode *else_block = NULL;
    if (urbi_parse_peek(p).type == TOK_KW_ELSE) {
        urbi_parse_consume(p);
        /* Else arm may also be unbraced. */
        else_block = (urbi_parse_peek(p).type == TOK_LBRACE)
            ? urbi_parse_block(p)
            : parse_single_stmt_as_block(p);
        if (!else_block) return (UAstNode *)&uparser_oom_sentinel;
        if (else_block->kind == AST_ERROR) return else_block;
    }

    UAstNode *node = urbi_parse_make_node(p, AST_IF, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.if_stmt.cond       = cond;
    node->u.if_stmt.then_block = then_block;
    node->u.if_stmt.else_block = else_block;
    return node;
}

static UAstNode *reject_bare_function_forms(UParser *p) {
    UToken next = urbi_parse_peek(p);
    if (next.type == TOK_LBRACE) {
        return urbi_parse_make_error(p, PARSE_BARE_FUNCTION,
                          "bare 'function { body }' is retired at v1.0; "
                          "use 'function() { body }' (add empty parens). "
                          "Legacy bare functions ambiguously meant "
                          "either 0-arg or no-formals — v1.0 requires explicit parens",
                          next.line, next.col);
    }
    if (next.type == TOK_IDENT) {
        UToken name_tok = urbi_parse_consume(p);
        if (urbi_parse_peek(p).type == TOK_LBRACE) {
            return urbi_parse_make_error(p, PARSE_BARE_FUNCTION,
                              "bare 'function name { body }' is retired at v1.0; "
                              "use 'function name() { body }' (add empty parens)",
                              name_tok.line, name_tok.col);
        }
        return urbi_parse_make_error(p, PARSE_NAMED_FUNCTION_NOT_SUPPORTED,
                          urbi_parse_kErrorMessages[PARSE_NAMED_FUNCTION_NOT_SUPPORTED],
                          name_tok.line, name_tok.col);
    }
    return NULL;
}

/* Rejected on lazy params: the lazy convention wraps caller-side thunks,
 * which has no legacy default-value semantics.
 *
 * Returns NULL on success (default stored or absent); an AST_ERROR or
 * OOM-sentinel node on failure — mirror of the urbi_parse_make_error contract. */
static UAstNode *parse_optional_param_default(UParser *p, UAstNode *pn,
                                              bool is_lazy) {
    pn->u.param.default_expr = NULL;
    if (urbi_parse_peek(p).type != TOK_EQ) return NULL;
    UToken eq = urbi_parse_consume(p);
    if (is_lazy) {
        return urbi_parse_make_error(p, PARSE_UNEXPECTED_TOKEN,
                          "default value not supported on a lazy parameter",
                          eq.line, eq.col);
    }
    UAstNode *def = urbi_parse_expression(p, 0);
    if (!def) return (UAstNode *)&uparser_oom_sentinel;
    if (def->kind == AST_ERROR) return def;
    pn->u.param.default_expr = def;
    return NULL;
}

/* --- urbi_parse_function: `function` [`name`] `(` params `)` `{` body `}` --- */

UAstNode *urbi_parse_function(UParser *p) {
    UToken kw = urbi_parse_consume(p);   /* urbi_parse_consume TOK_KW_FUNCTION */

    UAstNode *err = reject_bare_function_forms(p);
    if (err) return err;

    { UAstNode *err = NULL; if (!expect(p, TOK_LPAREN, PARSE_EXPECTED_LPAREN, &err)) return err; }  /* urbi_parse_consume '(' */

    /* Parameter list. */
    int cap = 4;
    UAstNode **params = (UAstNode **)uarena_alloc(p->arena,
                                                   (size_t)cap * sizeof(UAstNode *));
    if (!params) return (UAstNode *)&uparser_oom_sentinel;
    int count = 0;

    while (urbi_parse_peek(p).type != TOK_RPAREN && urbi_parse_peek(p).type != TOK_EOF) {
        bool is_lazy = false;
        if (urbi_parse_peek(p).type == TOK_KW_LAZY) {
            urbi_parse_consume(p);
            is_lazy = true;
        }

        UToken name = urbi_parse_peek(p);
        { UAstNode *err = NULL; if (!expect(p, TOK_IDENT, PARSE_EXPECTED_IDENT, &err)) return err; }

        UAstNode *pn = urbi_parse_make_node(p, AST_PARAM, name.line, name.col);
        if (!pn) return (UAstNode *)&uparser_oom_sentinel;
        pn->u.param.name_start = name.u.str.start;
        pn->u.param.name_len   = name.u.str.len;
        pn->u.param.is_lazy    = is_lazy;

        UAstNode *derr = parse_optional_param_default(p, pn, is_lazy);
        if (derr) return derr;

        if (count == cap) {
            if (!urbi_parse_arena_grow_node_array(p, &params, &cap, count))
                return (UAstNode *)&uparser_oom_sentinel;
        }
        params[count++] = pn;

        if (urbi_parse_peek(p).type == TOK_COMMA) {
            urbi_parse_consume(p);
        } else {
            break;
        }
    }

    { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }  /* urbi_parse_consume ')' */

    /* A function literal is a loop/switch boundary: break/continue do not
     * reach through it into an enclosing loop. */
    UParseFuncBoundary saved_boundary;
    urbi_parse_enter_function_boundary(p, &saved_boundary);
    UAstNode *body = urbi_parse_block(p);
    urbi_parse_leave_function_boundary(p, &saved_boundary);
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    UAstNode *node = urbi_parse_make_node(p, AST_FUNCTION, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.func.params      = params;
    node->u.func.param_count = count;
    node->u.func.body        = body;
    return node;
}

/* --- urbi_parse_property_decl: `(params) { body }` after a `get`/`set` IDENT.
 *
 * getter/setter.  Caller has already consumed the leading IDENT
 * (`get`/`set`) and the following slot-name IDENT — `name_tok` carries
 * the slot-name token.  The current urbi_parse_peek is `(`.
 *
 * Desugars directly to `recv.setProperty(name, "oget"|"oset", function)`
 * — no new opcode, no AST_PROPERTY_DECL kind.  `recv` is NULL only for
 * the implicit-receiver form at the start of a class body; that case
 * substitutes the class-body desugar's hidden receiver name
 * (p->class_body_name_start/len, set by parse_class_declaration around
 * its body parse) rather than leaving a dangling receiver, so the
 * result is always a complete, self-contained CALL. --- */
UAstNode *urbi_parse_property_decl(UParser *p, UAstNode *recv, UToken name_tok,
                              UAstMethodKind kind, int line, int col) {
    { UAstNode *err = NULL; if (!expect(p, TOK_LPAREN, PARSE_EXPECTED_LPAREN, &err)) return err; }  /* urbi_parse_consume '(' */

    /* Parameter list — identical shape to urbi_parse_function. */
    int cap = 4;
    UAstNode **params = (UAstNode **)uarena_alloc(p->arena,
                                                   (size_t)cap * sizeof(UAstNode *));
    if (!params) return (UAstNode *)&uparser_oom_sentinel;
    int count = 0;

    while (urbi_parse_peek(p).type != TOK_RPAREN && urbi_parse_peek(p).type != TOK_EOF) {
        bool is_lazy = false;
        if (urbi_parse_peek(p).type == TOK_KW_LAZY) {
            urbi_parse_consume(p);
            is_lazy = true;
        }

        UToken pname = urbi_parse_peek(p);
        { UAstNode *err = NULL; if (!expect(p, TOK_IDENT, PARSE_EXPECTED_IDENT, &err)) return err; }

        UAstNode *pn = urbi_parse_make_node(p, AST_PARAM, pname.line, pname.col);
        if (!pn) return (UAstNode *)&uparser_oom_sentinel;
        pn->u.param.name_start = pname.u.str.start;
        pn->u.param.name_len   = pname.u.str.len;
        pn->u.param.is_lazy    = is_lazy;

        UAstNode *derr = parse_optional_param_default(p, pn, is_lazy);
        if (derr) return derr;

        if (count == cap) {
            if (!urbi_parse_arena_grow_node_array(p, &params, &cap, count))
                return (UAstNode *)&uparser_oom_sentinel;
        }
        params[count++] = pn;

        if (urbi_parse_peek(p).type == TOK_COMMA) {
            urbi_parse_consume(p);
        } else {
            break;
        }
    }

    { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }  /* urbi_parse_consume ')' */

    /* A getter/setter body is a function literal: break/continue do not
     * reach through it into an enclosing loop. */
    UParseFuncBoundary saved_boundary;
    urbi_parse_enter_function_boundary(p, &saved_boundary);
    UAstNode *body = urbi_parse_block(p);
    urbi_parse_leave_function_boundary(p, &saved_boundary);
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    /* Build the inner AST_FUNCTION carrying params + body. */
    UAstNode *func = urbi_parse_make_node(p, AST_FUNCTION, line, col);
    if (!func) return (UAstNode *)&uparser_oom_sentinel;
    func->u.func.params      = params;
    func->u.func.param_count = count;
    func->u.func.body        = body;

    UAstNode *actual_recv = recv;
    if (actual_recv == NULL) {
        actual_recv = urbi_parse_desugar_ident(p, p->class_body_name_start,
                                                p->class_body_name_len, line, col);
        if (!actual_recv) return NULL;
    }

    const char *prop_kind = (kind == UAST_METHOD_GETTER) ? "oget" : "oset";
    UAstNode *name_str = urbi_parse_desugar_str(p, name_tok.u.str.start,
                                                 name_tok.u.str.len, line, col);
    if (!name_str) return NULL;
    UAstNode *kind_str = urbi_parse_desugar_str(p, prop_kind, 4, line, col);
    if (!kind_str) return NULL;
    UAstNode *setprop_mg = urbi_parse_desugar_member_get(p, actual_recv, "setProperty",
                                                          11, line, col);
    if (!setprop_mg) return NULL;

    UAstNode **args = (UAstNode **)uarena_alloc(p->arena, 3U * sizeof(UAstNode *));
    if (!args) return (UAstNode *)&uparser_oom_sentinel;
    args[0] = name_str;
    args[1] = kind_str;
    args[2] = func;
    return urbi_parse_desugar_call(p, setprop_mg, args, 3, line, col);
}

/* --- parse_return: `return [expr]` --- */

static UAstNode *parse_return(UParser *p) {
    UToken kw = urbi_parse_consume(p);  /* urbi_parse_consume TOK_KW_RETURN */

    /* Determine whether a return value follows.  Stop at any statement-ending
       token: EOF, `}`, `)`, `|`, `;`, or `,`. */
    UAstNode *value = NULL;
    {
        UTokenType nt = urbi_parse_peek(p).type;
        bool no_value = nt == TOK_EOF
                     || nt == TOK_RBRACE
                     || nt == TOK_RPAREN
                     || nt == TOK_PIPE
                     || nt == TOK_SEMI
                     || nt == TOK_COMMA;
        if (!no_value) {
            value = urbi_parse_expression(p, 0);
            if (!value) return (UAstNode *)&uparser_oom_sentinel;
            if (value->kind == AST_ERROR) return value;
        }
    }

    UAstNode *node = urbi_parse_make_node(p, AST_RETURN, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.ret.value = value;
    return node;
}

/* --- urbi_parse_throw: `throw expr` --- */

UAstNode *urbi_parse_throw(UParser *p) {
    UToken kw = urbi_parse_consume(p);  /* urbi_parse_consume TOK_KW_THROW */

    UAstNode *value = urbi_parse_expression(p, 0);
    if (!value) return (UAstNode *)&uparser_oom_sentinel;
    if (value->kind == AST_ERROR) return value;

    UAstNode *node = urbi_parse_make_node(p, AST_THROW, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.throw_expr.value = value;
    return node;
}

/* Paren form:   assert(expr)
 *   Records the source text span of `expr` for use in the failure diagnostic.
 *   src_text points into the source buffer between the `(` and `)` characters
 *   (trailing whitespace trimmed).
 *
 * Block form:   assert { stmts }
 *   Evaluates the block; truthy final value = pass (no throw).
 *   src_text/src_len = NULL/0.
 */
UAstNode *urbi_parse_assert(UParser *p) {
    UToken kw = urbi_parse_consume(p);  /* urbi_parse_consume TOK_KW_ASSERT */

    UToken next = urbi_parse_peek(p);

    UAstNode  *cond_expr;
    const char *msg_bytes;
    int         msg_len;

    if (next.type == TOK_LBRACE) {
        /* Block form: assert { stmts } — the diagnostic carries no source
         * span, just the bare "assertion failed". */
        UAstNode *block = urbi_parse_block(p);
        if (!block) return (UAstNode *)&uparser_oom_sentinel;
        if (block->kind == AST_ERROR) return block;

        static const char kMsgBase[] = "assertion failed";
        cond_expr = block;
        msg_bytes = kMsgBase;
        msg_len   = (int)(sizeof(kMsgBase) - 1U);
    } else {
        { UAstNode *err = NULL; if (!expect(p, TOK_LPAREN, PARSE_EXPECTED_LPAREN, &err)) return err; }  /* urbi_parse_consume '(' */

        /* Capture source text start: p->lex->cur is now right after '('.
         * Trim leading whitespace so the diagnostic text starts at the expression.
         * The source buffer is guaranteed to outlive the AST node.
         * Bound the walk at p->lex->end — `assert(` at the very
         * end of a non-NUL-terminated buffer put cur one past the end, and the
         * unbounded loop read past the allocation. */
        const char *src_start = p->lex->cur;
        while (src_start < p->lex->end &&
               (*src_start == ' ' || *src_start == '\t'
                || *src_start == '\r' || *src_start == '\n')) {
            src_start++;
        }

        UAstNode *expr = urbi_parse_inner_tier(p);
        if (!expr) return (UAstNode *)&uparser_oom_sentinel;
        if (expr->kind == AST_ERROR) return expr;

        /* Compute source text end: after urbi_parse_inner_tier, the parser has peeked
         * the first token after the expression.  That peeked token (')') was
         * produced by ulex_next which advanced p->lex->cur past ')'.
         * The ')' starts at (p->lex->cur - p->urbi_parse_peek.len) when have_peek is set. */
        const char *src_end = p->have_peek ? (p->lex->cur - (size_t)p->urbi_parse_peek.len)
                                           : p->lex->cur;
        /* Trim trailing whitespace so the diagnostic text is clean. */
        while (src_end > src_start && (src_end[-1] == ' ' || src_end[-1] == '\t'
                                        || src_end[-1] == '\r' || src_end[-1] == '\n')) {
            src_end--;
        }

        { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }  /* urbi_parse_consume ')' */

        static const char kMsgPrefix[] = "assertion failed: ";
        size_t prefix_len = sizeof(kMsgPrefix) - 1U;
        size_t span_len   = (size_t)(src_end - src_start);
        char *buf = (char *)uarena_alloc(p->arena, prefix_len + span_len);
        if (!buf) return (UAstNode *)&uparser_oom_sentinel;
        urbi_memcpy(buf, kMsgPrefix, prefix_len);
        urbi_memcpy(buf + prefix_len, src_start, span_len);

        cond_expr = expr;
        msg_bytes = buf;
        msg_len   = (int)(prefix_len + span_len);
    }

    /* if (!cond_expr) throw "<message>" */
    UAstNode *cond = urbi_parse_make_unary(p, UOP_NOT, cond_expr, kw.line, kw.col);
    if (!cond) return NULL;

    UAstNode *msg_node = urbi_parse_desugar_str(p, msg_bytes, msg_len, kw.line, kw.col);
    if (!msg_node) return NULL;

    UAstNode *throw_node = urbi_parse_make_node(p, AST_THROW, kw.line, kw.col);
    if (!throw_node) return NULL;
    throw_node->u.throw_expr.value = msg_node;

    UAstNode **then_stmts = (UAstNode **)uarena_alloc(p->arena, sizeof(UAstNode *));
    if (!then_stmts) return (UAstNode *)&uparser_oom_sentinel;
    then_stmts[0] = throw_node;
    UAstNode *then_block = urbi_parse_desugar_block(p, then_stmts, 1, kw.line, kw.col);
    if (!then_block) return NULL;

    UAstNode *if_node = urbi_parse_make_node(p, AST_IF, kw.line, kw.col);
    if (!if_node) return NULL;
    if_node->u.if_stmt.cond       = cond;
    if_node->u.if_stmt.then_block = then_block;
    if_node->u.if_stmt.else_block = NULL;
    return if_node;
}

/* --- urbi_parse_try: `try { body } [catch ([var] e [if guard]) { handler }] [else { body }] [finally { cleanup }]`
 *
 * Both catch and finally remain optional, but at least one must be present.
 * `else` requires a preceding catch clause. */

UAstNode *urbi_parse_try(UParser *p) {
    UToken kw = urbi_parse_consume(p);  /* urbi_parse_consume TOK_KW_TRY */

    UAstNode *body = urbi_parse_block(p);
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    const char *catch_var_start = NULL;
    int         catch_var_len   = 0;
    UAstNode   *catch_body      = NULL;
    UAstNode   *catch_guard     = NULL;
    UAstNode   *else_body       = NULL;
    UAstNode   *finally_body    = NULL;

    /* Optional catch clause. */
    if (urbi_parse_peek(p).type == TOK_KW_CATCH) {
        urbi_parse_consume(p);  /* urbi_parse_consume 'catch' */

        { UAstNode *err = NULL; if (!expect(p, TOK_LPAREN, PARSE_EXPECTED_LPAREN, &err)) return err; }

        /* Accept optional `var` keyword before the catch variable name. */
        if (urbi_parse_peek(p).type == TOK_KW_VAR) {
            urbi_parse_consume(p);  /* urbi_parse_consume 'var' — treated as sugar, no semantic change */
        }

        UToken var_tok = urbi_parse_peek(p);
        { UAstNode *err = NULL; if (!expect(p, TOK_IDENT, PARSE_EXPECTED_IDENT, &err)) return err; }
        catch_var_start = var_tok.u.str.start;
        catch_var_len   = var_tok.u.str.len;

        /* Accept optional `if expr` guard. */
        if (urbi_parse_peek(p).type == TOK_KW_IF) {
            urbi_parse_consume(p);  /* urbi_parse_consume 'if' */
            catch_guard = urbi_parse_expression(p, 0);
            if (!catch_guard) return (UAstNode *)&uparser_oom_sentinel;
            if (catch_guard->kind == AST_ERROR) return catch_guard;
        }

        { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }

        catch_body = urbi_parse_block(p);
        if (!catch_body) return (UAstNode *)&uparser_oom_sentinel;
        if (catch_body->kind == AST_ERROR) return catch_body;

        /* Accept optional `else { body }` clause after catch. */
        if (urbi_parse_peek(p).type == TOK_KW_ELSE) {
            urbi_parse_consume(p);  /* urbi_parse_consume 'else' */
            else_body = urbi_parse_block(p);
            if (!else_body) return (UAstNode *)&uparser_oom_sentinel;
            if (else_body->kind == AST_ERROR) return else_body;
        }
    }

    /* Optional finally clause. */
    if (urbi_parse_peek(p).type == TOK_KW_FINALLY) {
        urbi_parse_consume(p);  /* urbi_parse_consume 'finally' */

        finally_body = urbi_parse_block(p);
        if (!finally_body) return (UAstNode *)&uparser_oom_sentinel;
        if (finally_body->kind == AST_ERROR) return finally_body;
    }

    /* Require at least one of catch or finally. */
    if (catch_body == NULL && finally_body == NULL) {
        return urbi_parse_make_error(p, PARSE_TRY_NEEDS_CATCH_OR_FINALLY,
                          urbi_parse_kErrorMessages[PARSE_TRY_NEEDS_CATCH_OR_FINALLY],
                          kw.line, kw.col);
    }

    UAstNode *node = urbi_parse_make_node(p, AST_TRY, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.try_stmt.body            = body;
    node->u.try_stmt.catch_var_start = catch_var_start;
    node->u.try_stmt.catch_var_len   = catch_var_len;
    node->u.try_stmt.catch_body      = catch_body;
    node->u.try_stmt.catch_guard     = catch_guard;
    node->u.try_stmt.else_body       = else_body;
    node->u.try_stmt.finally_body    = finally_body;
    return node;
}

 /* parse_for — `for (var x : iter_expr) body` or `for (var x in iter_expr) body`
 *
 * Supports only the for-each form.  C-style `for (init; cond; step)` is a
 * migration (see docs/migration/control-flow-migration.md).  Count-form
 * `for (N) body` and flavoured `for|`/`for&` are deferred-v1.x.
 *
 * Lowered directly to core AST kinds — no AST_FOR_EACH node, no new
 * opcode:
 *   BLOCK {
 *     var $it = iter_expr;
 *     var $n  = $it.length();
 *     var $i  = 0;
 *     while ($i < $n) {
 *       var x = $it.get($i);
 *       $i = $i + 1;
 *       body
 *     }
 *   }
 * `$it`/`$n`/`$i` are hidden locals (urbi_parse_hidden_name).  break/continue
 * work inside the body because it is an ordinary AST_WHILE loop body. */
static UAstNode *parse_for(UParser *p) {
    UToken kw = urbi_parse_consume(p);  /* urbi_parse_consume TOK_KW_FOR */

    { UAstNode *err = NULL; if (!expect(p, TOK_LPAREN, PARSE_EXPECTED_LPAREN, &err)) return err; }

    /* Require `var` before the loop variable. */
    { UAstNode *err = NULL; if (!expect(p, TOK_KW_VAR, PARSE_FOR_EXPECTED_VAR, &err)) return err; }

    UToken name_tok = urbi_parse_peek(p);
    { UAstNode *err = NULL; if (!expect(p, TOK_IDENT, PARSE_EXPECTED_IDENT, &err)) return err; }

    /* Separator: `:` or `in`. */
    UToken sep_tok = urbi_parse_peek(p);
    const int sep_is_colon = (sep_tok.type == TOK_COLON);
    const int sep_is_in    = (sep_tok.type == TOK_IDENT &&
                              urbi_parse_ident_equals(sep_tok.u.str.start, sep_tok.u.str.len, "in", 2));
    if (!sep_is_colon && !sep_is_in) {
        return urbi_parse_make_error(p, PARSE_FOR_EXPECTED_COLON_OR_IN,
                          urbi_parse_kErrorMessages[PARSE_FOR_EXPECTED_COLON_OR_IN],
                          sep_tok.line, sep_tok.col);
    }
    urbi_parse_consume(p);

    /* Iterable expression (allows commas inside parens — urbi_parse_inner_tier). */
    UAstNode *iter = urbi_parse_inner_tier(p);
    if (!iter) return (UAstNode *)&uparser_oom_sentinel;
    if (iter->kind == AST_ERROR) return iter;

    { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }

    /* Body block.  Bump loop_depth so break/continue are valid inside. */
    p->loop_depth++;
    UAstNode *body = urbi_parse_block(p);
    p->loop_depth--;
    if (!body) return (UAstNode *)&uparser_oom_sentinel;
    if (body->kind == AST_ERROR) return body;

    int it_len, n_len, i_len;
    const char *it_name = urbi_parse_hidden_name(p, "it", &it_len);
    const char *n_name  = urbi_parse_hidden_name(p, "n", &n_len);
    const char *i_name  = urbi_parse_hidden_name(p, "i", &i_len);
    if (!it_name || !n_name || !i_name) return (UAstNode *)&uparser_oom_sentinel;

    /* var $it = iter_expr */
    UAstNode *var_it = urbi_parse_desugar_var_decl(p, it_name, it_len, iter, kw.line, kw.col);
    if (!var_it) return NULL;

    /* var $n = $it.length() */
    UAstNode *it_ref1 = urbi_parse_desugar_ident(p, it_name, it_len, kw.line, kw.col);
    if (!it_ref1) return NULL;
    UAstNode *len_mg = urbi_parse_desugar_member_get(p, it_ref1, "length", 6, kw.line, kw.col);
    if (!len_mg) return NULL;
    UAstNode *len_call = urbi_parse_desugar_call(p, len_mg, NULL, 0, kw.line, kw.col);
    if (!len_call) return NULL;
    UAstNode *var_n = urbi_parse_desugar_var_decl(p, n_name, n_len, len_call, kw.line, kw.col);
    if (!var_n) return NULL;

    /* var $i = 0 */
    UAstNode *zero = urbi_parse_make_int(p, 0, kw.line, kw.col);
    if (!zero) return NULL;
    UAstNode *var_i = urbi_parse_desugar_var_decl(p, i_name, i_len, zero, kw.line, kw.col);
    if (!var_i) return NULL;

    /* while ($i < $n) */
    UAstNode *i_ref1 = urbi_parse_desugar_ident(p, i_name, i_len, kw.line, kw.col);
    UAstNode *n_ref  = urbi_parse_desugar_ident(p, n_name, n_len, kw.line, kw.col);
    if (!i_ref1 || !n_ref) return NULL;
    UAstNode *cond = urbi_parse_make_node(p, AST_COMPARE, kw.line, kw.col);
    if (!cond) return NULL;
    cond->u.cmp.op  = CMP_LT;
    cond->u.cmp.lhs = i_ref1;
    cond->u.cmp.rhs = n_ref;

    /* var x = $it.get($i) */
    UAstNode *it_ref2 = urbi_parse_desugar_ident(p, it_name, it_len, kw.line, kw.col);
    UAstNode *i_ref2  = urbi_parse_desugar_ident(p, i_name, i_len, kw.line, kw.col);
    if (!it_ref2 || !i_ref2) return NULL;
    UAstNode *get_mg = urbi_parse_desugar_member_get(p, it_ref2, "get", 3, kw.line, kw.col);
    if (!get_mg) return NULL;
    UAstNode **get_args = (UAstNode **)uarena_alloc(p->arena, sizeof(UAstNode *));
    if (!get_args) return (UAstNode *)&uparser_oom_sentinel;
    get_args[0] = i_ref2;
    UAstNode *get_call = urbi_parse_desugar_call(p, get_mg, get_args, 1, kw.line, kw.col);
    if (!get_call) return NULL;
    UAstNode *var_x = urbi_parse_desugar_var_decl(p, name_tok.u.str.start, name_tok.u.str.len,
                                                  get_call, name_tok.line, name_tok.col);
    if (!var_x) return NULL;

    /* $i = $i + 1 */
    UAstNode *i_ref3 = urbi_parse_desugar_ident(p, i_name, i_len, kw.line, kw.col);
    if (!i_ref3) return NULL;
    UAstNode *one = urbi_parse_make_int(p, 1, kw.line, kw.col);
    if (!one) return NULL;
    UAstNode *inc = urbi_parse_make_binary(p, BOP_ADD, i_ref3, one, kw.line, kw.col);
    if (!inc) return NULL;
    UAstNode *assign_i = urbi_parse_make_node(p, AST_ASSIGN, kw.line, kw.col);
    if (!assign_i) return NULL;
    assign_i->u.assign.name_start = i_name;
    assign_i->u.assign.name_len   = i_len;
    assign_i->u.assign.value      = inc;

    UAstNode **inner_stmts = (UAstNode **)uarena_alloc(p->arena, 3U * sizeof(UAstNode *));
    if (!inner_stmts) return (UAstNode *)&uparser_oom_sentinel;
    inner_stmts[0] = var_x;
    inner_stmts[1] = assign_i;
    inner_stmts[2] = body;
    UAstNode *while_body = urbi_parse_desugar_block(p, inner_stmts, 3, kw.line, kw.col);
    if (!while_body) return NULL;

    UAstNode *while_node = urbi_parse_make_node(p, AST_WHILE, kw.line, kw.col);
    if (!while_node) return NULL;
    while_node->u.while_stmt.cond = cond;
    while_node->u.while_stmt.body = while_body;

    UAstNode **outer_stmts = (UAstNode **)uarena_alloc(p->arena, 4U * sizeof(UAstNode *));
    if (!outer_stmts) return (UAstNode *)&uparser_oom_sentinel;
    outer_stmts[0] = var_it;
    outer_stmts[1] = var_n;
    outer_stmts[2] = var_i;
    outer_stmts[3] = while_node;
    return urbi_parse_desugar_block(p, outer_stmts, 4, kw.line, kw.col);
}

static UAstNode *parse_break(UParser *p) {
    UToken kw = urbi_parse_consume(p);  /* urbi_parse_consume TOK_KW_BREAK */
    if (p->loop_depth == 0 && p->switch_depth == 0) {
        return urbi_parse_make_error(p, PARSE_BREAK_OUTSIDE_LOOP,
                          urbi_parse_kErrorMessages[PARSE_BREAK_OUTSIDE_LOOP],
                          kw.line, kw.col);
    }
    UAstNode *node = urbi_parse_make_node(p, AST_BREAK, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    return node;
}

static UAstNode *parse_continue(UParser *p) {
    UToken kw = urbi_parse_consume(p);  /* urbi_parse_consume TOK_KW_CONTINUE */
    if (p->loop_depth == 0) {
        return urbi_parse_make_error(p, PARSE_CONTINUE_OUTSIDE_LOOP,
                          urbi_parse_kErrorMessages[PARSE_CONTINUE_OUTSIDE_LOOP],
                          kw.line, kw.col);
    }
    UAstNode *node = urbi_parse_make_node(p, AST_CONTINUE, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    return node;
}

/* parse_switch — `switch (expr) { case v1: body1; case v2: body2; }`
 *
 * Grammar:
 *   switch ( expr ) { ( case expr : stmts )* }
 *
 * The emitter lowers to a chain of if-else comparisons.
 * Break inside a case body exits the switch (switch is a loop-context
 * in the emitter's patch-list sense). */
static UAstNode *parse_switch(UParser *p) {
    UToken kw = urbi_parse_consume(p);  /* urbi_parse_consume TOK_KW_SWITCH */

    { UAstNode *err = NULL; if (!expect(p, TOK_LPAREN, PARSE_EXPECTED_LPAREN, &err)) return err; }

    UAstNode *expr = urbi_parse_inner_tier(p);
    if (!expr) return (UAstNode *)&uparser_oom_sentinel;
    if (expr->kind == AST_ERROR) return expr;

    { UAstNode *err = NULL; if (!expect(p, TOK_RPAREN, PARSE_EXPECTED_RPAREN, &err)) return err; }

    { UAstNode *err = NULL; if (!expect(p, TOK_LBRACE, PARSE_EXPECTED_LBRACE, &err)) return err; }

    /* Parse case list (and optional default arm). */
    int cap = 4;
    UAstNode **case_vals   = (UAstNode **)uarena_alloc(p->arena,
                                                        (size_t)cap * sizeof(UAstNode *));
    UAstNode **case_bodies = (UAstNode **)uarena_alloc(p->arena,
                                                        (size_t)cap * sizeof(UAstNode *));
    if (!case_vals || !case_bodies) return (UAstNode *)&uparser_oom_sentinel;
    int case_count = 0;
    UAstNode *default_body = NULL;
    int has_default = 0;

    /* switch body: break inside a case must exit the switch.
     * Use switch_depth (not loop_depth) so `continue` inside a switch
     * still requires an enclosing real loop.  See parse_continue. */
    p->switch_depth++;

    while (urbi_parse_peek(p).type != TOK_RBRACE && urbi_parse_peek(p).type != TOK_EOF) {
        /* Skip statement separators between cases. */
        while (urbi_parse_peek(p).type == TOK_SEMI || urbi_parse_peek(p).type == TOK_PIPE) {
            urbi_parse_consume(p);
        }
        if (urbi_parse_peek(p).type == TOK_RBRACE || urbi_parse_peek(p).type == TOK_EOF) break;

        UToken case_tok = urbi_parse_peek(p);
        if (case_tok.type != TOK_KW_CASE && case_tok.type != TOK_KW_DEFAULT) {
            p->switch_depth--;
            return urbi_parse_make_error(p, PARSE_SWITCH_EXPECTED_CASE,
                              urbi_parse_kErrorMessages[PARSE_SWITCH_EXPECTED_CASE],
                              case_tok.line, case_tok.col);
        }

        if (case_tok.type == TOK_KW_DEFAULT) {
            /* default arm — at most one; no value expression. */
            if (has_default) {
                p->switch_depth--;
                return urbi_parse_make_error(p, PARSE_SWITCH_DUPLICATE_DEFAULT,
                                  urbi_parse_kErrorMessages[PARSE_SWITCH_DUPLICATE_DEFAULT],
                                  case_tok.line, case_tok.col);
            }
            has_default = 1;
            urbi_parse_consume(p);  /* urbi_parse_consume 'default' */

            UToken dcolon = urbi_parse_peek(p);
            if (dcolon.type != TOK_COLON) {
                p->switch_depth--;
                return urbi_parse_make_error(p, PARSE_SWITCH_EXPECTED_COLON,
                                  urbi_parse_kErrorMessages[PARSE_SWITCH_EXPECTED_COLON],
                                  dcolon.line, dcolon.col);
            }
            urbi_parse_consume(p);  /* urbi_parse_consume ':' */

            /* Collect default body stmts. */
            int dstmt_cap = 4;
            UAstNode **dstmts = (UAstNode **)uarena_alloc(p->arena,
                                                           (size_t)dstmt_cap * sizeof(UAstNode *));
            if (!dstmts) { p->switch_depth--; return (UAstNode *)&uparser_oom_sentinel; }
            int dstmt_count = 0;

            while (urbi_parse_peek(p).type != TOK_KW_CASE &&
                   urbi_parse_peek(p).type != TOK_KW_DEFAULT &&
                   urbi_parse_peek(p).type != TOK_RBRACE &&
                   urbi_parse_peek(p).type != TOK_EOF) {
                UAstNode *s = urbi_parse_statement_or_expr(p);
                if (!s) { p->switch_depth--; return (UAstNode *)&uparser_oom_sentinel; }
                if (s->kind == AST_ERROR) { p->switch_depth--; return s; }

                if (dstmt_count == dstmt_cap) {
                    if (!urbi_parse_arena_grow_node_array(p, &dstmts, &dstmt_cap, dstmt_count)) {
                        p->switch_depth--;
                        return (UAstNode *)&uparser_oom_sentinel;
                    }
                }
                dstmts[dstmt_count++] = s;

                if (urbi_parse_peek(p).type == TOK_SEMI || urbi_parse_peek(p).type == TOK_PIPE) {
                    urbi_parse_consume(p);
                }
            }

            UAstNode *dbody = urbi_parse_make_node(p, AST_BLOCK, case_tok.line, case_tok.col);
            if (!dbody) { p->switch_depth--; return (UAstNode *)&uparser_oom_sentinel; }
            dbody->u.block.stmts = dstmts;
            dbody->u.block.count = dstmt_count;
            default_body = dbody;
            continue;
        }

        /* case arm */
        urbi_parse_consume(p);  /* urbi_parse_consume 'case' */

        /* Case value expression (not a separator tier — parse as atom/prefix). */
        UAstNode *val = urbi_parse_expression(p, 0);
        if (!val) { p->switch_depth--; return (UAstNode *)&uparser_oom_sentinel; }
        if (val->kind == AST_ERROR) { p->switch_depth--; return val; }

        UToken colon = urbi_parse_peek(p);
        if (colon.type != TOK_COLON) {
            p->switch_depth--;
            return urbi_parse_make_error(p, PARSE_SWITCH_EXPECTED_COLON,
                              urbi_parse_kErrorMessages[PARSE_SWITCH_EXPECTED_COLON],
                              colon.line, colon.col);
        }
        urbi_parse_consume(p);  /* urbi_parse_consume ':' */

        /* Collect statements until the next `case`, `default`, `}`, or EOF.
         * Build them into an implicit AST_BLOCK. */
        int stmt_cap = 4;
        UAstNode **stmts = (UAstNode **)uarena_alloc(p->arena,
                                                       (size_t)stmt_cap * sizeof(UAstNode *));
        if (!stmts) { p->switch_depth--; return (UAstNode *)&uparser_oom_sentinel; }
        int stmt_count = 0;

        while (urbi_parse_peek(p).type != TOK_KW_CASE &&
               urbi_parse_peek(p).type != TOK_KW_DEFAULT &&
               urbi_parse_peek(p).type != TOK_RBRACE &&
               urbi_parse_peek(p).type != TOK_EOF) {
            /* Use urbi_parse_statement_or_expr (not urbi_parse_outer_tier) so that ';'
             * between case bodies is not greedily consumed across 'case'
             * boundaries — urbi_parse_outer_tier would eat the ';' and then try to
             * parse 'case' as an expression, yielding "expected expression". */
            UAstNode *s = urbi_parse_statement_or_expr(p);
            if (!s) { p->switch_depth--; return (UAstNode *)&uparser_oom_sentinel; }
            if (s->kind == AST_ERROR) { p->switch_depth--; return s; }

            if (stmt_count == stmt_cap) {
                if (!urbi_parse_arena_grow_node_array(p, &stmts, &stmt_cap, stmt_count)) {
                    p->switch_depth--;
                    return (UAstNode *)&uparser_oom_sentinel;
                }
            }
            stmts[stmt_count++] = s;

            /* Consume separator if present. */
            if (urbi_parse_peek(p).type == TOK_SEMI || urbi_parse_peek(p).type == TOK_PIPE) {
                urbi_parse_consume(p);
            }
        }

        UAstNode *body = urbi_parse_make_node(p, AST_BLOCK, case_tok.line, case_tok.col);
        if (!body) { p->switch_depth--; return (UAstNode *)&uparser_oom_sentinel; }
        body->u.block.stmts = stmts;
        body->u.block.count = stmt_count;

        /* Grow parallel arrays if needed.  urbi_parse_arena_grow_node_array doubles
         * *cap on every call, so the two arrays must track independent
         * capacities — sharing one cap left case_vals at half the claimed
         * capacity and overran it into case_bodies. */
        if (case_count == cap) {
            int bodies_cap = cap; /* before grow */
            if (!urbi_parse_arena_grow_node_array(p, &case_vals,   &cap, case_count) ||
                !urbi_parse_arena_grow_node_array(p, &case_bodies, &bodies_cap, case_count)) {
                p->switch_depth--;
                return (UAstNode *)&uparser_oom_sentinel;
            }
        }
        case_vals[case_count]   = val;
        case_bodies[case_count] = body;
        case_count++;
    }

    p->switch_depth--;

    { UAstNode *err = NULL; if (!expect(p, TOK_RBRACE, PARSE_EXPECTED_RBRACE, &err)) return err; }

    UAstNode *node = urbi_parse_make_node(p, AST_SWITCH, kw.line, kw.col);
    if (!node) return (UAstNode *)&uparser_oom_sentinel;
    node->u.switch_stmt.expr         = expr;
    node->u.switch_stmt.case_vals    = case_vals;
    node->u.switch_stmt.case_bodies  = case_bodies;
    node->u.switch_stmt.case_count   = case_count;
    node->u.switch_stmt.default_body = default_body;
    return node;
}
