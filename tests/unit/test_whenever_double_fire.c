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
 * === Finding 2: urbi_vm_watcher_eval_dirty requires safepoint visits ============
 *
 * `urbi_vm_watcher_eval_dirty` is only invoked from the `safepoint:` label in
 * `urbi_vm_dispatch_loop_until_yield` (uvm.c around line 1933).  The label is
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
 * `observer_dirty` and bump `watcher_dirty_count`, but `urbi_vm_watcher_eval_dirty`
 * is never called — accumulated dirty marks aren't drained until SOME
 * other strand hits a safepoint.
 *
 * On hardware (eye_demo) this works because the scan_tick at-handler
 * invokes `Realm.scanner.scan()` — that OP_CALL hits safepoint and
 * drains the shared dirty count, firing the whenever.  In isolation
 * (single watcher, single trivial at-handler), the eval never runs.
 *
 * Attempted fix at S46 (post-dispatch drain in urbi_step — drain on EVERY step
 * with LEVEL whenever semantics) caused unbounded re-eval of level-triggered
 * watchers whose bodies re-dirty their own observed object — the storm.
 * Reverted.
 *
 * SCHED-02 fix (v0.13.3-scheduler-liveness, Task 5):
 *   (a) urbi_step gains an idle pre-loop drain (when strand_runnable_count == 0)
 *       and a post-loop Step-4b drain, so dirty marks left by flat at-handler
 *       bodies are drained, waking parked waituntil/at and firing whenevers.
 *   (b) Those two drains use EDGE-gated whenever firing (uwatcher_eval.c,
 *       whenever_edge_only): a whenever fires only on its cond's rising edge,
 *       so a body that re-dirties its own observed object (cell-agnostic
 *       observer_dirty) cannot self-feed an unbounded re-fire — this is what
 *       bounds the storm and lets the VM quiesce.  The ACTIVE-dispatch drains
 *       (dispatcher safepoint / post-native / operator-fallback) keep LEVEL
 *       firing, so whenever_level.chk's 3 active-dispatch fires are unchanged.
 *
 * For scripts that want a single-fire rising-edge semantic outside an active
 * dispatch loop, `at` and the idle whenever path now coincide; a whenever that
 * should fire repeatedly while true needs active work (a periodic / event loop)
 * to supply the safepoints that drive its LEVEL re-fires.
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

/* === Test 2: at-handler body without nested call wakes a whenever via the
 *             idle/boundary reactive drain — BOUNDED, and the VM QUIESCES.
 *
 * Background.  An at-handler body that increments a Realm slot is a flat
 * statement sequence (no OP_CALL, no backward OP_JMP); it exits via top-frame
 * OP_RET, bypassing the dispatcher `safepoint:` label, so its observer_dirty
 * marks were never drained during active dispatch.  The SCHED-02 idle/boundary
 * drain (ustep.c pre-loop + post-loop Step 4b) now drains them, so the whenever
 * subscribed to Realm.x fires.
 *
 * The hazard this test pins.  observer_dirty (uwatcher.c) is cell-agnostic: the
 * whenever's body writes Realm.fired, which re-dirties the SAME Realm object
 * whose cell carries the OBSERVER bit (the cond reads Realm.x).  A level-trigger
 * idle drain would re-fire the still-truthy whenever on every step forever — the
 * reverted-S46 storm (an adversarial -O0 rebuild of the first cut measured
 * FIRED=998 with the VM stuck RUNNING).  The fix gates the idle/boundary drain
 * to the rising edge (uwatcher_eval.c, whenever_edge_only), so the whenever
 * fires at most once per false->true transition of its cond and the VM quiesces.
 *
 * Contract asserted here:
 *   1. Every tick's drain RETURNS a settled verdict (QUIESCENT / WAKE_AT), never
 *      a capped RUNNING — i.e. no spin (termination).
 *   2. `fired` is BOUNDED and small: the cond `Realm.x > 3` has exactly one
 *      false->true edge across the 5 ticks (x crosses 3 at tick 3), so the
 *      edge-gated idle drain fires once.  We assert 1 <= fired <= 2: the upper
 *      bound is 2 rather than 1 as a single-fire margin — the rising edge can be
 *      observed by either the pre-loop idle drain or the post-loop Step-4b drain
 *      of the crossing step, so at most one extra rising-edge evaluation is
 *      tolerated against drain-ordering nuance.  A storm (hundreds of fires)
 *      blows through it; the empirical value with the current scheduler is 1.
 *   3. dirty_count is fully drained to 0 (the VM is genuinely idle). */
