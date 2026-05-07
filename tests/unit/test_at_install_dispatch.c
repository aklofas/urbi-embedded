/* SPDX-License-Identifier: BSD-3-Clause */
/* Integration tests: OP_AT_INSTALL / OP_AT_SYNC_INSTALL / OP_WHENEVER_INSTALL
 * dispatch (T41, spec #2 §6.3); and OP_WAITUNTIL_INSTALL immediate-wake
 * (T42, spec #2 §6.3).
 *
 * T41 cases:
 *   1. at_install_runs_to_completion:
 *      Compile and run "var x = 0; at (x > 5) x;" — no panic; UVM_OK; watcher
 *      installed (active_watchers_head non-NULL); cond starts false so no fire.
 *
 *   2. whenever_install_runs_to_completion:
 *      Compile and run "var c = 0; var b = 0; whenever (c) b;" — no panic; UVM_OK.
 *
 *   3. at_sync_install_runs_to_completion:
 *      Compile and run "var c = 0; var b = 0; at sync (c) b;" — no panic; UVM_OK.
 *
 * T42 case (added in T42 commit, stub disabled until then):
 *   4. waituntil_does_not_yield_when_cond_true:
 *      Compile and run "var x = 1; waituntil (x);" — cond starts true so the
 *      install fast-path unregisters immediately.  Strand stays RUNNABLE (not
 *      WAITING), uvm_run returns UVM_OK, and active_watchers_head is NULL. */

#include "utest.h"
#include "value/uarena.h"
#include "parse/uast.h"
#include "module/umodule.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "vm/uvm.h"
#include "watcher/uwatcher.h"   /* UWATCHER_AT, urbi_watcher_unregister_internal */

#include <string.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers
 * =================================================================== */

typedef struct {
    UVM     vm;
    UModule module;
    UArena  arena;
} PipeCtx;

/* compile_source: lex+parse+emit src into ctx.  Returns 0 on success, -1 on
 * compile error.  Caller must call pipeline_ctx_destroy when done. */
static int
compile_source(PipeCtx *ctx, const char *src)
{
    uvm_init(&ctx->vm, NULL, NULL);
    uarena_init(&ctx->arena, 4096);
    memset(&ctx->module, 0, sizeof(ctx->module));

    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UEmitter e;
    uemit_init(&e, &ctx->module, &ctx->arena, &ctx->vm, NULL);

    UParser p;
    uparse_init(&p, &lex, &ctx->arena);

    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) break;
        (void)uemit_statement(&e, node);
        uarena_reset(&ctx->arena);
    }

    return (uemit_finish(&e) == EMIT_OK) ? 0 : -1;
}

static void
pipeline_ctx_destroy(PipeCtx *ctx)
{
    /* Drain active watchers so uvm_destroy is clean. */
    while (ctx->vm.active_watchers_head != NULL)
        urbi_watcher_unregister_internal(&ctx->vm,
                                         ctx->vm.active_watchers_head);
    umodule_destroy(&ctx->module);
    uarena_destroy(&ctx->arena);
    uvm_destroy(&ctx->vm);
}

/* ===================================================================
 * T41 test cases
 * =================================================================== */

/* 1. at_install_runs_to_completion
 *
 * "var x = 0; at (x > 5) x;" must:
 *   - Compile without error.
 *   - Run to completion (UVM_OK) — no panic from M5 stub.
 *   - Leave a watcher in active_watchers_head (cond starts false → installed). */
UTEST(at_install_runs_to_completion)
{
    PipeCtx ctx;
    int rc = compile_source(&ctx, "var x = 0; at (x > 5) x");
    UASSERT_EQ(0, rc);

    UValue out;
    UVMError vm_rc = uvm_run(&ctx.vm, &ctx.module, &out);
    UASSERT_EQ(UVM_OK, (int)vm_rc);
    /* Watcher installed (cond starts false, no fire). */
    UASSERT(ctx.vm.active_watchers_head != NULL);

    pipeline_ctx_destroy(&ctx);
}

/* 2. whenever_install_runs_to_completion
 *
 * "var c = 0; var b = 0; whenever (c) b" — no panic; UVM_OK. */
UTEST(whenever_install_runs_to_completion)
{
    PipeCtx ctx;
    int rc = compile_source(&ctx, "var c = 0; var b = 0; whenever (c) b");
    UASSERT_EQ(0, rc);

    UValue out;
    UVMError vm_rc = uvm_run(&ctx.vm, &ctx.module, &out);
    UASSERT_EQ(UVM_OK, (int)vm_rc);

    pipeline_ctx_destroy(&ctx);
}

/* 3. at_sync_install_runs_to_completion
 *
 * "var c = 0; var b = 0; at sync (c) b" — no panic; UVM_OK. */
UTEST(at_sync_install_runs_to_completion)
{
    PipeCtx ctx;
    int rc = compile_source(&ctx, "var c = 0; var b = 0; at sync (c) b");
    UASSERT_EQ(0, rc);

    UValue out;
    UVMError vm_rc = uvm_run(&ctx.vm, &ctx.module, &out);
    UASSERT_EQ(UVM_OK, (int)vm_rc);

    pipeline_ctx_destroy(&ctx);
}

/* ===================================================================
 * Cond hook helpers for T42
 * =================================================================== */

/* hook_cond_true: simulates a cond closure returning bool-true. */
static void
hook_cond_true(struct UVM *vm, struct UClosure *cond,
               UValue *out_result, int *out_threw)
{
    (void)vm; (void)cond;
    out_result->kind = (uint8_t)UVAL_BOOL;
    out_result->v.i  = 1;
    *out_threw = 0;
}

/* ===================================================================
 * T42 test case: OP_WAITUNTIL_INSTALL immediate-wake
 * =================================================================== */

/* 4. waituntil_does_not_yield_when_cond_true
 *
 * Install a waituntil with a hook that returns truthy.  The T40 fast-path
 * must unregister the watcher immediately, leaving the strand RUNNING.
 * uvm_run returns UVM_OK; active_watchers_head is NULL (watcher freed). */
UTEST(waituntil_does_not_yield_when_cond_true)
{
    PipeCtx ctx;
    /* Compile "var x = 0; waituntil (x)" — the hook will override the
     * cond evaluation to return truthy regardless of x's actual value. */
    int rc = compile_source(&ctx, "var x = 0; waituntil (x)");
    UASSERT_EQ(0, rc);

    /* Install hook before run: cond always true → immediate-wake path. */
    ctx.vm.test_install_cond_hook = hook_cond_true;

    UValue out;
    UVMError vm_rc = uvm_run(&ctx.vm, &ctx.module, &out);

    ctx.vm.test_install_cond_hook = NULL;

    UASSERT_EQ(UVM_OK, (int)vm_rc);
    /* Watcher unregistered immediately — no watcher survives. */
    UASSERT(ctx.vm.active_watchers_head == NULL);

    pipeline_ctx_destroy(&ctx);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_at_install_dispatch_suite(void)
{
    printf("test_at_install_dispatch\n");
    utest_run("at_install_runs_to_completion",
              at_install_runs_to_completion);
    utest_run("whenever_install_runs_to_completion",
              whenever_install_runs_to_completion);
    utest_run("at_sync_install_runs_to_completion",
              at_sync_install_runs_to_completion);
    utest_run("waituntil_does_not_yield_when_cond_true",
              waituntil_does_not_yield_when_cond_true);
}
