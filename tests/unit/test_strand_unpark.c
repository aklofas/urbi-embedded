/* SPDX-License-Identifier: BSD-3-Clause */
/* test_strand_unpark — refactor-3 SCHED-05: tag-stop/cancel of a parked
 * strand must remove the strand's third-party links (the wake-side mirror
 * of strand_cleanup_observers).
 *
 * Pre-fix failure modes (OBSERVED on the pre-fix tree):
 *   - JOIN: urbi_tag_stop woke a JOIN-parked parent via
 *     urbi_sched_strand_make_runnable but left it threaded on
 *     child->joiners_head.  When the child later reached DEAD,
 *     urbi_vm_fork_wake_joiners walked the (possibly freed) parent — ASan
 *     heap-use-after-free, or a READY->READY make_runnable (SCHED-005
 *     assert / circular ready-queue corruption).
 *   - WATCHER: urbi_tag_stop woke a waituntil(cond)-parked strand but left
 *     w->waiter_strand pointing at it.  The next rising edge had
 *     urbi_vm_watcher_eval_dirty call urbi_sched_strand_make_runnable on a DEAD/freed
 *     strand (ASan UAF; pinned end-to-end by
 *     tests/chk/tag/stop_waituntil_nested.chk).
 *
 * Post-fix contract: urbi_sched_strand_unpark(s, enqueue) performs the
 * reason-dispatched unlink (SLEEP: sleep queue; EVENT: waiter chain; JOIN:
 * child->joiners_head; WATCHER: w->waiter_strand scrub + watcher
 * unregister) before the strand leaves WAITING. */

#include "utest.h"

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "vm/uop_fork.h"               /* urbi_vm_fork_wake_joiners */
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"
#include "realm/urealm.h"
#include "tag/utag.h"
#include "watcher/uwatcher.h"
#include "watcher/uwatcher_state.h"
#include "twatcher_install_helper.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

static UValue
make_nil(void)
{
    UValue v;
    v.kind = UVAL_NIL;
    v.v.i  = 0;
    return v;
}

/* ===================================================================
 * Case 1: tag-stop of a JOIN-parked parent unlinks it from
 * child->joiners_head (SCHED-05).  Constructs the park exactly as
 * src/vm/uop_fork.c's OP_JOIN_WAIT does (block-then-link), fires
 * urbi_tag_stop on the shared realm tag, then frees the woken parent and
 * drives the child's death-path joiner wake.  Pre-fix: the parent stays
 * linked (assert red) and urbi_vm_fork_wake_joiners walks freed memory (ASan red).
 * =================================================================== */
