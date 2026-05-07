/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: urbi_tag_stop synchronous deposit + host_call_pending_count wiring
 * (row 11 §3.5).
 *
 * Tests cover:
 *  1. tag_stop_deposits_on_member_strands: both strands get TAG_STOP + target.
 *  2. tag_stop_increments_host_call_pending_count_per_fresh_strand: N=3 strands.
 *  3. tag_stop_idempotent_on_repeat_call: second call does not double-increment.
 *  4. tag_stop_does_not_overwrite_cancel: CANCEL is higher priority than TAG_STOP.
 *  5. tag_stop_overwrites_throw: TAG_STOP overwrites THROW (row 7 C-1).
 *  6. tag_stop_decrement_on_strand_destroy: counter falls as strands are destroyed. */

#include "utest.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "runtime/ucleanup.h"
#include "utag.h"
#include "watcher/uwatcher.h"
#include "urbi/urbi.h"

#define UTEST(name) static void name(void)

/* === Helpers === */

static UValue
make_nil(void)
{
    UValue v;
    v.kind = UVAL_NIL;
    v.v.i  = 0;
    return v;
}

/* Create a strand via urbi_strand_create (attaches realm->tag ambient). */
static UStrand *
create_member_strand(URealm *r)
{
    return urbi_strand_create(r, NULL);
}

/* No-op onleave hook: prevents run_watcher_onleave from dispatching the
 * (UClosure *)1 sentinel through real bytecode. */
static void
onleave_drain_noop(struct UVM *vm, struct UWatcher *w)
{
    (void)vm; (void)w;
}

/* === Test cases === */

/* 1. tag_stop_deposits_on_member_strands
 *
 * Create 2 strands under a realm; both inherit realm->tag.
 * Call urbi_tag_stop on realm->tag; both strands must receive UEXEC_TAG_STOP
 * with unwind_target == realm->tag. */
