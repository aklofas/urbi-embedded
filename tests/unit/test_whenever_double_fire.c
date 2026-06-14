/* SPDX-License-Identifier: BSD-3-Clause */
/* test_whenever_double_fire — documents watcher eval-pass semantics
 * surfaced by an eye_demo investigation 2026-05-16.
 *
 * Original observation: eye_demo's `whenever (Realm.blob_count > Realm.last_milestone + 500) ...`
 * fires its body TWICE per threshold cross.  Initially filed as a runtime
 * bug.  Host-side investigation produced TWO findings:
 *
 * === Finding 1: whenever's level-trigger semantic is per-spec ===========
 *
 * `whenever (cond) body` fires on every dirty pass where cond is truthy,
 * NOT just on the rising edge.  This matches `tests/chk/reactive/at/whenever_level.chk`
 * which asserts fired=3 after 2 mutations.  The eye_demo's double-fire is
 * the level-trigger working correctly (two safepoint visits both observe
 * cond truthy before the body's slot-write propagates).
 *
 * The correct idiom for "fire once per rising edge" is `at`, not
 * `whenever`.  Use `at` when you want a single fire per false→true
 * transition of cond — this is what the eye_demo milestone tracker
 * actually wanted.
 *
 * === Finding 2: watcher_eval_dirty requires safepoint visits ============
 *
 * `watcher_eval_dirty` is only invoked from the `safepoint:` label in
 * `dispatch_loop_until_yield` (uvm.c around line 1933).  The label is
 * reached via explicit `goto safepoint` from a small set of opcodes:
 *
 *   - OP_CALL (every function call)
 *   - OP_JMP backward (loop iterations)
 *   - OP_RET non-top-frame (returning from a nested call)
 *   - OP_THROW
 *   - OP_CALL native that raised unwind
 *
 * Top-frame OP_RET goes directly to `exit_strand:`, skipping safepoint.
 * Body strands spawned by event-triggered at-handlers whose bodies are
 * flat statement sequences (no nested call, no loop) never hit a
 * safepoint mid-execution.  Their writes to subscribed Realm slots fire
 * `observer_dirty` and bump `watcher_dirty_count`, but `watcher_eval_dirty`
 * is never called — accumulated dirty marks aren't drained until SOME
 * other strand hits a safepoint.
 *
 * On hardware (eye_demo) this works because the scan_tick at-handler
 * invokes `Realm.scanner.scan()` — that OP_CALL hits safepoint and
 * drains the shared dirty count, firing the whenever.  In isolation
 * (single watcher, single trivial at-handler), the eval never runs.
 *
 * Attempted fix at S46 (post-dispatch drain in urbi_step — drain on EVERY step)
 * caused unbounded re-eval of level-triggered watchers whose bodies write
 * subscribed slots — broke whenever_level.chk.  Reverted.
 *
 * SCHED-02 fix (v0.13.3-scheduler-liveness, Task 5): a targeted idle-VM
 * pre-loop drain fires in urbi_step when strand_runnable_count == 0 (VM
 * idle between host steps).  This drains dirty marks from at-handler bodies
 * that exited without a safepoint, waking parked waituntil/whenever strands.
 * The guard prevents re-firing during active strand execution (safepoints
 * continue to handle that path), keeping whenever_level.chk correct (3 fires).
 *
 * Workaround for scripts that need a single-fire rising-edge semantic:
 * use `at` instead of `whenever`.  Hardware code naturally hits safepoints
 * through method calls; pure event-loop scripts can use `at` for edge-trigger.
 *
 * Documentation candidate: `docs/internals/reactive-runtime.md` should
 * call out this contract explicitly.  Filed as design-risk
 * "watcher eval requires safepoint visits" with workaround.
 *
 * Filed as a documentation/usage clarification, NOT a runtime bug. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "chunk/uchunk.h"
#include "value/uarena.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

static UStepResult
drain_to_quiescent(UVM *vm)
{
    UStepResult r = URBI_STEP_QUIESCENT;
    int i;
    for (i = 0; i < 500; i++) {
        r = urbi_step(vm, 500, NULL);
        if (r == URBI_STEP_QUIESCENT || r == URBI_STEP_WAKE_AT ||
            r == URBI_STEP_FATAL) {
            return r;
        }
    }
    return r;
}

/* === Test 1: chunk-top write fires cond watcher (baseline) =============
 * Per existing whenever_level.chk semantics.  Confirms the subscription
 * and dirty-trigger mechanism works on the happy path: chunk-top synchronous
 * function call writes a subscribed slot, the whenever cond fires. */
