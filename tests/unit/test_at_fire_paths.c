/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: urbi_vm_watcher_eval_dirty fire-decision paths (T43).
 * Spec #2 §8.3–§8.5.
 *
 * All tests use the hook-based approach (test_watcher_condition_hook,
 * test_watcher_fire_hook, test_watcher_onleave_hook) because the v1.0
 * no-globals constraint blocks runtime variable mutation from outside
 * the bytecode path at this milestone.  This exercises the fire-decision
 * logic independently of the bytecode/scratch-frame execution path.
 *
 * Cases:
 *   1. at_rising_edge_fires_body:
 *      AT mode, condition flips false → true → fire once; stays true → no
 *      second fire; flips back to false → no body fire (just falling).
 *
 *   2. at_with_onleave_fires_on_falling_edge:
 *      AT mode with onleave.  Rising → body fires, BODY_FIRED_SINCE_ONLEAVE
 *      set.  Falling → onleave fires; flag cleared.  No body on falling.
 *
 *   3. whenever_fires_every_pass_while_truthy:
 *      WHENEVER mode.  Two dirty passes with cond true → two fires.
 *      After flipping cond false → no fire on third pass.
 *
 *   4. at_sync_runs_inline:
 *      AT_SYNC mode, rising edge → inline body fires (via fire hook).
 *      No spawn_body_coroutine called (no body strand created).
 *
 *   5. waituntil_rising_edge_wakes_waiter:
 *      WAITUNTIL mode.  Install with falsy seed; wire waiter_strand in WAIT
 *      state; rising edge → waiter_strand transitions to READY, watcher
 *      unregistered.
 *
 *   6. at_no_onleave_falling_edge_no_crash:
 *      AT mode without onleave.  Falling edge must not fire anything and
 *      must not crash (BODY_FIRED_SINCE_ONLEAVE guard prevents call). */

#include "utest.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"        /* USTRAND_WAIT_WATCHER, USTRAND_STATE_READY */
#include "watcher/uwatcher.h"
#include "twatcher_install_helper.h"
#include "sched/usched_cooperative.h"  /* sched_strand_block */
#include "urbi/urbi.h"

#include <stdlib.h>
#include <stddef.h>

#define UTEST(name) static void name(void)

/* === Dummy GC-managed closure helpers (T17 sentinel conversion) ===
 *
 * Replaces (UClosure *)1/2/3 integer sentinels with real GC-managed closures.
 * The GC walker (mark_root_callback) would crash on a bare integer cast to
 * UClosure* if a GC cycle ran while a sentinel was stored in w->condition or
 * w->onleave.  Real native closures are GC-safe; their UCell header has
 * valid color/type fields. */
static int
dummy_native_fn(struct UVM *vm, UValue self, UValue *args,
                uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    *out = urbi_make_nil();
    return 0;
}

static UClosure *
make_dummy_closure(UVM *vm)
{
    return urbi_make_native_closure(vm, dummy_native_fn);
}

/* ===================================================================
 * Shared hook state
 * =================================================================== */

static int g_fire_count;
static int g_onleave_count;

/* Togglable condition shared across tests. */
static int g_cond_truthy;

/* ===================================================================
 * Condition hooks
 * =================================================================== */

static UValue
hook_cond_toggle(struct UVM *vm, struct UWatcher *w)
{
    UValue v = {0};
    (void)vm; (void)w;
    if (g_cond_truthy) {
        v.kind = (uint8_t)UVAL_BOOL;
        v.v.i  = 1;
    }
    return v;
}

/* ===================================================================
 * Body / onleave fire hooks
 * =================================================================== */

static void
hook_fire_count(struct UVM *vm, struct UWatcher *w)
{
    (void)vm; (void)w;
    g_fire_count++;
}

static void
hook_onleave_count(struct UVM *vm, struct UWatcher *w)
{
    (void)vm; (void)w;
    g_onleave_count++;
}

