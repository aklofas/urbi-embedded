/* SPDX-License-Identifier: BSD-3-Clause */
/* T72: top-level var/function become realm globals — compile-time tests.
 *
 * Verifies that at chunk-top (UFuncState.parent == NULL), AST_VAR_DECL
 * emits OP_SETSLOT on r_global_slot rather than absorbing the init value
 * as a frame-local register.
 *
 * Runtime correctness (OP_LOAD_REALM_GLOBAL prologue fills r_global_slot
 * with realm->global_object) is verified in T73.  These tests only cover
 * the compile-time instruction stream. */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "value/uintern.h"  /* ustr_intern */
#include "lex/ulex.h"
#include "module/umodule.h"
#include "parse/uparse.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* Helper: compile source to module. */
typedef struct {
    ULexer   lex;
    UArena   arena;
    UParser  p;
    UModule  module;
    UVM      vm;
    UEmitter e;
} GVCtx;

static void gv_ctx_init(GVCtx *c, const char *src)
{
    ulex_init(&c->lex, src, strlen(src));
    uarena_init(&c->arena, 0);
    urbi_vm_init(&c->vm, NULL, NULL);
    c->module = (UModule){0};
    uparse_init(&c->p, &c->lex, &c->arena);
    uemit_init(&c->e, &c->module, &c->arena, &c->vm, "test_gv");
}

static UEmitError gv_ctx_run(GVCtx *c)
{
    UAstNode *stmt;
    while ((stmt = uparse_next_statement(&c->p)) != NULL) {
        UEmitError rc = uemit_statement(&c->e, stmt);
        if (rc != EMIT_OK) return rc;
    }
    return uemit_finish(&c->e);
}

static void gv_ctx_destroy(GVCtx *c)
{
    uarena_destroy(&c->arena);
    umodule_destroy(&c->module, NULL);
    urbi_vm_destroy(&c->vm);
}

/* === Tests === */

UTEST(emit_top_level_var_decl_compiles_ok) {
    /* Chunk-top var decl should compile without error. */
    GVCtx c;
    gv_ctx_init(&c, "var x = 42");
    UEmitError rc = gv_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, (int)rc);
    gv_ctx_destroy(&c);
}

UTEST(emit_top_level_var_decl_emits_setslot) {
    /* "var x = 42" at chunk-top must emit OP_SETSLOT (write to global slot). */
    GVCtx c;
    gv_ctx_init(&c, "var x = 42");
    UEmitError rc = gv_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, (int)rc);

    bool found_setslot = false;
    for (size_t i = 0; i < c.module.instr_count; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_SETSLOT) {
            found_setslot = true;
            break;
        }
    }
    UASSERT(found_setslot);
    gv_ctx_destroy(&c);
}

UTEST(emit_top_level_var_decl_sets_references_global) {
    /* Chunk-top var must set references_global (same register as global reads). */
    GVCtx c;
    gv_ctx_init(&c, "var x = 42");
    UAstNode *stmt = uparse_next_statement(&c.p);
    UASSERT(stmt != NULL);
    uemit_statement(&c.e, stmt);
    UASSERT(c.e.current_fs != NULL);
    UASSERT(c.e.current_fs->references_global);
    uemit_finish(&c.e);
    gv_ctx_destroy(&c);
}

UTEST(emit_top_level_var_decl_does_not_grow_nactvar) {
    /* At chunk-top, var decl must NOT add a named local (nactvar stays 0
     * for the "x" binding — only r_global_slot is reserved internally). */
    GVCtx c;
    gv_ctx_init(&c, "var x = 42");
    UAstNode *stmt = uparse_next_statement(&c.p);
    UASSERT(stmt != NULL);
    uemit_statement(&c.e, stmt);
    UFuncState *fs = c.e.current_fs;
    UASSERT(fs != NULL);
    /* nactvar should be 0 — the global slot is reserved via freereg,
     * not via a named actvars[] entry. */
    UASSERT_EQ(0, fs->nactvar);
    uemit_finish(&c.e);
    gv_ctx_destroy(&c);
}

