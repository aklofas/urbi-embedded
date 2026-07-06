/* SPDX-License-Identifier: BSD-3-Clause */
/* Regression tests for the vm_reactive_drain idle-pump (Task 5 of
 * v0.13.3-scheduler-liveness):
 *
 *   SCHED-02 / B10 — host_write_wakes_waituntil_with_zero_runnable:
 *     A host-C slot write between urbi_step calls must reach a strand parked
 *     in waituntil(cond).  Pre-fix: the pre-loop pump ran ISR-ring /
 *     periodics / sleepers but not slot-change-drain / watcher-eval, so the
 *     parked strand was never woken.  Post-fix: vm_reactive_drain fires at
 *     the top of each urbi_step call.
 *
 *   SCHED-12 — sched12_drain_preserves_in_eval_when_nested:
 *     drain_pending_onleave_queue previously used absolute set/clear for
 *     in_eval (set 1 at entry, 0 at exit).  The save/restore fix ensures
 *     the caller's in_eval value is preserved across the call.  A nested
 *     call from within urbi_vm_watcher_eval_dirty (in_eval = 1 on entry) would
 *     clobber the flag to 0, dropping the outer eval's reentrancy guard.
 *     This test pins the post-fix contract by calling drain with in_eval = 1
 *     and asserting it remains 1 on return.
 *
 *   Onleave-orphan — onleave_orphan_mid_drain_push_not_lost:
 *     pending_onleave_tail was not updated when the last queue entry was
 *     popped.  An onleave handler that pushed a new entry mid-drain would
 *     append to the stale tail pointer, orphaning the new entry (unreachable
 *     from head).  The fix updates tail inline before invoking each handler.
 */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "urbi/types.h"           /* UVAL_INT, UVAL_BOOL */
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "chunk/uchunk.h"
#include "value/uarena.h"
#include "watcher/uwatcher.h"     /* uwatcher_pool_alloc, drain_pending_onleave_queue */
#include "runtime/utest_hooks.h"  /* UTestHooks */
#include "runtime/umacros.h"      /* urbi_zero */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* -----------------------------------------------------------------------
 * SCHED-02 / B10: host-C write wakes a parked waituntil strand
 * --------------------------------------------------------------------- */

UTEST(host_write_wakes_waituntil_with_zero_runnable)
{
    /* refactor-3 SCHED-02/B10: a host-C slot write while the only strand is
     * parked in waituntil(cond) was never drained — the pre-loop pump in
     * urbi_step ran ISR-ring/ROS/periodics/sleepers but not the reactive
     * drain (slot-change-drain + watcher-eval-dirty).
     * Post-fix: vm_reactive_drain fires at the top of each urbi_step call,
     * draining dirty marks and waking the parked WAITUNTIL strand. */
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UArena arena;
    UProto module;
    urbi_zero(&module, sizeof module);
    uarena_init(&arena, 4096);

    /* Compile and run: loader strand parks at waituntil(Realm.go).
     * "Realm.done = 42" executes only after the strand wakes. */
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var Realm.go = false;\n"
        "waituntil(Realm.go);\n"
        "Realm.done = 42",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Drive to quiescent: the loader strand is parked in waituntil. */
    int qs = utest_e2e_run_to_no_runnable(&vm);
    UASSERT_EQ(1, qs);

    /* done must not exist yet (strand has not run past waituntil). */
    UValue done_pre;
    urbi_zero(&done_pre, sizeof done_pre);
    int pre = urbi_realm_get_global(&vm, realm, "done", strlen("done"),
                                    &done_pre);
    /* Either the slot doesn't exist (URBI_ERR_NOT_FOUND) or it is nil/0. */
    (void)pre;

    /* Host-C write: Realm.go = 1.  The write barrier fires observer_dirty →
     * dirty_count++.  Without the pre-loop drain fix, no path drains this
     * dirty mark while the only strand is parked. */
    UValue go_val;
    urbi_zero(&go_val, sizeof go_val);
    go_val.kind = UVAL_INT;
    go_val.v.i  = 1;
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, realm, "go", strlen("go"),
                                               go_val));

    /* SCHED-02 post-fix: the next urbi_step pre-loop drain fires
     * urbi_vm_watcher_eval_dirty → waituntil rising edge → strand wakes → runs
     * "Realm.done = 42" → exits.  Without the fix the strand stays parked. */
    int after = utest_e2e_run_to_no_runnable(&vm);
    UASSERT_EQ(1, after);

    /* Realm.done must now be 42. */
    UValue done_after;
    urbi_zero(&done_after, sizeof done_after);
    UASSERT_EQ(URBI_OK,
               urbi_realm_get_global(&vm, realm, "done", strlen("done"),
                                     &done_after));
    UASSERT_EQ((int)UVAL_INT, (int)done_after.kind);
    UASSERT_EQ(42LL, done_after.v.i);

    uarena_destroy(&arena);
    uchunk_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * SCHED-12: drain_pending_onleave_queue save/restore for in_eval
 * --------------------------------------------------------------------- */