UTEST(whenever_chunktop_write_fires_cond_baseline)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UArena  arena;
    UProto module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var x = 0;"
        "var fired = 0;"
        "whenever (Realm.x > 3) Realm.fired = Realm.fired + 1;"
        "var t = function () { Realm.x = 5 }; t()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    UStepResult step = drain_to_quiescent(&vm);
    UASSERT(step != URBI_STEP_FATAL);

    UValue fired = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "fired", 5, &fired));
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT(fired.v.i >= 1);

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

/* === Test 2: at-handler body without nested call NOW wakes watcher
 *             via the idle-VM pre-loop reactive drain (SCHED-02 fix).
 *
 * Previously this test documented the safepoint-visit LIMITATION: an at-handler
 * body that increments a Realm slot could not trigger a `whenever` watcher
 * subscribed to that slot, because the body is a flat sequence (no OP_CALL,
 * no backward OP_JMP), exits via top-frame OP_RET (bypasses the safepoint label
 * in dispatch_loop_until_yield), and dirty_count therefore never reached
 * watcher_eval_dirty.  The fix was filed as a future item.
 *
 * SCHED-02 fix (v0.13.3-scheduler-liveness, Task 5): vm_reactive_drain is now
 * called in urbi_step's pre-loop pump when strand_runnable_count == 0.  After
 * the at-handler body exits (dead strand, no remaining runnable strands), the
 * next urbi_step call sees strand_runnable_count == 0 AND dirty_count > 0 and
 * drains: watcher_eval_dirty runs, the whenever fires, the body spawns.
 *
 * Note: the whenever body itself writes Realm.fired which re-dirties Realm,
 * so watcher_eval_dirty fires on every subsequent idle step (level-triggered
 * per spec §6.5).  fired_count >= 1 is the correct post-fix contract;
 * a specific count is not asserted because it depends on the number of
 * drain_to_quiescent iterations.  Use `at` instead of `whenever` for a
 * single-fire rising-edge semantic. */
UTEST(at_handler_body_without_call_does_not_drain_dirty)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t tick = urbi_event_register(&vm, r, "tick", NULL, NULL);
    UASSERT(tick != URBI_EVENT_ID_INVALID);

    UArena  arena;
    UProto module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var x = 0;"
        "var fired = 0;"
        "whenever (Realm.x > 3) Realm.fired = Realm.fired + 1;"
        "at (tick?) Realm.x = Realm.x + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    int i;
    for (i = 0; i < 5; i++) {
        urbi_inject_event(&vm, (uint32_t)tick, NULL, 0U);
        UStepResult step = drain_to_quiescent(&vm);
        UASSERT(step != URBI_STEP_FATAL);
    }

    /* The at-handler DOES run — x reaches 5.  Write barriers fire. */
    UValue x = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "x", 1, &x));
    UASSERT_EQ((int)UVAL_INT, (int)x.kind);
    UASSERT_EQ(5LL, x.v.i);

    /* dirty_count is 0: the idle-VM pre-loop drain (SCHED-02 fix) drains dirty
     * marks between urbi_step calls.  The previous assertion
     * "dirty_count >= 5" documented the pre-fix limitation and is removed. */

    /* The watcher fires: the idle-VM drain wakes the whenever after each tick's
     * at-handler body exits.  fired >= 1 is the post-fix contract; a specific
     * count is not pinned (level-trigger + re-dirty from the body write causes
     * multiple fires per drain_to_quiescent iteration). */
    UValue fired = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "fired", 5, &fired));
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT(fired.v.i >= 1);

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

/* === Test 3: at-handler body WITH a nested call DOES drain dirty.
 *
 * The workaround pattern: add a no-op method call to the at-body so its
 * OP_CALL hits safepoint, draining the accumulated dirty count from any
 * preceding slot writes in the body.  After 5 ticks crossing the
 * threshold, the watcher fires (level-trigger semantic: fires multiple
 * times due to body re-dirtying the cond, but the existence of at
 * least one fire confirms the workaround works). */
UTEST(at_handler_body_with_call_drains_dirty)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t tick = urbi_event_register(&vm, r, "tick", NULL, NULL);
    UASSERT(tick != URBI_EVENT_ID_INVALID);

    UArena  arena;
    UProto module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        /* Body invokes a method (OP_CALL → safepoint).  The method does
         * the increment.  The OP_CALL safepoint hits BEFORE entering bump,
         * but the watcher's read-set includes Realm.x — the body's write
         * inside bump fires observer_dirty, and the call from bump back
         * (OP_RET non-top → safepoint) drains it. */
        "class Helper { var bump = function () { Realm.x = Realm.x + 1 } };"
        "Realm.helper = Helper.new();"
        "var x = 0;"
        "var fired = 0;"
        "whenever (Realm.x > 3) Realm.fired = Realm.fired + 1;"
        "at (tick?) Realm.helper.bump()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    int i;
    for (i = 0; i < 5; i++) {
        urbi_inject_event(&vm, (uint32_t)tick, NULL, 0U);
        UStepResult step = drain_to_quiescent(&vm);
        UASSERT(step != URBI_STEP_FATAL);
    }

    UValue x = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "x", 1, &x));
    UASSERT_EQ((int)UVAL_INT, (int)x.kind);
    UASSERT_EQ(5LL, x.v.i);

    UValue fired = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "fired", 5, &fired));
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT(fired.v.i >= 1);

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

