/* SPDX-License-Identifier: BSD-3-Clause */
/* T45: AST_AT_EVENT parse + emit tests.
 *
 * Covers postfix `?` recognition inside at(...) and the emit arm that
 * produces OP_AT_EVENT_INSTALL / OP_AT_EVENT_SYNC_INSTALL.
 */

#include "utest.h"

#include <string.h>

#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "umodule.h"
#include "parse/uparse.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

typedef struct {
    ULexer  lex;
    UArena  arena;
    UParser p;
} ParseCtx;

static void ctx_init(ParseCtx *c, const char *src) {
    ulex_init(&c->lex, src, strlen(src));
    uarena_init(&c->arena, 0);
    uparse_init(&c->p, &c->lex, &c->arena);
}

static void ctx_destroy(ParseCtx *c) {
    uarena_destroy(&c->arena);
}

/* Compile source; return emit error.  On success, *mod_out contains bytecode. */
static UEmitError at_event_compile(const char *src,
                                   UModule    *mod_out,
                                   UArena     *arena_out,
                                   UVM        *vm_out,
                                   UEmitter   *e_out) {
    uvm_init(vm_out, NULL, NULL);
    uarena_init(arena_out, 4096);

    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    uemit_init(e_out, mod_out, arena_out, vm_out, NULL);

    UParser p;
    uparse_init(&p, &lex, arena_out);

    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) break;
        (void)uemit_statement(e_out, node);
        uarena_reset(arena_out);
    }
    return uemit_finish(e_out);
}

static void at_event_cleanup(UModule *mod, UArena *arena, UVM *vm) {
    umodule_destroy(mod);
    uarena_destroy(arena);
    uvm_destroy(vm);
}

/* Return true if any instruction in the module root has opcode == op. */
static bool bytecode_has_op(const UModule *m, UOpcode op) {
    size_t i;
    for (i = 0; i < m->instr_count; i++) {
        if (uinstr_op(m->instructions[i]) == op) return true;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * T45 parse tests
 * ----------------------------------------------------------------------- */

/* at (e?) body  →  AST_AT_EVENT, sync_flag = false */
UTEST(parse_at_event_with_question_postfix) {
    ParseCtx c;
    ctx_init(&c, "at (e?) body");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_AT_EVENT, n->kind);
    UASSERT_EQ(0, (int)n->u.at_event.is_sync);
    UASSERT(n->u.at_event.event_expr != NULL);
    UASSERT_EQ(AST_IDENT, n->u.at_event.event_expr->kind);
    ctx_destroy(&c);
}

/* at sync (e?) body  →  AST_AT_EVENT, sync_flag = true */
UTEST(parse_at_sync_event) {
    ParseCtx c;
    ctx_init(&c, "at sync (e?) body");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_AT_EVENT, n->kind);
    UASSERT_EQ(1, (int)n->u.at_event.is_sync);
    ctx_destroy(&c);
}

/* at (e?) body onleave handler  →  onleave populated */
UTEST(parse_at_event_with_onleave) {
    ParseCtx c;
    ctx_init(&c, "at (e?) body onleave handler");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_AT_EVENT, n->kind);
    UASSERT(n->u.at_event.onleave != NULL);
    ctx_destroy(&c);
}

/* at (e?) body without onleave  →  onleave is NULL */
UTEST(parse_at_event_no_onleave) {
    ParseCtx c;
    ctx_init(&c, "at (e?) body");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_AT_EVENT, n->kind);
    UASSERT(n->u.at_event.onleave == NULL);
    ctx_destroy(&c);
}

/* at (cond) body (no ?) still produces AST_WATCHER */
UTEST(parse_at_no_question_still_watcher) {
    ParseCtx c;
    ctx_init(&c, "at (x > 0) body");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_WATCHER, n->kind);
    ctx_destroy(&c);
}

/* var x = e?  →  PARSE_QUESTION_OUTSIDE_AT error */
UTEST(parse_question_outside_at_errors) {
    ParseCtx c;
    ctx_init(&c, "var x = e?");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_ERROR, n->kind);
    UASSERT_EQ(PARSE_QUESTION_OUTSIDE_AT, (UParseError)n->u.err.code);
    ctx_destroy(&c);
}

/* Standalone e? in expression position  →  PARSE_QUESTION_OUTSIDE_AT */
UTEST(parse_question_outside_at_standalone) {
    ParseCtx c;
    ctx_init(&c, "e?");
    UAstNode *n = uparse_next_statement(&c.p);
    UASSERT(n != NULL);
    UASSERT_EQ(AST_ERROR, n->kind);
    UASSERT_EQ(PARSE_QUESTION_OUTSIDE_AT, (UParseError)n->u.err.code);
    ctx_destroy(&c);
}

/* -----------------------------------------------------------------------
 * T45 emit tests
 * ----------------------------------------------------------------------- */

/* at (e?) body  →  bytecode contains OP_AT_EVENT_INSTALL (=43).
 * Pre-declare both ev and body_val to satisfy the v1.0 no-globals rule. */
UTEST(emit_at_event_produces_OP_AT_EVENT_INSTALL) {
    UModule  module = {0};
    UArena   arena;
    UVM      vm;
    UEmitter e;

    UEmitError rc = at_event_compile(
        "var ev = 0; var body_val = 0; at (ev?) body_val",
        &module, &arena, &vm, &e);
    UASSERT_EQ(EMIT_OK, rc);
    UASSERT(bytecode_has_op(&module, OP_AT_EVENT_INSTALL));

    at_event_cleanup(&module, &arena, &vm);
}