UTEST(tag_stop_join_parked_unlinks_joiner)
{
    UVM    vm;
    UValue nil = make_nil();

    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UASSERT(r->tag != NULL);

    UStrand *parent = urbi_strand_create(&vm, r, NULL);
    UStrand *child  = urbi_strand_create(&vm, r, NULL);
    UASSERT(parent != NULL && child != NULL);

    /* Park parent JOIN-blocked on child, mirroring OP_JOIN_WAIT:
     * block first (RUNNING -> WAITING|JOIN), then thread onto the
     * child's joiners chain via wait_next. */
    parent->state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count++;     /* satisfy block's RUNNING-decrement */
    urbi_sched_strand_block(parent, USTRAND_REASON_JOIN,
                       (uint64_t)(uintptr_t)child);
    parent->wait_next   = child->joiners_head;
    child->joiners_head = parent;
    UASSERT_EQ(1U, vm.strand_waiting_count);

    /* Cross-strand stop on the realm tag (both strands are members). */
    UASSERT_EQ(URBI_OK, urbi_tag_stop(&vm, r->tag, nil));

    /* SCHED-05: the woken parent must have left the joiners chain. */
    UASSERT_EQ((unsigned)USTRAND_READY, (unsigned)USTRAND_GET_STATE(parent));
    UASSERT(child->joiners_head == NULL);
    UASSERT(parent->wait_next == NULL);
    UASSERT_EQ(0U, vm.strand_waiting_count);

    /* Drive the would-be waker: free the parent, then run the child's
     * death-path joiner wake.  Pre-fix this walked the freed parent. */
    urbi_strand_destroy(&vm, parent);
    urbi_vm_fork_wake_joiners(child, &vm);
    UASSERT(child->joiners_head == NULL);

    urbi_realm_destroy(&vm, r);   /* frees child */
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 2: tag-stop of a WATCHER-parked strand (waituntil(cond)) scrubs
 * w->waiter_strand and retires the watcher (SCHED-05).  A waituntil
 * watcher without a waiter has nothing left to wake; leaving it armed
 * means the next rising edge wakes a DEAD/freed strand.
 * =================================================================== */
UTEST(tag_stop_watcher_parked_scrubs_waiter)
{
    UVM    vm;
    UValue nil = make_nil();

    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UStrand *s = urbi_strand_create(&vm, r, NULL);
    UASSERT(s != NULL);

    /* Install a WAITUNTIL watcher (no owning tag — keeps it off the
     * stopped tag's member cascade so only the unpark scrub can clear it)
     * and park s on it, mirroring urbi_watcher_install_watcher_runtime's park. */
    UWatcher *w = urbi_watcher_install_for_test(
        &vm, UWATCHER_WAITUNTIL, NULL, NULL, NULL, NULL, NULL, 0);
    UASSERT(w != NULL);
    w->waiter_strand = s;

    s->state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count++;     /* satisfy block's RUNNING-decrement */
    urbi_sched_strand_block(s, USTRAND_REASON_WATCHER, 0);
    UASSERT_EQ(1U, vm.strand_waiting_count);

    UASSERT_EQ(URBI_OK, urbi_tag_stop(&vm, r->tag, nil));

    /* SCHED-05: woken strand; watcher back-pointer scrubbed and the
     * now-waiterless waituntil watcher retired from the active list. */
    UASSERT_EQ((unsigned)USTRAND_READY, (unsigned)USTRAND_GET_STATE(s));
    UASSERT(vm.active_watchers_head == NULL);
    UASSERT_EQ(0U, vm.watchers->active_count);
    UASSERT_EQ(0U, vm.strand_waiting_count);

    urbi_strand_destroy(&vm, s);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 3 (spec-review hazard 1): unparking a WATCHER-parked strand while
 * vm->watchers->in_eval is set (a tag-stop/cancel fired from an AT_SYNC
 * inline body or a drain onleave handler) must NOT pool_free the watcher
 * inline — urbi_vm_watcher_eval_dirty's walk holds a `next` snapshot, and a freed
 * slot's next_active is repurposed as the pool freelist link (the walk
 * would wander into mode-0 freelist slots).  Mid-eval the retire is
 * deferred via urbi_watcher_pending_onleave_queue_push (PENDING_UNREGISTER keeps the
 * snapshot skippable-but-valid); the pool_free happens at the next
 * safepoint drain.  End-to-end shape pinned by
 * tests/chk/tag/stop_waituntil_mid_eval.chk.
 * =================================================================== */
UTEST(unpark_watcher_mid_eval_defers_retire)
{
    UVM vm;

    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UStrand *s = urbi_strand_create(&vm, r, NULL);
    UASSERT(s != NULL);

    UWatcher *w = urbi_watcher_install_for_test(
        &vm, UWATCHER_WAITUNTIL, NULL, NULL, NULL, NULL, NULL, 0);
    UASSERT(w != NULL);
    w->waiter_strand = s;

    s->state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count++;     /* satisfy block's RUNNING-decrement */
    urbi_sched_strand_block(s, USTRAND_REASON_WATCHER, 0);

    uint16_t in_use_before = vm.watchers->pool_in_use;
    vm.watchers->in_eval = 1;       /* simulate a mid-eval wake */
    urbi_sched_strand_unpark(s, /*enqueue=*/1);
    vm.watchers->in_eval = 0;

    /* Deferred: scrubbed + PENDING_UNREGISTER + on the pending FIFO with
     * the slot still allocated; off the active list. */
    UASSERT(w->waiter_strand == NULL);
    UASSERT((w->flags & URBI_WATCHER_PENDING_UNREGISTER) != 0U);
    UASSERT(vm.pending_onleave_head == w);
    UASSERT(vm.active_watchers_head == NULL);
    UASSERT_EQ((unsigned)in_use_before, (unsigned)vm.watchers->pool_in_use);
    UASSERT_EQ((unsigned)USTRAND_READY, (unsigned)USTRAND_GET_STATE(s));

    /* The deferred drain performs the actual retire + pool_free. */
    urbi_watcher_drain_pending_onleave_queue(&vm);
    UASSERT(vm.pending_onleave_head == NULL);
    UASSERT_EQ((unsigned)(in_use_before - 1U),
               (unsigned)vm.watchers->pool_in_use);
    UASSERT_EQ(0U, vm.watchers->active_count);

    urbi_strand_destroy(&vm, s);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void test_strand_unpark_suite(void);

void
test_strand_unpark_suite(void)
{
    printf("test_strand_unpark\n");
    utest_run("tag_stop_join_parked_unlinks_joiner",
              tag_stop_join_parked_unlinks_joiner);
    utest_run("tag_stop_watcher_parked_scrubs_waiter",
              tag_stop_watcher_parked_scrubs_waiter);
    utest_run("unpark_watcher_mid_eval_defers_retire",
              unpark_watcher_mid_eval_defers_retire);
}