UTEST(at_handler_body_without_call_wakes_whenever_bounded)
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
        /* Termination: the VM SETTLES every tick — a self-re-dirtying level
         * whenever would leave drain_to_quiescent returning RUNNING (the 500-
         * iteration cap), which is the storm signature.  Must be QUIESCENT or
         * WAKE_AT, never RUNNING. */
        UASSERT(step == URBI_STEP_QUIESCENT || step == URBI_STEP_WAKE_AT);
    }

    /* The at-handler DOES run — x reaches 5.  Write barriers fire. */
    UValue x = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "x", 1, &x));
    UASSERT_EQ((int)UVAL_INT, (int)x.kind);
    UASSERT_EQ(5LL, x.v.i);

    /* Genuinely idle: all dirty marks drained. */
    UASSERT_EQ(0U, vm.watchers->dirty_count);

    /* Bounded fire: cond has one false->true edge → edge-gated idle drain fires
     * once.  1 <= fired <= 2 (the 998-fire storm would fail this). */
    UValue fired = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "fired", 5, &fired));
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT(fired.v.i >= 1);
    UASSERT(fired.v.i <= 2);

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

/* === Test 6: SELF-RE-DIRTYING whenever terminates (storm regression) ====
 *
 * The worst case for the SCHED-02 idle/boundary drain: a whenever whose body
 * keeps its OWN condition true.  cond `Realm.go` is flipped true by a host
 * slot-write; the body re-affirms `Realm.go = 1` on every fire, so under the
 * old LEVEL idle drain the still-truthy whenever re-fired on every step and the
 * VM never quiesced (the 998-fire storm).
 *
 * With the edge-gated idle/boundary drain, the whenever fires once on the
 * false->true edge; thereafter `go` stays truthy, so there is no further rising
 * edge and the VM QUIESCES.  This test fails (hangs at the drain cap → RUNNING)
 * if the bound is ever removed. */
UTEST(self_redirtying_whenever_terminates)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UArena  arena;
    UProto module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var Realm.go = false;"
        "var Realm.fired = 0;"
        "whenever (Realm.go) { Realm.fired = Realm.fired + 1; Realm.go = 1 }",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Settle the install (cond false → no fire, VM idle). */
    UStepResult s0 = drain_to_quiescent(&vm);
    UASSERT(s0 == URBI_STEP_QUIESCENT || s0 == URBI_STEP_WAKE_AT);

    /* Host slot-write flips the cond true (the SCHED-02 external stimulus). */
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "go", 2,
                                              utest_e2e_make_int(1)));

    /* Drive to quiescence.  Termination: must SETTLE, not spin at the cap. */
    UStepResult s1 = drain_to_quiescent(&vm);
    UASSERT(s1 != URBI_STEP_FATAL);
    UASSERT(s1 == URBI_STEP_QUIESCENT || s1 == URBI_STEP_WAKE_AT);
    UASSERT_EQ(0U, vm.watchers->dirty_count);

    /* The body ran (cascade resolved): go is re-affirmed truthy, fired bumped. */
    UValue go = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "go", 2, &go));
    UASSERT_EQ((int)UVAL_INT, (int)go.kind);
    UASSERT_EQ(1LL, go.v.i);

    /* Bounded fire: the single host-driven false->true edge fires the whenever
     * once; the body re-affirms `go` truthy, so no further rising edge occurs.
     * Upper bound 2 (not 1) is the single-fire margin — the edge may be observed
     * by the pre-loop or the post-loop Step-4b drain — while a storm (hundreds of
     * fires) still fails it.  Empirical value with the current scheduler is 1. */
    UValue fired = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "fired", 5, &fired));
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT(fired.v.i >= 1);
    UASSERT(fired.v.i <= 2);

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

void
test_whenever_double_fire_suite(void)
{
    utest_run("whenever_double_fire: chunk-top write fires cond (baseline)",
              whenever_chunktop_write_fires_cond_baseline);
    utest_run("whenever_double_fire: at-body without call wakes whenever, bounded + quiesces (SCHED-02)",
              at_handler_body_without_call_wakes_whenever_bounded);
    utest_run("whenever_double_fire: at-body with call DOES drain dirty (workaround)",
              at_handler_body_with_call_drains_dirty);
    utest_run("whenever_double_fire: try/catch/finally with caught throw — finally runs exactly once (S5a)",
              try_catch_finally_runs_finally_on_caught_throw);
    utest_run("whenever_double_fire: nested try/finally in try/catch — finally DOES run",
              nested_try_finally_in_try_catch_runs_finally);
    utest_run("whenever_double_fire: self-re-dirtying whenever terminates (storm regression)",
              self_redirtying_whenever_terminates);
}
