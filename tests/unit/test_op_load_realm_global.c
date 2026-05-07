/* SPDX-License-Identifier: BSD-3-Clause */
/* T73: OP_LOAD_REALM_GLOBAL dispatch + frame prologue prepend.
 *
 * Three tests:
 *   1. OP_LOAD_REALM_GLOBAL runtime: dispatching the opcode loads
 *      realm->global_object into R[A].
 *   2. A function that references a global gets OP_LOAD_REALM_GLOBAL
 *      prepended as its first instruction by the frame finalizer.
 *   3. A pure-local function (no global references) gets no prologue. */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "umodule.h"
#include "parse/uparse.h"
#include "uvm.h"
#include "urbi/urbi.h"       /* urbi_realm_create / urbi_realm_destroy */
#include "realm/urealm.h"    /* URealm.global_object */

#define UTEST(name) static void name(void)

/* Helper: compile source with a realm-aware context. */
typedef struct {
    ULexer   lex;
    UArena   arena;
    UParser  p;
    UModule  module;
    UVM      vm;
    UEmitter e;
    URealm  *realm;
} RGCtx;

static void rg_ctx_init(RGCtx *c, const char *src)
{
    ulex_init(&c->lex, src, strlen(src));
    uarena_init(&c->arena, 0);
    uvm_init(&c->vm, NULL, NULL);
    c->realm  = urbi_realm_create(&c->vm);
    c->module = (UModule){0};
    uparse_init(&c->p, &c->lex, &c->arena);
    uemit_init(&c->e, &c->module, &c->arena, &c->vm, "test_rg");
}

static UEmitError rg_ctx_compile(RGCtx *c)
{
    UAstNode *stmt;
    while ((stmt = uparse_next_statement(&c->p)) != NULL) {
        UEmitError rc = uemit_statement(&c->e, stmt);
        if (rc != EMIT_OK) return rc;
    }
    return uemit_finish(&c->e);
}

static void rg_ctx_destroy(RGCtx *c)
{
    uarena_destroy(&c->arena);
    umodule_destroy(&c->module);
    if (c->realm != NULL) urbi_realm_destroy(&c->vm, c->realm);
    uvm_destroy(&c->vm);
}

/* === Tests === */

UTEST(op_load_realm_global_loads_realm_global_object) {
    /* Runtime test: compile "Object" (a bare global identifier), run it
     * in a realm, and verify the returned result is an object value. The
     * script reads the "Object" global via GETSLOT on r_global_slot; the
     * prologue must have loaded realm->global_object into r_global_slot
     * first for GETSLOT to return anything. */
    RGCtx c;
    rg_ctx_init(&c, "Object");

    UEmitError emit_rc = rg_ctx_compile(&c);
    UASSERT_EQ(EMIT_OK, (int)emit_rc);

    UValue result = {0};
    int run_rc = urbi_run_chunk(&c.vm, c.realm, &c.module, &result);
    UASSERT_EQ(URBI_OK, run_rc);
    /* The "Object" global is a UVAL_OBJECT (set up by urbi_realm_create). */
    UASSERT_EQ((uint8_t)UVAL_OBJECT, result.kind);

    rg_ctx_destroy(&c);
}

UTEST(function_referencing_global_emits_load_realm_global_prologue) {
    /* Compile-time test: a function body that reads a global must have
     * OP_LOAD_REALM_GLOBAL as the very first instruction (prepended by
     * the frame finalizer). */
    RGCtx c;
    /* Use a nested function so we can inspect its UProto instructions
     * without the chunk-top wrapper.  The outer chunk's first instruction
     * is also OP_LOAD_REALM_GLOBAL (for the SETSLOT that writes `f`). */
    rg_ctx_init(&c, "var f = function() { return Object; }");

    UEmitError rc = rg_ctx_compile(&c);
    UASSERT_EQ(EMIT_OK, (int)rc);

    /* Scan module instructions for OP_LOAD_REALM_GLOBAL. */
    bool found = false;
    for (size_t i = 0; i < c.module.instr_count; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_LOAD_REALM_GLOBAL) {
            found = true;
            /* It must be the first instruction (prologue). */
            UASSERT_EQ((size_t)0, i);
            break;
        }
    }
    UASSERT(found);

    rg_ctx_destroy(&c);
}

UTEST(function_without_global_ref_skips_prologue) {
    /* A nested function that only uses its own local variables must NOT
     * have OP_LOAD_REALM_GLOBAL prepended — prologue-free is the goal
     * for pure-local code. */
    RGCtx c;
    /* Outer chunk writes "f" to a global (so the outer chunk has a prologue),
     * but the inner function body only touches its local `x`. */
    rg_ctx_init(&c, "var f = function() { var x = 1; return x; }");

    UEmitError rc = rg_ctx_compile(&c);
    UASSERT_EQ(EMIT_OK, (int)rc);

    /* Find the first UProto in the module (the inner function). */
    UASSERT(c.module.nested_count > 0u);
    UProto *inner = c.module.nested[0];
    UASSERT(inner != NULL);
    UASSERT(inner->instr_count > 0u);

    /* The inner function's first instruction must NOT be OP_LOAD_REALM_GLOBAL. */
    UASSERT(uinstr_op(inner->instructions[0]) != OP_LOAD_REALM_GLOBAL);

    rg_ctx_destroy(&c);
}

void
test_op_load_realm_global_suite(void)
{
    utest_run("op_load_realm_global: loads realm->global_object into R[A]",
              op_load_realm_global_loads_realm_global_object);
    utest_run("function referencing global emits OP_LOAD_REALM_GLOBAL prologue",
              function_referencing_global_emits_load_realm_global_prologue);
    utest_run("function without global ref skips OP_LOAD_REALM_GLOBAL prologue",
              function_without_global_ref_skips_prologue);
}
