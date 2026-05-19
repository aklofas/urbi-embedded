/* SPDX-License-Identifier: BSD-3-Clause */
/* Phase 3 cluster 1 (T20-T23): emit error-path correctness.
 *
 * T20 (EMIT-003): uemit_close_function captures prologue_prepend_instr
 *                 return value and gates downstream IC-array work on
 *                 e->error so a partial-prepend proto is not extended.
 * T21 (EMIT-004): emit_function_literal cleans up partial child_proto on
 *                 intern OOM (no half-initialised proto in module->nested[]).
 * T22 (EMIT-005): uemit_close_function propagates IC-array OOM rather than
 *                 silently zeroing p->ic_count.
 * T23 (SCAN-001): emit_expr explicitly handles AST_PROP_GET / AST_PROP_SET
 *                 (closes scan-build -Wswitch concern).
 *
 * The OOM-injection tests sweep failure points across the alloc range and
 * assert (a) some injection produces EMIT_OOM and (b) no injection produces
 * a phantom EMIT_OK.  They exercise the error path so future regressions
 * have a regression-coverage seat.
 */

#include "utest.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "value/uarena.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "emit/uemit.h"
#include "chunk/umodule.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* --- OOM-injection allocator -------------------------------------------- */

typedef struct {
    int alloc_calls;     /* count of NEW allocations (ptr==NULL, n>0) */
    int fail_at;         /* fail when alloc_calls > fail_at; -1 = never */
} EmitSpy;

static void *
emit_spy_alloc(void *ptr, size_t n, void *ud)
{
    EmitSpy *spy = (EmitSpy *)ud;
    if (n == 0) {
        free(ptr);
        return NULL;
    }
    if (ptr == NULL) {
        spy->alloc_calls++;
        if (spy->fail_at >= 0 && spy->alloc_calls > spy->fail_at)
            return NULL;
    }
    return realloc(ptr, n);
}

/* --- Test harness -------------------------------------------------------- */

typedef struct {
    ULexer   lex;
    UArena   arena;
    UParser  p;
    UModule  module;
    UVM      vm;
    UEmitter e;
    EmitSpy  spy;
} ECtx;

static void
ectx_init(ECtx *c, const char *src, int fail_at)
{
    ulex_init(&c->lex, src, strlen(src));
    uarena_init(&c->arena, 0);
    /* Use the spy on BOTH the VM and the module so emit-time allocations
     * route through it.  Module allocator drives proto/instr/IC growth;
     * VM allocator drives ustr_intern. */
    c->spy.alloc_calls = 0;
    c->spy.fail_at = fail_at;
    urbi_vm_init(&c->vm, emit_spy_alloc, &c->spy);
    c->module = (UModule){0};
    c->module.alloc_fn = emit_spy_alloc;
    c->module.alloc_ud = &c->spy;
    uparse_init(&c->p, &c->lex, &c->arena);
    uemit_init(&c->e, &c->module, &c->arena, &c->vm, "test");
}

static UEmitError
ectx_run(ECtx *c)
{
    UAstNode *stmt;
    while ((stmt = uparse_next_statement(&c->p)) != NULL) {
        UEmitError rc = uemit_statement(&c->e, stmt);
        if (rc != EMIT_OK) break;
    }
    return uemit_finish(&c->e);
}

static void
ectx_destroy(ECtx *c)
{
    /* Disable failure injection during teardown so destruction can free. */
    c->spy.fail_at = -1;
    uarena_destroy(&c->arena);
    umodule_destroy(&c->module, NULL);
    urbi_vm_destroy(&c->vm);
}

/* --- T20: prologue_prepend_instr return-value propagation --------------- */

UTEST(emit_close_function_propagates_prologue_oom)
{
    /* Source that forces uemit_close_function to enter the prologue path:
     * a function body that references a realm global (`g`) sets
     * fs->references_global = true, so close emits OP_LOAD_REALM_GLOBAL via
     * prologue_prepend_instr.  An OOM injected during the prologue grow
     * path must propagate as EMIT_OOM, and the IC-array branches that
     * follow must be gated on e->error so they do not extend a proto
     * whose instructions buffer is in an indeterminate partial-shift
     * state. */
    const char *src = "var f = function() { return g + 1 }";

    /* Probe successful run for the alloc count baseline. */
    int total;
    {
        ECtx c;
        ectx_init(&c, src, -1);
        UEmitError rc = ectx_run(&c);
        UASSERT_EQ(EMIT_OK, rc);
        total = c.spy.alloc_calls;
        ectx_destroy(&c);
    }
    UASSERT(total > 4);

    /* Walk failure points across the alloc range to hit the close-time
     * prologue/IC allocations.  At least one injection point must produce
     * EMIT_OOM; for any point that triggers failure, the result must be a
     * non-OK error code (never EMIT_OK with a corrupted proto). */
    bool saw_oom = false;
    for (int fail_at = 0; fail_at <= total + 1; fail_at++) {
        ECtx c;
        ectx_init(&c, src, fail_at);
        UEmitError rc = ectx_run(&c);
        if (rc == EMIT_OOM) saw_oom = true;
        if (c.spy.alloc_calls > c.spy.fail_at) {
            UASSERT(rc != EMIT_OK);
        }
        ectx_destroy(&c);
    }
    UASSERT(saw_oom);
}