/* === Test 4: try/catch/finally runs finally on a caught throw ===========
 *
 * Originally discovered 2026-05-16 on eye_demo as a footgun: `try { throw }
 * catch (e) {} finally {}` did NOT run the finally arm when the catch absorbed
 * the throw, because emit_try_frame reached the outer FLAG_HAS_FINALLY frame
 * via normal flow (post-catch), popping TRY_END without running finally.
 *
 * Fixed at v1.0 / M10 (design-risks v0.11.4-D): finally runs on EVERY exit
 * kind per REVIVAL §S5a — the normal-completion and post-catch paths now
 * emit an inline copy of the finally body (uemit_unwind.c, emit_finally_inline).
 * So a caught throw runs catch once AND finally once. */
UTEST(try_catch_finally_runs_finally_on_caught_throw)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "caught_n", 8,
                                               utest_e2e_make_int(0)));
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "finally_n", 9,
                                               utest_e2e_make_int(0)));

    UArena  arena;
    UProto module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "try {"
        "  throw \"x\""
        "} catch (e) {"
        "  Realm.caught_n = Realm.caught_n + 1"
        "} finally {"
        "  Realm.finally_n = Realm.finally_n + 1"
        "}",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    UStepResult step = drain_to_quiescent(&vm);
    UASSERT(step != URBI_STEP_FATAL);

    UValue cn = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "caught_n", 8, &cn));
    UASSERT_EQ((int)UVAL_INT, (int)cn.kind);
    UASSERT_EQ(1LL, cn.v.i);   /* catch ran exactly once */

    UValue fn = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "finally_n", 9, &fn));
    UASSERT_EQ((int)UVAL_INT, (int)fn.kind);
    UASSERT_EQ(1LL, fn.v.i);   /* finally runs exactly once, even on a caught throw (S5a) */

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

/* === Test 5: nested try/finally + outer try/catch DOES run finally ====
 *
 * The working pattern from nested_finally.chk: place finally INSIDE the
 * try block (separate try-finally), then wrap with outer try-catch.
 * The inner finally runs during unwind (before outer catch absorbs);
 * outer catch absorbs the throw. */
UTEST(nested_try_finally_in_try_catch_runs_finally)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "caught_n", 8,
                                               utest_e2e_make_int(0)));
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "finally_n", 9,
                                               utest_e2e_make_int(0)));

    UArena  arena;
    UProto module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "try {"
        "  try {"
        "    throw \"x\""
        "  } finally {"
        "    Realm.finally_n = Realm.finally_n + 1"
        "  }"
        "} catch (e) {"
        "  Realm.caught_n = Realm.caught_n + 1"
        "}",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    UStepResult step = drain_to_quiescent(&vm);
    UASSERT(step != URBI_STEP_FATAL);

    UValue fn = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "finally_n", 9, &fn));
    UASSERT_EQ((int)UVAL_INT, (int)fn.kind);
    UASSERT_EQ(1LL, fn.v.i);   /* inner finally ran during unwind */

    UValue cn = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "caught_n", 8, &cn));
    UASSERT_EQ((int)UVAL_INT, (int)cn.kind);
    UASSERT_EQ(1LL, cn.v.i);   /* outer catch absorbed the throw */

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

void
test_whenever_double_fire_suite(void)
{
    utest_run("whenever_double_fire: chunk-top write fires cond (baseline)",
              whenever_chunktop_write_fires_cond_baseline);
    utest_run("whenever_double_fire: at-body without call wakes watcher via idle-VM drain (SCHED-02)",
              at_handler_body_without_call_does_not_drain_dirty);
    utest_run("whenever_double_fire: at-body with call DOES drain dirty (workaround)",
              at_handler_body_with_call_drains_dirty);
    utest_run("whenever_double_fire: try/catch/finally with caught throw — finally runs exactly once (S5a)",
              try_catch_finally_runs_finally_on_caught_throw);
    utest_run("whenever_double_fire: nested try/finally in try/catch — finally DOES run",
              nested_try_finally_in_try_catch_runs_finally);
}
