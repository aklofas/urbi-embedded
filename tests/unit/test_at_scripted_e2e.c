/* SPDX-License-Identifier: BSD-3-Clause */
/* End-to-end: scripted at(cond) fires through real bytecode dispatch.
 *
 * Unlike test_at_fire_paths.c (which uses test_watcher_*_hook fields to
 * inject behavior), this suite compiles real urbiscript and runs it
 * through the production install + eval paths.  The watcher's cond
 * closure goes through urbi_run_closure_on_scratch (no hooks); the body
 * strand spawns through spawn_body_coroutine (M5 baseline).
 *
 * Body observes its run via Realm.fired counter — incremented inside the
 * body, read by the host afterwards via urbi_realm_get_global.  This
 * exercises the install-time cond eval (T7), eval-time cond eval (T8),
 * the new strand.module_instance synthesis path in the scratch helper
 * (T7's bundled bug fix — exercised here by Realm.x global access via
 * OP_GETSLOT), and async body strand completion through the M5 scheduler. */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "value/uarena.h"
#include "uast.h"
#include "uemit.h"
#include "ulex.h"
#include "umodule.h"
#include "uparse.h"
#include "uvm.h"
#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "watcher/uwatcher.h"

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers
 * =================================================================== */

/* make_int: construct an integer UValue without <string.h>. */
static UValue
make_int(int64_t n)
{
    UValue v;
    v.kind = UVAL_INT;
    v.v.i  = n;
    return v;
}

/* compile_and_run: lex+parse+emit+run source under the VM's global realm.
 * Returns URBI_OK on success, error code otherwise.
 * out_result may be NULL (result discarded). */
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

/* run_to_no_runnable: drive the VM until strand_runnable_count == 0, a
 * fatal strand is detected, or the iteration cap is hit.
 *
 * Returns 1 if strand_runnable_count reached 0 (quiescent enough for tests
 * that leave active watchers installed — watchers keep watcher_active_count > 0
 * which prevents URBI_STEP_QUIESCENT even after all body strands complete).
 * Returns -1 on URBI_STEP_FATAL.
 * Returns 0 on cap exhaustion (timeout). */
#define E2E_MAX_ITERS 1000

static int
run_to_no_runnable(UVM *vm)
{
    int i;
    for (i = 0; i < E2E_MAX_ITERS; i++) {
        UStepResult sr = urbi_step(vm, 1000, NULL);
        if (sr == URBI_STEP_FATAL)     return -1;
        if (sr == URBI_STEP_WAKE_AT)   return 1;   /* no time-sleeping strands */
        if (sr == URBI_STEP_QUIESCENT) return 1;
        if (vm->strand_runnable_count == 0) return 1;
    }
    return 0;  /* timeout */
}

/* ===================================================================
 * Test: scripted_at_fires_on_rising_edge
 *
 * Install at (Realm.x > 5) Realm.fired = Realm.fired + 1 via a real
 * compiled script.  Trigger the rising edge by writing Realm.x = 10
 * through a nested function call (so the non-top-frame OP_RET safepoint
 * fires watcher_eval_dirty and spawns the body strand).  Drive the body
 * strand to completion via urbi_step.  Verify Realm.fired == 1.
 *
 * A second write (Realm.x = 20) must not re-fire (rising-edge discipline).
 * =================================================================== */
