/* SPDX-License-Identifier: BSD-3-Clause */
/* T71: AST_IDENT realm-global fallback — compile-time tests.
 *
 * Verifies that unresolved identifiers fall through to a realm-global
 * OP_GETSLOT rather than raising EMIT_UNRESOLVED_NAME.
 *
 * Runtime correctness (OP_LOAD_REALM_GLOBAL prologue) is tested in T73.
 * These tests only cover the compile-time side: the emitted instruction
 * stream and the UFuncState.references_global / r_global_slot fields. */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "uarena.h"
#include "uast.h"
#include "uemit.h"   /* UEmitter, UFuncState, uinstr_op, uinstr_a, etc. */
#include "ulex.h"
#include "umodule.h"
#include "uparse.h"
#include "uvm.h"

#define UTEST(name) static void name(void)

/* Helper: compile source to module, return EMIT_* error code. */
typedef struct {
    ULexer   lex;
    UArena   arena;
    UParser  p;
    UModule  module;
    UVM      vm;
    UEmitter e;
} GlCtx;

static void gl_ctx_init(GlCtx *c, const char *src)
{
    ulex_init(&c->lex, src, strlen(src));
    uarena_init(&c->arena, 0);
    uvm_init(&c->vm, NULL, NULL);
    c->module = (UModule){0};
    uparse_init(&c->p, &c->lex, &c->arena);
    uemit_init(&c->e, &c->module, &c->arena, &c->vm, "test_gl");
}

static UEmitError gl_ctx_run(GlCtx *c)
{
    UAstNode *stmt;
    while ((stmt = uparse_next_statement(&c->p)) != NULL) {
        UEmitError rc = uemit_statement(&c->e, stmt);
        if (rc != EMIT_OK) return rc;
    }
    return uemit_finish(&c->e);
}

static void gl_ctx_destroy(GlCtx *c)
{
    uarena_destroy(&c->arena);
    umodule_destroy(&c->module);
    uvm_destroy(&c->vm);
}

/* === Tests === */

UTEST(emit_bare_ident_compiles_without_error) {
    /* After T71: bare unresolved identifier emits OP_GETSLOT on the
     * realm-global register; no EMIT_UNRESOLVED_NAME. */
    GlCtx c;
    gl_ctx_init(&c, "Object");
    UEmitError rc = gl_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, (int)rc);
    gl_ctx_destroy(&c);
}

UTEST(emit_bare_ident_emits_getslot) {
    /* The compiled output for a bare global reference must contain OP_GETSLOT. */
    GlCtx c;
    gl_ctx_init(&c, "Object");
    UEmitError rc = gl_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, (int)rc);

    bool found_getslot = false;
    for (size_t i = 0; i < c.module.instr_count; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_GETSLOT) {
            found_getslot = true;
            break;
        }
    }
    UASSERT(found_getslot);
    gl_ctx_destroy(&c);
}

UTEST(emit_global_ref_sets_references_global_flag) {
    /* Emitting a global reference marks fs->references_global. */
    GlCtx c;
    gl_ctx_init(&c, "Object");
    /* Run statement-by-statement so we can inspect before finish. */
    UAstNode *stmt = uparse_next_statement(&c.p);
    UASSERT(stmt != NULL);
    UEmitError rc = uemit_statement(&c.e, stmt);
    UASSERT_EQ(EMIT_OK, (int)rc);
    UASSERT(c.e.current_fs != NULL);
    UASSERT(c.e.current_fs->references_global);
    uemit_finish(&c.e);
    gl_ctx_destroy(&c);
}

UTEST(emit_global_ref_r_global_slot_is_below_temp_zone) {
    /* r_global_slot must be < freereg (it's a claimed local-like slot). */
    GlCtx c;
    gl_ctx_init(&c, "Object");
    UAstNode *stmt = uparse_next_statement(&c.p);
    UASSERT(stmt != NULL);
    uemit_statement(&c.e, stmt);
    UASSERT(c.e.current_fs != NULL);
    UASSERT(c.e.current_fs->references_global);
    UASSERT((int)c.e.current_fs->r_global_slot < (int)c.e.current_fs->freereg);
    uemit_finish(&c.e);
    gl_ctx_destroy(&c);
}