/* ===================================================================
 * Helper: run one dirty-eval pass
 * =================================================================== */

static void
run_one_dirty_pass(struct UVM *vm)
{
    vm->watchers->dirty_count = 1U;
    urbi_vm_watcher_eval_dirty(vm);
}

/* ===================================================================
 * Test 1: at_rising_edge_fires_body
 * =================================================================== */

UTEST(at_rising_edge_fires_body)
{
    UVM      vm;
    UWatcher *w;

    urbi_vm_init(&vm, NULL, NULL);

    g_fire_count   = 0;
    g_cond_truthy  = 0;   /* start false — seed will be NIL */

    /* Install with condition hook off so seed is NIL (falsy). */
    w = urbi_watcher_install_for_test(
        &vm, UWATCHER_AT, NULL, /*condition=*/make_dummy_closure(&vm),
        NULL, NULL, NULL, 0U);
    UASSERT(w != NULL);
    UASSERT_EQ((int)w->last_value_cache.kind, (int)UVAL_NIL);

    vm.test_hooks->watcher_condition = hook_cond_toggle;
    vm.test_hooks->watcher_fire      = hook_fire_count;

    /* Pass 1: cond still false → no fire (NIL→NIL, no rising edge). */
    run_one_dirty_pass(&vm);
    UASSERT_EQ(g_fire_count, 0);

    /* Pass 2: flip to true → rising edge fires once. */
    g_cond_truthy = 1;
    run_one_dirty_pass(&vm);
    UASSERT_EQ(g_fire_count, 1);

    /* Pass 3: still true → no rising edge, no second fire. */
    run_one_dirty_pass(&vm);
    UASSERT_EQ(g_fire_count, 1);

    /* Pass 4: flip back to false → falling edge, no body fire. */
    g_cond_truthy = 0;
    run_one_dirty_pass(&vm);
    UASSERT_EQ(g_fire_count, 1);   /* body count unchanged */

    vm.test_hooks->watcher_condition = NULL;
    vm.test_hooks->watcher_fire      = NULL;
    urbi_watcher_unregister_internal(&vm, w);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 2: at_with_onleave_fires_on_falling_edge
 * =================================================================== */

UTEST(at_with_onleave_fires_on_falling_edge)
{
    UVM      vm;
    UWatcher *w;

    urbi_vm_init(&vm, NULL, NULL);

    g_fire_count   = 0;
    g_onleave_count = 0;
    g_cond_truthy  = 0;   /* seed false */

    /* body=NULL so fire is observed via test_watcher_fire_hook (no realm needed). */
    w = urbi_watcher_install_for_test(
        &vm, UWATCHER_AT, NULL, /*condition=*/make_dummy_closure(&vm),
        NULL,
        /*onleave=*/make_dummy_closure(&vm),
        NULL, 0U);
    UASSERT(w != NULL);

    vm.test_hooks->watcher_condition = hook_cond_toggle;
    vm.test_hooks->watcher_fire      = hook_fire_count;
    vm.test_hooks->watcher_onleave   = hook_onleave_count;

    /* Rising edge: body fires, BODY_FIRED_SINCE_ONLEAVE set. */
    g_cond_truthy = 1;
    run_one_dirty_pass(&vm);
    UASSERT_EQ(g_fire_count,   1);
    UASSERT_EQ(g_onleave_count, 0);
    UASSERT(w->flags & URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE);

    /* Falling edge: onleave fires; BODY_FIRED_SINCE_ONLEAVE cleared. */
    g_cond_truthy = 0;
    run_one_dirty_pass(&vm);
    UASSERT_EQ(g_fire_count,   1);   /* body count unchanged */
    UASSERT_EQ(g_onleave_count, 1);
    UASSERT(!(w->flags & URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE));

    /* Second falling edge (cond stays false): flag already clear → no onleave. */
    run_one_dirty_pass(&vm);
    UASSERT_EQ(g_onleave_count, 1);

    vm.test_hooks->watcher_condition = NULL;
    vm.test_hooks->watcher_fire      = NULL;
    vm.test_hooks->watcher_onleave   = NULL;
    urbi_watcher_unregister_internal(&vm, w);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 3: whenever_fires_every_pass_while_truthy
 * =================================================================== */

UTEST(whenever_fires_every_pass_while_truthy)
{
    UVM      vm;
    UWatcher *w;

    urbi_vm_init(&vm, NULL, NULL);

    g_fire_count  = 0;
    g_cond_truthy = 1;   /* start true for WHENEVER seed too */

    /* Install with hook already active so seed is truthy.
     * WHENEVER fires every pass regardless of edge — seed value is irrelevant
     * for the fire decision but we set it consistently. */
    vm.test_hooks->watcher_condition = hook_cond_toggle;
    vm.test_hooks->watcher_fire      = hook_fire_count;

    w = urbi_watcher_install_for_test(
        &vm, UWATCHER_WHENEVER, NULL, make_dummy_closure(&vm),
        NULL, NULL, NULL, 0U);
    UASSERT(w != NULL);

    /* Two passes with cond true → two fires. */
    run_one_dirty_pass(&vm);
    run_one_dirty_pass(&vm);
    UASSERT_EQ(g_fire_count, 2);

    /* Flip cond false → no fire on third pass. */
    g_cond_truthy = 0;
    run_one_dirty_pass(&vm);
    UASSERT_EQ(g_fire_count, 2);

    vm.test_hooks->watcher_condition = NULL;
    vm.test_hooks->watcher_fire      = NULL;
    urbi_watcher_unregister_internal(&vm, w);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 4: at_sync_runs_inline
 * =================================================================== */

UTEST(at_sync_runs_inline)
{
    UVM      vm;
    UWatcher *w;

    urbi_vm_init(&vm, NULL, NULL);

    g_fire_count  = 0;
    g_cond_truthy = 0;   /* seed false */

    /* body=make_dummy_closure to exercise invoke_body_inline path.
     * AT_SYNC uses invoke_body_inline (no spawn, no realm needed).
     * The fire hook (hook_fire_count) intercepts before urbi_run_closure_on_scratch
     * so the native body closure is never dispatched via bytecode. */
    w = urbi_watcher_install_for_test(
        &vm, UWATCHER_AT_SYNC, NULL, make_dummy_closure(&vm),
        /*body=*/make_dummy_closure(&vm),
        NULL, NULL, 0U);
    UASSERT(w != NULL);

    vm.test_hooks->watcher_condition = hook_cond_toggle;
    vm.test_hooks->watcher_fire      = hook_fire_count;

    /* Rising edge: inline body fires (via fire hook). */
    g_cond_truthy = 1;
    run_one_dirty_pass(&vm);
    UASSERT_EQ(g_fire_count, 1);

    /* Still true → no second inline fire (rising-edge, not level). */
    run_one_dirty_pass(&vm);
    UASSERT_EQ(g_fire_count, 1);

    /* No body strand created (AT_SYNC runs inline — no spawn). */
    UASSERT(w->body_strand == NULL);

    vm.test_hooks->watcher_condition = NULL;
    vm.test_hooks->watcher_fire      = NULL;
    urbi_watcher_unregister_internal(&vm, w);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 5: waituntil_rising_edge_wakes_waiter
 * =================================================================== */

UTEST(waituntil_rising_edge_wakes_waiter)
{
    UVM     vm;
    UStrand waiter;

    urbi_vm_init(&vm, NULL, NULL);
    ustrand_init(&waiter, &vm);

    g_cond_truthy = 0;   /* seed false so watcher stays installed */

    /* Park the waiter in WAIT_WATCHER through the real transition (mirrors
     * install_watcher_runtime's sched_strand_block park).  v0.13.3
     * (SCHED-13): a raw state stamp would bypass the strand_waiting_count
     * increment, so the rising-edge wake below would trip the
     * no-saturation decrement in sched_strand_make_runnable. */
    waiter.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;   /* satisfy block's RUNNING-decrement */
    sched_strand_block(&waiter, USTRAND_REASON_WATCHER, 0);

    vm.test_hooks->watcher_condition = hook_cond_toggle;

    /* Install WAITUNTIL with waiter_strand wired. */
    UWatcher *w = urbi_watcher_install_for_test(
        &vm, UWATCHER_WAITUNTIL, NULL, make_dummy_closure(&vm),
        NULL, NULL, NULL, 0U);
    UASSERT(w != NULL);
    /* Wire waiter_strand manually (install_internal does not know about waiter). */
    w->waiter_strand = &waiter;

    /* Cond still false → no wake, watcher still installed. */
    run_one_dirty_pass(&vm);
    UASSERT(vm.active_watchers_head != NULL);
    UASSERT_EQ((int)waiter.state, (int)USTRAND_WAIT_WATCHER);

    /* Rising edge: waiter wakes (READY), watcher unregistered. */
    g_cond_truthy = 1;
    run_one_dirty_pass(&vm);
    UASSERT(vm.active_watchers_head == NULL);
    UASSERT_EQ((int)waiter.state, (int)USTRAND_STATE_READY);

    vm.test_hooks->watcher_condition = NULL;
    /* Dequeue waiter from ready queue before destroying vm. */
    if (vm.ready_head == &waiter) {
        vm.ready_head = waiter.ready_next;
        if (vm.ready_head != NULL)
            vm.ready_head->ready_prev = NULL;
        else
            vm.ready_tail = NULL;
        if (vm.strand_runnable_count > 0)
            vm.strand_runnable_count--;
        waiter.state = USTRAND_STATE_DORMANT;
    }
    ustrand_destroy(&waiter, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 6: at_no_onleave_falling_edge_no_crash
 * =================================================================== */

UTEST(at_no_onleave_falling_edge_no_crash)
{
    UVM      vm;
    UWatcher *w;

    urbi_vm_init(&vm, NULL, NULL);

    g_fire_count  = 0;
    g_cond_truthy = 0;

    w = urbi_watcher_install_for_test(
        &vm, UWATCHER_AT, NULL, make_dummy_closure(&vm),
        NULL, NULL, NULL, 0U);
    UASSERT(w != NULL);

    vm.test_hooks->watcher_condition = hook_cond_toggle;
    vm.test_hooks->watcher_fire      = hook_fire_count;

    /* Rising edge fires body. */
    g_cond_truthy = 1;
    run_one_dirty_pass(&vm);
    UASSERT_EQ(g_fire_count, 1);

    /* Falling edge: no onleave → nothing fires; no crash. */
    g_cond_truthy = 0;
    run_one_dirty_pass(&vm);
    UASSERT_EQ(g_fire_count, 1);  /* body count unchanged */

    /* BODY_FIRED_SINCE_ONLEAVE should be set (body fired once) but
     * the falling-edge onleave check short-circuits on onleave == NULL.
     * The flag state itself is an implementation detail; verify no crash only. */

    vm.test_hooks->watcher_condition = NULL;
    vm.test_hooks->watcher_fire      = NULL;
    urbi_watcher_unregister_internal(&vm, w);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_at_fire_paths_suite(void)
{
    printf("test_at_fire_paths\n");
    utest_run("at_rising_edge_fires_body",
              at_rising_edge_fires_body);
    utest_run("at_with_onleave_fires_on_falling_edge",
              at_with_onleave_fires_on_falling_edge);
    utest_run("whenever_fires_every_pass_while_truthy",
              whenever_fires_every_pass_while_truthy);
    utest_run("at_sync_runs_inline",
              at_sync_runs_inline);
    utest_run("waituntil_rising_edge_wakes_waiter",
              waituntil_rising_edge_wakes_waiter);
    utest_run("at_no_onleave_falling_edge_no_crash",
              at_no_onleave_falling_edge_no_crash);
}