/* at sync (e?) body  →  bytecode contains OP_AT_EVENT_SYNC_INSTALL (=44) */
UTEST(emit_at_sync_event_produces_OP_AT_EVENT_SYNC_INSTALL) {
    UModule  module = {0};
    UArena   arena;
    UVM      vm;
    UEmitter e;

    UEmitError rc = at_event_compile(
        "var ev = 0; var body_val = 0; at sync (ev?) body_val",
        &module, &arena, &vm, &e);
    UASSERT_EQ(EMIT_OK, rc);
    UASSERT(bytecode_has_op(&module, OP_AT_EVENT_SYNC_INSTALL));

    at_event_cleanup(&module, &arena, &vm);
}

/* Regression: when event_expr routes through AST_IDENT global-fallback or
 * AST_MEMBER_GET, those arms only bump e->next_reg without bumping
 * fs->freereg.  AST_AT_EVENT's subsequent emit_function_literal then
 * allocates body_reg from the stale freereg, colliding with event_reg.
 * OP_CLOSURE clobbers the event pointer at runtime; the install opcode
 * trips R[A] == R[B] (type confusion: closure interpreted as event).
 *
 * The fix syncs freereg to next_reg after emit_expr for the event
 * expression.  AST_WATCHER does not have this bug because cond is wrapped
 * in a closure (which routes through emit_function_literal symmetrically).
 *
 * This test compiles `at sync (Realm.evt?) body_val` and asserts the
 * emitted OP_AT_EVENT_SYNC_INSTALL has distinct event/body registers.
 * Pre-fix: event_reg == body_reg.  Post-fix: event_reg < body_reg. */
UTEST(emit_at_event_global_member_event_expr_disjoint_regs) {
    UModule  module = {0};
    UArena   arena;
    UVM      vm;
    UEmitter e;

    UEmitError rc = at_event_compile(
        "var body_val = 0; at sync (Realm.evt?) body_val",
        &module, &arena, &vm, &e);
    UASSERT_EQ(EMIT_OK, rc);

    /* Find the OP_AT_EVENT_SYNC_INSTALL instruction and confirm its
     * A (event_reg) and B (body_reg) operands are different. */
    bool found = false;
    size_t i;
    for (i = 0; i < module.instr_count; i++) {
        uint32_t inst = module.instructions[i];
        if (uinstr_op(inst) == OP_AT_EVENT_SYNC_INSTALL) {
            uint8_t a = uinstr_a(inst);
            uint8_t b = uinstr_b(inst);
            UASSERT(a != b);  /* event_reg must not collide with body_reg */
            found = true;
            break;
        }
    }
    UASSERT(found);

    at_event_cleanup(&module, &arena, &vm);
}

/* Sibling check for the async install path (OP_AT_EVENT_INSTALL): same
 * desync risk because the emit handler is symmetric in event_expr. */
UTEST(emit_at_event_async_global_member_event_expr_disjoint_regs) {
    UModule  module = {0};
    UArena   arena;
    UVM      vm;
    UEmitter e;

    UEmitError rc = at_event_compile(
        "var body_val = 0; at (Realm.evt?) body_val",
        &module, &arena, &vm, &e);
    UASSERT_EQ(EMIT_OK, rc);

    bool found = false;
    size_t i;
    for (i = 0; i < module.instr_count; i++) {
        uint32_t inst = module.instructions[i];
        if (uinstr_op(inst) == OP_AT_EVENT_INSTALL) {
            uint8_t a = uinstr_a(inst);
            uint8_t b = uinstr_b(inst);
            UASSERT(a != b);
            found = true;
            break;
        }
    }
    UASSERT(found);

    at_event_cleanup(&module, &arena, &vm);
}

/* -----------------------------------------------------------------------
 * Suite entry point
 * ----------------------------------------------------------------------- */

void test_parse_at_event_suite(void) {
    utest_run("parse_at_event_with_question_postfix",
              parse_at_event_with_question_postfix);
    utest_run("parse_at_sync_event",
              parse_at_sync_event);
    utest_run("parse_at_event_with_onleave",
              parse_at_event_with_onleave);
    utest_run("parse_at_event_no_onleave",
              parse_at_event_no_onleave);
    utest_run("parse_at_no_question_still_watcher",
              parse_at_no_question_still_watcher);
    utest_run("parse_question_outside_at_errors",
              parse_question_outside_at_errors);
    utest_run("parse_question_outside_at_standalone",
              parse_question_outside_at_standalone);
    utest_run("emit_at_event_produces_OP_AT_EVENT_INSTALL",
              emit_at_event_produces_OP_AT_EVENT_INSTALL);
    utest_run("emit_at_sync_event_produces_OP_AT_EVENT_SYNC_INSTALL",
              emit_at_sync_event_produces_OP_AT_EVENT_SYNC_INSTALL);
    utest_run("emit_at_event_global_member_event_expr_disjoint_regs",
              emit_at_event_global_member_event_expr_disjoint_regs);
    utest_run("emit_at_event_async_global_member_event_expr_disjoint_regs",
              emit_at_event_async_global_member_event_expr_disjoint_regs);
}