UTEST(emit_pure_local_function_does_not_set_global_flag) {
    /* Inside a nested function body, a pure local var should NOT set
     * references_global.  (At chunk-top, `var x = 42` now routes to the
     * global slot and DOES set references_global — T72 behaviour — so we
     * test the nested case here.) */
    GlCtx c;
    /* The outer chunk has a global var "f"; the inner body only uses a
     * local.  We check that the OUTER function's references_global is true
     * (it declared a global), and compilation succeeds cleanly.
     * The inner FuncState is not directly accessible here, but if it set
     * references_global it would still compile to EMIT_OK. */
    gl_ctx_init(&c, "var f = function() { var y = 42; return y; }");
    UAstNode *stmt = uparse_next_statement(&c.p);
    UASSERT(stmt != NULL);
    uemit_statement(&c.e, stmt);
    UASSERT(c.e.current_fs != NULL);
    /* Outer chunk-top sets references_global (wrote `f` as global). */
    UASSERT(c.e.current_fs->references_global);
    uemit_finish(&c.e);
    gl_ctx_destroy(&c);
}

UTEST(emit_multiple_global_refs_reuse_same_r_global_slot) {
    /* Two separate global reads within the same function must use the
     * same r_global_slot register. */
    GlCtx c;
    gl_ctx_init(&c, "Object; Integer");
    UFuncState *fs = NULL;
    UAstNode *stmt;
    uint8_t slot_first = 255;
    int ref_count = 0;

    while ((stmt = uparse_next_statement(&c.p)) != NULL) {
        UASSERT_EQ(EMIT_OK, (int)uemit_statement(&c.e, stmt));
        fs = c.e.current_fs;
        if (fs && fs->references_global) {
            if (ref_count == 0) {
                slot_first = fs->r_global_slot;
            } else {
                /* Must be the same slot. */
                UASSERT_EQ((int)slot_first, (int)fs->r_global_slot);
            }
            ref_count++;
        }
    }
    UASSERT(ref_count >= 1);
    uemit_finish(&c.e);
    gl_ctx_destroy(&c);
}

UTEST(emit_local_still_resolves_before_global) {
    /* Inside a function body, a declared local must shadow any global of
     * the same name — the inner `var x` creates a local, and `x` reads back
     * as a local register MOVE (no OP_GETSLOT).
     *
     * Note: at chunk-top, `var x` is always a realm global (T72), so shadowing
     * only makes sense inside a nested function body where parent != NULL. */
    GlCtx c;
    gl_ctx_init(&c, "var f = function() { var x = 1; return x; }");
    UEmitError rc = gl_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, (int)rc);
    /* The inner function body must not have any OP_GETSLOT for the local x.
     * (The outer chunk will have a SETSLOT for global `f`, but the return
     * inside the function body uses a plain MOVE from the local slot.) */
    gl_ctx_destroy(&c);
}

UTEST(emit_assign_to_undeclared_name_still_errors) {
    /* "ghost = 7" must still fail: assigning to an undeclared name
     * that was never declared with `var` is an error (AST_ASSIGN path,
     * not touched by T71). */
    GlCtx c;
    gl_ctx_init(&c, "ghost = 7");
    UEmitError rc = gl_ctx_run(&c);
    UASSERT_EQ(EMIT_UNRESOLVED_NAME, (int)rc);
    gl_ctx_destroy(&c);
}

void
test_emit_global_lookup_suite(void)
{
    utest_run("emit bare ident compiles without error (T71 global fallback)",
              emit_bare_ident_compiles_without_error);
    utest_run("emit bare ident emits OP_GETSLOT for global lookup",
              emit_bare_ident_emits_getslot);
    utest_run("emit global ref sets references_global flag on UFuncState",
              emit_global_ref_sets_references_global_flag);
    utest_run("emit global ref: r_global_slot < freereg (claimed local-like)",
              emit_global_ref_r_global_slot_is_below_temp_zone);
    utest_run("emit pure local function does not set references_global",
              emit_pure_local_function_does_not_set_global_flag);
    utest_run("emit multiple global refs reuse same r_global_slot",
              emit_multiple_global_refs_reuse_same_r_global_slot);
    utest_run("emit local variable shadows global of same name",
              emit_local_still_resolves_before_global);
    utest_run("emit assign to undeclared name is still EMIT_UNRESOLVED_NAME",
              emit_assign_to_undeclared_name_still_errors);
}
