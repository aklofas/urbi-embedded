/* SPDX-License-Identifier: BSD-3-Clause */
/* End-to-end: scripted at sync (cond) body fires inline through real
 * bytecode dispatch.
 *
 * Companion to test_at_scripted_e2e.c (the AT-mode version), but exercises
 * the AT_SYNC path: the body runs synchronously on the scratch frame inside
 * watcher_eval_dirty (no body-strand spawn).  No test hooks installed —
 * the watcher's cond closure goes through urbi_run_closure_on_scratch and
 * the body goes through invoke_body_inline → urbi_run_closure_on_scratch.
 *
 * Body observes its run via Realm.fired counter — incremented inside the
 * body, read by the host afterwards via urbi_realm_get_global. */

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
#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "watcher/uwatcher.h"

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers (mirrors test_at_scripted_e2e.c)
 * =================================================================== */

static UValue
make_int(int64_t n)
{
    UValue v;
    v.kind = UVAL_INT;
    v.v.i  = n;
    return v;
}

static int
compile_and_run(UVM *vm, const char *src, UValue *out_result)
{
    URealm *realm = urbi_realm_global(vm);
    if (realm == NULL) return URBI_ERR_OOM;

    ULexer   lex;
    UArena   arena;
    UModule  module = {0};
    UEmitter e;
    UParser  p;
    UAstNode *node;

    ulex_init(&lex, src, strlen(src));
    uarena_init(&arena, 4096);
    uemit_init(&e, &module, &arena, vm, NULL);
    uparse_init(&p, &lex, &arena);

    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) {
            uarena_destroy(&arena);
            umodule_destroy(&module);
            return URBI_ERR_COMPILE;
        }
        if (uemit_statement(&e, node) != EMIT_OK) {
            uarena_destroy(&arena);
            umodule_destroy(&module);
            return URBI_ERR_COMPILE;
        }
        uarena_reset(&arena);
    }
    if (uemit_finish(&e) != EMIT_OK) {
        uarena_destroy(&arena);
        umodule_destroy(&module);
        return URBI_ERR_COMPILE;
    }

    UValue result = {0};
    int rc = urbi_run_chunk(vm, realm, &module, &result);
    if (out_result != NULL) {
        *out_result = result;
    }
    uarena_destroy(&arena);
    umodule_destroy(&module);
    return rc;
}

#define E2E_MAX_ITERS 1000

static int
run_to_no_runnable(UVM *vm)
{
    int i;
    for (i = 0; i < E2E_MAX_ITERS; i++) {
        UStepResult sr = urbi_step(vm, 1000, NULL);
        if (sr == URBI_STEP_FATAL)     return -1;
        if (sr == URBI_STEP_WAKE_AT)   return 1;
        if (sr == URBI_STEP_QUIESCENT) return 1;
        if (vm->strand_runnable_count == 0) return 1;
    }
    return 0;
}

/* ===================================================================
 * Test: scripted_at_sync_fires_on_rising_edge
 *
 * Install at sync (Realm.x > 5) Realm.fired = Realm.fired + 1 via a real
 * compiled script.  Trigger the rising edge by writing Realm.x = 10
 * through a nested function call (so the non-top-frame OP_RET safepoint
 * fires watcher_eval_dirty).  Unlike AT mode, the body runs inline via
 * invoke_body_inline (no body strand) — so Realm.fired must equal 1
 * immediately after compile_and_run returns.
 *
 * A second write (Realm.x = 20) must not re-fire (rising-edge discipline).
 * =================================================================== */
UTEST(scripted_at_sync_fires_on_rising_edge)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    URealm *gr = urbi_realm_global(&vm);
    UASSERT(gr != NULL);
    if (gr == NULL) { uvm_destroy(&vm); return; }

    int rc;
    rc = urbi_realm_set_global(&vm, gr, "x",     1, make_int(0));
    UASSERT_EQ(URBI_OK, rc);
    rc = urbi_realm_set_global(&vm, gr, "fired", 5, make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    /* === Phase 1: install the at sync watcher === */
    rc = compile_and_run(&vm,
        "at sync (Realm.x > 5) Realm.fired = Realm.fired + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);
    if (rc != URBI_OK) {
        uvm_destroy(&vm);
        return;
    }

    UASSERT(vm.active_watchers_head != NULL);

    UValue fired = {0};
    rc = urbi_realm_get_global(&vm, gr, "fired", 5, &fired);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT_EQ(0, (int)fired.v.i);

    /* === Phase 2: trigger the rising edge ===
     *
     * The non-top OP_RET safepoint fires watcher_eval_dirty.  For AT_SYNC,
     * the body runs inline on the scratch frame (no strand spawn).  So
     * Realm.fired must equal 1 by the time compile_and_run returns. */
    rc = compile_and_run(&vm,
        "var __trigger__ = function() { Realm.x = 10 }; __trigger__()",
        NULL);
    if (rc != URBI_OK) {
        while (vm.active_watchers_head != NULL)
            urbi_watcher_unregister_internal(&vm, vm.active_watchers_head);
        uvm_destroy(&vm);
        return;
    }

    /* AT_SYNC: body fired inline; no strand to drain.  Run to quiescence
     * for symmetry with the AT-mode test (defensive — should be no-op). */
    int step_rc = run_to_no_runnable(&vm);
    UASSERT(step_rc != -1);

    rc = urbi_realm_get_global(&vm, gr, "fired", 5, &fired);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT_EQ(1, (int)fired.v.i);

    /* === Phase 3: same-direction write must NOT re-fire === */
    rc = compile_and_run(&vm,
        "var __trigger2__ = function() { Realm.x = 20 }; __trigger2__()",
        NULL);
    (void)rc;

    (void)run_to_no_runnable(&vm);

    rc = urbi_realm_get_global(&vm, gr, "fired", 5, &fired);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT_EQ(1, (int)fired.v.i);

    /* === Cleanup === */
    while (vm.active_watchers_head != NULL)
        urbi_watcher_unregister_internal(&vm, vm.active_watchers_head);
    uvm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_at_sync_scripted_suite(void)
{
    printf("test_at_sync_scripted\n");
    utest_run("scripted_at_sync_fires_on_rising_edge",
              scripted_at_sync_fires_on_rising_edge);
}