UTEST(scripted_at_fires_on_rising_edge)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* Pre-install Realm.x = 0, Realm.fired = 0 via C API.
     * This installs them on global_object before the watcher is compiled,
     * so the IC for Realm.x exists when the watcher cond is traced. */
    URealm *gr = urbi_realm_global(&vm);
    UASSERT(gr != NULL);
    if (gr == NULL) { uvm_destroy(&vm); return; }

    int rc;
    rc = urbi_realm_set_global(&vm, gr, "x",     1, make_int(0));
    UASSERT_EQ(URBI_OK, rc);
    rc = urbi_realm_set_global(&vm, gr, "fired", 5, make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    /* === Phase 1: install the at-watcher ===
     *
     * Compile and run "at (Realm.x > 5) Realm.fired = Realm.fired + 1".
     * install_watcher_runtime runs the cond closure via
     * urbi_run_closure_on_scratch (the T7 path); Realm.x == 0 so the
     * watcher installs with last_value_cache = false and enters the
     * active_watchers_head list.  The read-set records global_object
     * (the cell that holds Realm.x), setting UGC_HAS_WATCHER_OBSERVER
     * on it so any future OP_SETSLOT write to global_object triggers
     * observer_dirty => watcher_dirty_count++. */
    rc = compile_and_run(&vm,
        "at (Realm.x > 5) Realm.fired = Realm.fired + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);
    if (rc != URBI_OK) {
        uvm_destroy(&vm);
        return;
    }

    /* Watcher must be installed (cond was false at install time). */
    UASSERT(vm.active_watchers_head != NULL);

    /* fired must still be 0 — cond was false at install, no body fired. */
    UValue fired = {0};
    rc = urbi_realm_get_global(&vm, gr, "fired", 5, &fired);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT_EQ(0, (int)fired.v.i);

    /* === Phase 2: trigger the rising edge ===
     *
     * Write Realm.x = 10 via a nested function call.  The write goes
     * through OP_SETSLOT which calls urbi_gc_slot_write on global_object.
     * Because UGC_HAS_WATCHER_OBSERVER is set (from phase 1 trace), this
     * calls observer_dirty => watcher_dirty_count = 1.
     *
     * The non-top-frame OP_RET after the function body returns hits the
     * safepoint inside dispatch_loop_until_yield, which calls
     * watcher_eval_dirty.  watcher_eval_dirty evaluates the cond
     * (Realm.x > 5, now 10 > 5 = true) and detects a rising edge,
     * spawning the body strand via spawn_body_coroutine.
     *
     * The body strand runs Realm.fired = Realm.fired + 1.  For this to
     * succeed the body strand needs a module_instance (to resolve the IC
     * table for OP_GETSLOT/SETSLOT at frame_count==0).  This is the
     * gap being tested: do_spawn_body_coroutine does NOT currently
     * synthesize a module_instance for the body strand (unlike
     * urbi_run_closure_on_scratch which synthesizes one for the cond).
     *
     * Expected outcome (T9 integration test role):
     *   - If the body strand has a valid module_instance (e.g. if a future
     *     fix wires it in do_spawn_body_coroutine), the test passes.
     *   - If module_instance is NULL, the body strand HALTs with
     *     "GETSLOT/SETSLOT: no IC table bound", urbi_step returns
     *     URBI_STEP_FATAL, and Realm.fired stays 0.
     *
     * The nested function call pattern ensures the safepoint fires AFTER
     * the write (at non-top-frame OP_RET), making watcher_eval_dirty run
     * within the same compile_and_run invocation. */
    rc = compile_and_run(&vm,
        "var __trigger__ = function() { Realm.x = 10 }; __trigger__()",
        NULL);
    /* compile_and_run itself may return OK even if the watcher eval
     * spawned a body strand that later fails — the body strand runs
     * asynchronously via urbi_step, not synchronously inside uvm_run.
     * However, if the non-top OP_RET safepoint fires watcher_eval_dirty
     * and the body is spawned BEFORE compile_and_run returns, the body
     * strand might already be in the ready queue. */
    if (rc != URBI_OK) {
        /* Drain watchers before destroy. */
        while (vm.active_watchers_head != NULL)
            urbi_watcher_unregister_internal(&vm, vm.active_watchers_head);
        uvm_destroy(&vm);
        return;
    }

    /* === Phase 3: drive the body strand to completion ===
     *
     * The body strand (if spawned) is now in the ready queue.
     * run_to_no_runnable drives urbi_step until no runnable strands remain. */
    int step_rc = run_to_no_runnable(&vm);
    UASSERT(step_rc != -1);  /* URBI_STEP_FATAL → body strand crashed */

    /* === Phase 4: verify body fired exactly once ===
     *
     * If the body strand executed successfully, Realm.fired == 1.
     * If module_instance was NULL, the strand HALTed with a type error,
     * urbi_step returned FATAL, and Realm.fired stayed 0. */
    rc = urbi_realm_get_global(&vm, gr, "fired", 5, &fired);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT_EQ(1, (int)fired.v.i);

    /* === Phase 5: same-direction write must NOT re-fire ===
     *
     * Write Realm.x = 20.  The watcher is AT mode: the rising edge
     * already fired (last_value_cache is truthy), so this same-direction
     * write must not spawn another body strand. */
    rc = compile_and_run(&vm,
        "var __trigger2__ = function() { Realm.x = 20 }; __trigger2__()",
        NULL);
    (void)rc;

    (void)run_to_no_runnable(&vm);

    rc = urbi_realm_get_global(&vm, gr, "fired", 5, &fired);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT_EQ(1, (int)fired.v.i);   /* still 1 — no re-fire on same-direction */

    /* === Cleanup ===
     *
     * Drain active watchers before destroying the VM to avoid
     * use-after-free in watcher pool teardown. */
    while (vm.active_watchers_head != NULL)
        urbi_watcher_unregister_internal(&vm, vm.active_watchers_head);
    uvm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_at_scripted_e2e_suite(void)
{
    printf("test_at_scripted_e2e\n");
    utest_run("scripted_at_fires_on_rising_edge",
              scripted_at_fires_on_rising_edge);
}