/* --- T21: emit_function_literal cleanup on intern OOM ------------------- */

UTEST(emit_function_literal_clean_on_intern_oom)
{
    /* A function with multiple parameters: emit_function_literal interns
     * each parameter name in sequence.  Pre-T21, a mid-loop ustr_intern
     * OOM left a half-initialised UProto stuck in module->nested[].  The
     * fix interns all names BEFORE allocating child_proto, so an intern
     * OOM short-circuits with module->nested_count unchanged. */
    const char *src = "var f = function(a, b, c) { return a + b + c }";

    int total;
    {
        ECtx c;
        ectx_init(&c, src, -1);
        UEmitError rc = ectx_run(&c);
        UASSERT_EQ(EMIT_OK, rc);
        total = c.spy.alloc_calls;
        ectx_destroy(&c);
    }
    UASSERT(total > 4);

    /* For any injection that fails: emit must report failure and the
     * module must destroy cleanly (no double-free, no use-after-free). */
    bool saw_failure = false;
    for (int fail_at = 0; fail_at <= total + 1; fail_at++) {
        ECtx c;
        ectx_init(&c, src, fail_at);
        UEmitError rc = ectx_run(&c);
        if (rc != EMIT_OK) saw_failure = true;
        if (c.spy.alloc_calls > c.spy.fail_at) {
            UASSERT(rc != EMIT_OK);
        }
        ectx_destroy(&c);
    }
    UASSERT(saw_failure);
}

/* --- T22: IC-array OOM propagation -------------------------------------- */

UTEST(emit_close_function_propagates_ic_array_oom)
{
    /* Source with multiple slot accesses to grow ic_next > 0; the
     * IC-array allocation in uemit_close_function copies fs->ic_names
     * into target_proto->ic_names.  When that allocation fails, the
     * emit must propagate EMIT_OOM cleanly.  Pre-T22 fix the proto path
     * also zeroed p->ic_count (silent zeroing); post-fix p->ic_count
     * stays at its zero-init value and the error propagates via
     * e->error alone, matching the module-sibling path. */
    const char *src = "var f = function(o) { return o.a + o.b + o.c }";

    int total;
    {
        ECtx c;
        ectx_init(&c, src, -1);
        UEmitError rc = ectx_run(&c);
        UASSERT_EQ(EMIT_OK, rc);
        total = c.spy.alloc_calls;
        ectx_destroy(&c);
    }
    UASSERT(total > 4);

    bool saw_oom = false;
    for (int fail_at = 0; fail_at <= total + 1; fail_at++) {
        ECtx c;
        ectx_init(&c, src, fail_at);
        UEmitError rc = ectx_run(&c);
        if (rc == EMIT_OOM) saw_oom = true;
        if (c.spy.alloc_calls > c.spy.fail_at) {
            UASSERT(rc != EMIT_OK);
        }
        ectx_destroy(&c);
    }
    UASSERT(saw_oom);
}

/* --- T23: AST_PROP_GET / AST_PROP_SET handling -------------------------- */

UTEST(emit_expr_rejects_arrow_prop_get)
{
    /* Arrow-access syntax `obj.x->y` parses to AST_PROP_GET.  v0.5.7 has
     * no runtime support for arrow-access semantics; the emit path
     * rejects with EMIT_UNSUPPORTED_AST via an explicit case arm rather
     * than the prior NOLINT-suppressed default fall-through.  This test
     * locks in that behaviour as a regression seat. */
    ECtx c;
    ectx_init(&c, "var f = function(o) { return o.x->y }", -1);
    UEmitError rc = ectx_run(&c);
    UASSERT_EQ(EMIT_UNSUPPORTED_AST, rc);
    UASSERT_EQ(EMIT_UNSUPPORTED_AST, c.e.error);
    ectx_destroy(&c);
}

UTEST(emit_expr_rejects_arrow_prop_set)
{
    /* Arrow-access assignment `obj.x->y = v` parses to AST_PROP_SET. */
    ECtx c;
    ectx_init(&c, "var f = function(o, v) { o.x->y = v }", -1);
    UEmitError rc = ectx_run(&c);
    UASSERT_EQ(EMIT_UNSUPPORTED_AST, rc);
    UASSERT_EQ(EMIT_UNSUPPORTED_AST, c.e.error);
    ectx_destroy(&c);
}

/* --- Suite registration -------------------------------------------------- */

void
test_emit_error_paths_suite(void)
{
    utest_run("emit_close_function propagates prologue OOM",
              emit_close_function_propagates_prologue_oom);
    utest_run("emit_function_literal clean on intern OOM",
              emit_function_literal_clean_on_intern_oom);
    utest_run("emit_close_function propagates ic_array OOM",
              emit_close_function_propagates_ic_array_oom);
    utest_run("emit_expr rejects arrow prop_get",
              emit_expr_rejects_arrow_prop_get);
    utest_run("emit_expr rejects arrow prop_set",
              emit_expr_rejects_arrow_prop_set);
}