UTEST(tag_stop_deposits_on_member_strands)
{
    UVM vm;
    UValue nil = make_nil();

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UASSERT(r->tag != NULL);

    UStrand *s1 = create_member_strand(r);
    UStrand *s2 = create_member_strand(r);
    UASSERT(s1 != NULL);
    UASSERT(s2 != NULL);

    /* Both strands must appear in realm->tag member list. */
    UASSERT(r->tag->member_strands_head != NULL);

    int rc = urbi_tag_stop(&vm, r->tag, nil);
    UASSERT_EQ(rc, URBI_OK);

    /* Both strands get TAG_STOP with the correct target. */
    UASSERT_EQ((int)s1->pending_unwind, (int)UEXEC_TAG_STOP);
    UASSERT(s1->unwind_target == r->tag);
    UASSERT_EQ((int)s2->pending_unwind, (int)UEXEC_TAG_STOP);
    UASSERT(s2->unwind_target == r->tag);

    urbi_strand_destroy(s1);
    urbi_strand_destroy(s2);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 2. tag_stop_increments_host_call_pending_count_per_fresh_strand
 *
 * 3 strands — all DORMANT (fresh_deposit=true for each).
 * After urbi_tag_stop, host_call_pending_count must be 3. */
UTEST(tag_stop_increments_host_call_pending_count_per_fresh_strand)
{
    UVM vm;
    UValue nil = make_nil();

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UStrand *s1 = create_member_strand(r);
    UStrand *s2 = create_member_strand(r);
    UStrand *s3 = create_member_strand(r);
    UASSERT(s1 != NULL && s2 != NULL && s3 != NULL);

    UASSERT_EQ((int)vm.host_call_pending_count, 0);

    urbi_tag_stop(&vm, r->tag, nil);

    UASSERT_EQ((int)vm.host_call_pending_count, 3);

    urbi_strand_destroy(s1);
    urbi_strand_destroy(s2);
    urbi_strand_destroy(s3);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 3. tag_stop_idempotent_on_repeat_call
 *
 * Call urbi_tag_stop twice on the same tag.  The cross_strand_stop_pending
 * flag must prevent double-incrementing the counter. */
UTEST(tag_stop_idempotent_on_repeat_call)
{
    UVM vm;
    UValue nil = make_nil();

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UStrand *s = create_member_strand(r);
    UASSERT(s != NULL);

    urbi_tag_stop(&vm, r->tag, nil);
    UASSERT_EQ((int)vm.host_call_pending_count, 1);

    /* Second call: flag is sticky, counter must not change. */
    urbi_tag_stop(&vm, r->tag, nil);
    UASSERT_EQ((int)vm.host_call_pending_count, 1);

    urbi_strand_destroy(s);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 4. tag_stop_does_not_overwrite_cancel
 *
 * A strand with UEXEC_CANCEL must not have its pending_unwind overwritten
 * by urbi_tag_stop; the counter must also not be incremented for that strand
 * (CANCEL > TAG_STOP per row 7 C-1). */
UTEST(tag_stop_does_not_overwrite_cancel)
{
    UVM vm;
    UValue nil = make_nil();

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    /* Two strands: s_cancel pre-set to CANCEL; s_ok stays OK. */
    UStrand *s_cancel = create_member_strand(r);
    UStrand *s_ok     = create_member_strand(r);
    UASSERT(s_cancel != NULL && s_ok != NULL);

    s_cancel->pending_unwind = UEXEC_CANCEL;

    urbi_tag_stop(&vm, r->tag, nil);

    /* s_cancel must retain CANCEL. */
    UASSERT_EQ((int)s_cancel->pending_unwind, (int)UEXEC_CANCEL);
    /* s_ok must get TAG_STOP. */
    UASSERT_EQ((int)s_ok->pending_unwind, (int)UEXEC_TAG_STOP);

    /* Counter must only reflect the fresh deposit on s_ok (= 1), not s_cancel. */
    UASSERT_EQ((int)vm.host_call_pending_count, 1);

    urbi_strand_destroy(s_cancel);
    urbi_strand_destroy(s_ok);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 5. tag_stop_overwrites_throw
 *
 * A strand with UEXEC_THROW must be overwritten by TAG_STOP (TAG_STOP > THROW). */
UTEST(tag_stop_overwrites_throw)
{
    UVM vm;
    UValue nil = make_nil();

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UStrand *s = create_member_strand(r);
    UASSERT(s != NULL);

    s->pending_unwind = UEXEC_THROW;

    urbi_tag_stop(&vm, r->tag, nil);

    /* TAG_STOP must overwrite THROW. */
    UASSERT_EQ((int)s->pending_unwind, (int)UEXEC_TAG_STOP);
    UASSERT(s->unwind_target == r->tag);

    /* fresh_deposit was true (THROW maps to fresh), so counter incremented. */
    UASSERT_EQ((int)vm.host_call_pending_count, 1);

    urbi_strand_destroy(s);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 6. tag_stop_decrement_on_strand_destroy
 *
 * Deposit on N strands; verify counter == N.
 * Destroy strands one by one; counter must track. */
UTEST(tag_stop_decrement_on_strand_destroy)
{
    UVM vm;
    UValue nil = make_nil();

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UStrand *s1 = create_member_strand(r);
    UStrand *s2 = create_member_strand(r);
    UStrand *s3 = create_member_strand(r);
    UASSERT(s1 != NULL && s2 != NULL && s3 != NULL);

    urbi_tag_stop(&vm, r->tag, nil);
    UASSERT_EQ((int)vm.host_call_pending_count, 3);

    /* Destroy s1 — counter must drop to 2. */
    urbi_strand_destroy(s1);
    UASSERT_EQ((int)vm.host_call_pending_count, 2);

    /* Destroy s2 — counter must drop to 1. */
    urbi_strand_destroy(s2);
    UASSERT_EQ((int)vm.host_call_pending_count, 1);

    /* Destroy s3 — counter must reach 0. */
    urbi_strand_destroy(s3);
    UASSERT_EQ((int)vm.host_call_pending_count, 0);

    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* ============================================================
 * §9.3 gap-fill: watcher cascade + drain ordering
 * ============================================================ */

/* 7. realm_destroy_cascade_watchers
 *
 * Install a watcher on realm->tag.  Call urbi_tag_stop (mimicking the
 * realm-destroy cascade path).  Verify the watcher ends up on the
 * pending_onleave_queue and is properly removed from tag->member_watchers_head. */
UTEST(realm_destroy_cascade_watchers)
{
    UVM    vm;
    UValue nil = make_nil();

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UASSERT(r->tag != NULL);

    /* Install a watcher owned by realm->tag. */
    UWatcher *w = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, r->tag, NULL, NULL, NULL, NULL, 0u);
    UASSERT(w != NULL);
    UASSERT(r->tag->member_watchers_head == w);

    /* urbi_tag_stop cascades watcher list to pending_onleave_queue. */
    int rc = urbi_tag_stop(&vm, r->tag, nil);
    UASSERT_EQ(rc, URBI_OK);

    /* Watcher must be on the pending_onleave_queue. */
    UASSERT(vm.pending_onleave_head == w);
    /* Tag's member_watchers_head cleared by the cascade. */
    UASSERT(r->tag->member_watchers_head == NULL);

    /* Drain to unregister. */
    drain_pending_onleave_queue(&vm);
    UASSERT(vm.pending_onleave_head == NULL);
    UASSERT_EQ((long long)vm.watcher_active_count, 0LL);

    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 8. realm_destroy_drain_ordering
 *
 * Install a watcher; push it to pending_onleave_queue; set dirty count.
 * Verify that drain_pending_onleave_queue runs the onleave path BEFORE
 * watcher_eval_dirty would run (i.e. drain clears in_watcher_eval correctly,
 * leaving it available for a subsequent eval pass). */
UTEST(realm_destroy_drain_ordering)
{
    UVM    vm;

    uvm_init(&vm, NULL, NULL);

    /* Install a no-op onleave hook so drain doesn't dereference the
     * (UClosure *)1 sentinel.  The test observes drain state, not onleave
     * effects, so hook semantics don't matter. */
    vm.test_watcher_onleave_hook = onleave_drain_noop;

    UWatcher *w = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, NULL, NULL,
        /*onleave=*/(UClosure *)1, NULL, 0u);
    UASSERT(w != NULL);

    /* Simulate a dirty condition AND a pending cleanup simultaneously. */
    vm.watcher_dirty_count = 3u;
    pending_onleave_queue_push(&vm, w);

    /* Before drain: in_watcher_eval must be false (drain hasn't run yet). */
    UASSERT(!vm.in_watcher_eval);

    /* Run drain — must leave in_watcher_eval false when it returns.
     * After drain completes, watcher_dirty_count is unchanged (drain does not
     * call watcher_eval_dirty; that's the scheduler's job after drain). */
    drain_pending_onleave_queue(&vm);
    UASSERT(!vm.in_watcher_eval);
    UASSERT(vm.pending_onleave_head == NULL);
    /* Dirty count must NOT have been cleared by drain alone. */
    UASSERT_EQ((unsigned)vm.watcher_dirty_count, 3u);

    /* Now eval can run cleanly. */
    watcher_eval_dirty(&vm);
    UASSERT_EQ((unsigned)vm.watcher_dirty_count, 0u);
    UASSERT(!vm.in_watcher_eval);

    uvm_destroy(&vm);
}

/* === Suite entry point === */

void
test_tag_stop_realm_suite(void)
{
    printf("test_tag_stop_realm\n");
    utest_run("tag_stop deposits on member strands",
              tag_stop_deposits_on_member_strands);
    utest_run("tag_stop increments host_call_pending_count per fresh strand",
              tag_stop_increments_host_call_pending_count_per_fresh_strand);
    utest_run("tag_stop idempotent on repeat call",
              tag_stop_idempotent_on_repeat_call);
    utest_run("tag_stop does not overwrite CANCEL",
              tag_stop_does_not_overwrite_cancel);
    utest_run("tag_stop overwrites THROW",
              tag_stop_overwrites_throw);
    utest_run("tag_stop counter decrements on strand destroy",
              tag_stop_decrement_on_strand_destroy);
    /* §9.3 gap-fill: watcher cascade + drain ordering */
    utest_run("realm_destroy_cascade_watchers",    realm_destroy_cascade_watchers);
    utest_run("realm_destroy_drain_ordering",      realm_destroy_drain_ordering);
}