UTEST(sched12_drain_preserves_in_eval_when_nested)
{
    /* SCHED-12: drain_pending_onleave_queue previously used absolute set/clear
     * for in_eval (set 1 at entry, hard-reset to 0 at exit).  A nested call
     * from within urbi_vm_watcher_eval_dirty (in_eval=1 on entry) would clobber the
     * flag to 0, dropping the outer eval's reentrancy guard and enabling a
     * pool-recycle UAF if the outer eval checked in_eval after drain returned.
     * The save/restore fix preserves the caller's in_eval value.
     *
     * The URBI_INTERNAL_ASSERT(!in_eval) was removed from drain together with
     * this fix — save/restore makes nested calls safe.
     *
     * Pre-fix: this test aborts (ASSERT) in URBI_DEBUG builds; in release
     * builds it fails on the post-call UASSERT_EQ(1, in_eval) because drain
     * hard-resets in_eval to 0.
     * Post-fix: in_eval is preserved across the call (= 1 on return). */
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    /* Simulate being inside urbi_vm_watcher_eval_dirty. */
    vm.watchers->in_eval = 1;
    /* Drain with empty queue — the save/restore must preserve in_eval = 1. */
    drain_pending_onleave_queue(&vm);
    UASSERT_EQ(1, (int)vm.watchers->in_eval);
    /* Reset for clean teardown. */
    vm.watchers->in_eval = 0;
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Onleave-orphan: mid-drain push must not be lost
 * --------------------------------------------------------------------- */

/* No-op native fn used only to back the dummy onleave closures below.  The
 * drain's run_watcher_onleave short-circuits to the watcher_onleave test hook
 * before ever calling native_fn, so this body never actually runs — it exists
 * solely so urbi_make_native_closure has a non-NULL fn to wrap. */
static int
onleave_noop_native(struct UVM *vm, UValue self, UValue *args,
                    uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    if (out != NULL) *out = urbi_make_nil();
    return 0;   /* UEXEC_OK */
}

/* Static state for the cascade hook (sequentially-run test; no concurrency). */
static int      g_orphan_count;
static bool     g_orphan_pushed;
static UWatcher *g_orphan_w2;   /* watcher pushed by the first onleave */

/* Hook: counts calls and, on the FIRST call, pushes g_orphan_w2 directly
 * onto the pending queue — simulating an onleave handler that calls
 * tag.stop() on another watcher (cascade teardown). */
static void
onleave_cascade_hook(struct UVM *vm, struct UWatcher *w)
{
    (void)w;
    g_orphan_count++;
    if (!g_orphan_pushed) {
        g_orphan_pushed = true;
        /* Direct append: mirrors what pending_onleave_queue_push does for the
         * tail-link step (the active-list unlink is skipped because
         * g_orphan_w2 was never on active_watchers_head). */
        g_orphan_w2->next_active = NULL;
        if (vm->pending_onleave_tail != NULL) {
            vm->pending_onleave_tail->next_active = g_orphan_w2;
            vm->pending_onleave_tail = g_orphan_w2;
        } else {
            /* Tail was NULL (queue just became empty after the pop and the
             * inline fix zeroed the tail).  With the fix this is the expected
             * path; without the fix tail is stale and the else branch is never
             * taken, causing head to stay NULL and w2 to be orphaned. */
            vm->pending_onleave_head = g_orphan_w2;
            vm->pending_onleave_tail = g_orphan_w2;
        }
    }
}

UTEST(onleave_orphan_mid_drain_push_not_lost)
{
    /* Onleave-orphan bug: drain_pending_onleave_queue did not update
     * pending_onleave_tail when popping the last (tail) entry.  An onleave
     * handler that pushed a new entry mid-drain appended to the stale tail
     * pointer, which was now pointing at the just-popped entry.  The new
     * entry was unreachable from head — orphaned — and the post-loop rescan
     * set head=NULL → tail=NULL, silently discarding it.
     *
     * Fix: update tail inline (rescan from head) whenever the popped entry
     * was the tail, BEFORE invoking the onleave handler.
     *
     * Test shape: single-entry queue [w1].  w1's onleave hook pushes w2.
     * Pre-fix:  g_orphan_count = 1 (w2 orphaned); UASSERT_EQ(2, ..) → FAIL.
     * Post-fix: g_orphan_count = 2 (w2 picked up in same drain pass). */
    g_orphan_count  = 0;
    g_orphan_pushed = false;

    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* Allocate two watcher slots from the VM pool. */
    UWatcher *w1 = uwatcher_pool_alloc(&vm);
    UWatcher *w2 = uwatcher_pool_alloc(&vm);
    UASSERT(w1 != NULL);
    UASSERT(w2 != NULL);

    /* onleave must be non-NULL so drain calls run_watcher_onleave (which then
     * dispatches to our test hook).  Use REAL GC-managed closures rather than
     * the deprecated integer-cast sentinel ((UClosure *)1) — the latter would
     * crash uwatcher_gc.c's walker if a GC slice ran during the drain
     * (twatcher_install_helper.h documents the deprecation).  No GC runs in this
     * test (the drain and the cascade hook allocate nothing), but real closures
     * keep the watcher GC-walkable regardless.  These closures are unrooted but
     * survive the test since nothing triggers a collection; urbi_vm_destroy
     * reclaims them at teardown. */
    w1->onleave = urbi_make_native_closure(&vm, onleave_noop_native);
    w2->onleave = urbi_make_native_closure(&vm, onleave_noop_native);
    UASSERT(w1->onleave != NULL);
    UASSERT(w2->onleave != NULL);

    /* Build a single-entry pending queue: head = w1 = tail. */
    w1->next_active          = NULL;
    vm.pending_onleave_head  = w1;
    vm.pending_onleave_tail  = w1;

    /* Pre-set active_count: 2 (w1 in queue + w2 pushed mid-drain).
     * urbi_watcher_unregister_internal decrements on each pop, so we need
     * the count to start at 2 to avoid the active_count > 0 assert. */
    vm.watchers->active_count = 2;

    g_orphan_w2 = w2;

    /* Override test_hooks with a stack-local struct for the duration of this
     * drain.  Save the original pointer (heap-allocated by urbi_vm_init via
     * utest_hooks_create) so it can be restored before urbi_vm_destroy — the
     * destroy call frees it; overwriting vm.test_hooks = NULL would leak it. */
    UTestHooks *orig_hooks = vm.test_hooks;
    UTestHooks hooks;
    urbi_zero(&hooks, sizeof hooks);
    hooks.watcher_onleave = onleave_cascade_hook;
    vm.test_hooks = &hooks;

    drain_pending_onleave_queue(&vm);

    vm.test_hooks = orig_hooks;

    /* Both handlers must have fired. */
    UASSERT_EQ(2, g_orphan_count);
    /* Queue must be fully drained. */
    UASSERT(vm.pending_onleave_head == NULL);
    UASSERT(vm.pending_onleave_tail == NULL);
    /* active_count decremented twice (w1 + w2). */
    UASSERT_EQ(0u, vm.watchers->active_count);

    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Suite entry point
 * --------------------------------------------------------------------- */

void
test_idle_vm_pump_suite(void)
{
    utest_run("idle_vm_pump: SCHED-02 host write wakes parked waituntil",
              host_write_wakes_waituntil_with_zero_runnable);
    utest_run("idle_vm_pump: SCHED-12 drain preserves in_eval when nested",
              sched12_drain_preserves_in_eval_when_nested);
    utest_run("idle_vm_pump: onleave orphan mid-drain push not lost",
              onleave_orphan_mid_drain_push_not_lost);
}