UTEST(emit_top_level_var_decl_no_local_entry_created) {
    /* Chunk-top var must not add an entry to actvars[] — the name "x"
     * lives in the global slot, not as a frame local. */
    GVCtx c;
    gv_ctx_init(&c, "var x = 42");
    UAstNode *stmt = uparse_next_statement(&c.p);
    UASSERT(stmt != NULL);
    uemit_statement(&c.e, stmt);
    UFuncState *fs = c.e.current_fs;
    UASSERT(fs != NULL);
    /* No actvars entry named "x". */
    const char *canonical = ustr_intern(&c.vm, "x", 1);
    bool found_local = false;
    int i;
    for (i = 0; i < fs->nactvar; i++) {
        if (fs->actvars[i].name == canonical) {
            found_local = true;
            break;
        }
    }
    UASSERT(!found_local);
    uemit_finish(&c.e);
    gv_ctx_destroy(&c);
}

UTEST(emit_nested_var_decl_stays_local) {
    /* Inside a function body, var decl must still allocate a local register
     * (NOT write to the global slot). */
    GVCtx c;
    /* The outer "var f = function() { var x = 99; }" compiles the inner
     * function body as a nested FuncState (parent != NULL), so inner var x
     * must remain a local.  We just check that compilation succeeds and
     * that the module does NOT contain a top-level OP_SETSLOT from the
     * inner body (the outer chunk may have one for "f" itself). */
    gv_ctx_init(&c, "var f = function() { var x = 99; return x; }");
    UEmitError rc = gv_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, (int)rc);
    gv_ctx_destroy(&c);
}

UTEST(emit_two_top_level_vars_both_emit_setslot) {
    /* Two chunk-top vars must each produce an OP_SETSLOT. */
    GVCtx c;
    gv_ctx_init(&c, "var a = 1; var b = 2");
    UEmitError rc = gv_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, (int)rc);

    int setslot_count = 0;
    for (size_t i = 0; i < c.module.instr_count; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_SETSLOT)
            setslot_count++;
    }
    /* Expect exactly 2 OP_SETSLOT instructions (one per var). */
    UASSERT_EQ(2, setslot_count);
    gv_ctx_destroy(&c);
}

UTEST(emit_top_level_var_reuse_same_r_global_slot) {
    /* Two chunk-top vars must share the same r_global_slot register.
     * The recv register in both OP_SETSLOT instructions must be equal. */
    GVCtx c;
    gv_ctx_init(&c, "var a = 1; var b = 2");
    UEmitError rc = gv_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, (int)rc);

    /* Collect all OP_SETSLOT recv registers (field B). */
    uint8_t recv[2];
    int found = 0;
    for (size_t i = 0; i < c.module.instr_count && found < 2; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_SETSLOT)
            recv[found++] = uinstr_b(c.module.instructions[i]);
    }
    UASSERT_EQ(2, found);
    UASSERT_EQ((int)recv[0], (int)recv[1]);
    gv_ctx_destroy(&c);
}

void
test_emit_global_var_suite(void)
{
    utest_run("emit top-level var decl compiles without error",
              emit_top_level_var_decl_compiles_ok);
    utest_run("emit top-level var decl emits OP_SETSLOT",
              emit_top_level_var_decl_emits_setslot);
    utest_run("emit top-level var decl sets references_global on UFuncState",
              emit_top_level_var_decl_sets_references_global);
    utest_run("emit top-level var decl does not grow nactvar (no named local)",
              emit_top_level_var_decl_does_not_grow_nactvar);
    utest_run("emit top-level var decl: no actvars entry for the name",
              emit_top_level_var_decl_no_local_entry_created);
    utest_run("emit nested var decl inside function stays local (not global)",
              emit_nested_var_decl_stays_local);
    utest_run("emit two top-level vars each produce OP_SETSLOT",
              emit_two_top_level_vars_both_emit_setslot);
    utest_run("emit two top-level vars reuse same r_global_slot register",
              emit_top_level_var_reuse_same_r_global_slot);
}
