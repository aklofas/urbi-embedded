/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: URBI_SCHED_COOPERATIVE interface (row 9 §11.2).
   Extended from 4 to 23 cases at the row 9 sweep. */

#include "utest.h"
#include "sched/usched_cooperative.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "urbi/urbi.h"   /* urbi_vm_has_live_work (case 23) */

#define UTEST(name) static void name(void)

/* Mock clock for tests that need a controllable "now". */
static uint64_t g_sched_now_us;
static uint64_t sched_mock_clock(void *ud) { (void)ud; return g_sched_now_us; }

/* Case 1: sched_init empties the ready queue and reports quiescent. */
UTEST(sched_init_empties_ready_queue)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);
    UASSERT(vm.ready_head == NULL);
    UASSERT(vm.ready_tail == NULL);
    UASSERT_EQ(vm.strand_runnable_count, 0U);
    UASSERT(sched_pick_next(&vm) == NULL);
    UASSERT(sched_quiescent(&vm));
    urbi_vm_destroy(&vm);
}

/* Case 2: sched_strand_make_runnable appends in FIFO order; pick_next
   returns the head; runnable_count reflects all three strands. */
UTEST(sched_make_runnable_appends_tail)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b, c;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);
    ustrand_init(&c, &vm);

    sched_strand_make_runnable(&a);
    sched_strand_make_runnable(&b);
    sched_strand_make_runnable(&c);

    UASSERT(sched_pick_next(&vm) == &a);
    UASSERT_EQ(vm.strand_runnable_count, 3U);
    UASSERT(!sched_quiescent(&vm));

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    ustrand_destroy(&c, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 3: sched_earliest_wake_us returns UINT64_MAX when sleep queue is empty. */
UTEST(sched_earliest_wake_uintmax_on_empty)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);
    UASSERT_EQ(sched_earliest_wake_us(&vm), UINT64_MAX);
    urbi_vm_destroy(&vm);
}

/* Case 5: sched_strand_make_runnable is idempotent in that calling it a second
   time on an already-READY strand simply tail-inserts again (re-enqueueing is
   the caller's responsibility to avoid; the scheduler itself does not guard
   against double-enqueue — test documents current behaviour). */
UTEST(sched_make_runnable_sets_state_ready)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    sched_strand_make_runnable(&s);
    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_READY);
    UASSERT_EQ(vm.strand_runnable_count, 1U);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 6: sched_strand_block (REASON_SLEEP) removes strand from the ready
   queue and inserts into the sleep queue; runnable_count decrements. */
UTEST(sched_strand_block_sleep_moves_to_sleep_q)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    /* Simulate: strand starts RUNNING (was dispatched). */
    s.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;

    sched_strand_block(&s, USTRAND_REASON_SLEEP, /*wake_us*/ 1000U);

    UASSERT_EQ((int)(s.state & USTRAND_STATE_MASK), (int)USTRAND_WAITING);
    UASSERT_EQ((int)USTRAND_GET_REASON(&s), (int)USTRAND_REASON_SLEEP);
    /* wakeup_pending_count incremented by sleep_q_insert */
    UASSERT_EQ(vm.wakeup_pending_count, 1U);
    /* runnable_count decremented */
    UASSERT_EQ(vm.strand_runnable_count, 0U);
    /* strand is on sleep queue */
    UASSERT(vm.sleep_q_head == &s);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 7: sched_dequeue_ready_head dequeues the head; after dequeue, the
   second strand is the new head.  SCHED-01 (v0.13.3): the dequeue is
   count-NEUTRAL — the dequeued strand transitions READY -> RUNNING inside
   the counted set (count == |READY| + |RUNNING|); the decrement happens at
   the park/death transition, not at dequeue (pre-refactor this test pinned
   the old decrement-at-dequeue rule). */
UTEST(sched_dequeue_ready_head_advances_queue)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    sched_strand_make_runnable(&a);
    sched_strand_make_runnable(&b);
    UASSERT_EQ(vm.strand_runnable_count, 2U);

    sched_dequeue_ready_head(&vm);
    a.state = USTRAND_STATE_RUNNING;

    UASSERT_EQ(vm.strand_runnable_count, 2U);
    UASSERT(vm.ready_head == &b);
    UASSERT(vm.ready_tail == &b);
    /* a is no longer on a list */
    UASSERT(a.ready_next == NULL);
    UASSERT(a.ready_prev == NULL);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 8: sched_pick_next on an empty queue returns NULL (no-crash). */
UTEST(sched_pick_next_empty_returns_null)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UASSERT(sched_pick_next(&vm) == NULL);

    urbi_vm_destroy(&vm);
}

/* Case 9: sleep_q_insert maintains sorted order (head, mid, tail cases).
   Use sched_strand_block to drive insertion indirectly. */
UTEST(sched_sleep_q_sorted_insertion)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand early, mid, late;
    ustrand_init(&early, &vm);
    ustrand_init(&mid,   &vm);
    ustrand_init(&late,  &vm);

    /* Insert out-of-order: late, early, mid — sorted queue should be
       early → mid → late after all three insertions. */
    early.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&early, USTRAND_REASON_SLEEP, 100U);

    late.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&late, USTRAND_REASON_SLEEP, 300U);

    mid.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&mid, USTRAND_REASON_SLEEP, 200U);

    /* Head must be 'early' (smallest wake_us). */
    UASSERT(vm.sleep_q_head == &early);
    UASSERT(vm.sleep_q_head->wait_next == &mid);
    UASSERT(vm.sleep_q_head->wait_next->wait_next == &late);
    UASSERT(late.wait_next == NULL);

    UASSERT_EQ(vm.wakeup_pending_count, 3U);

    ustrand_destroy(&early, &vm);
    ustrand_destroy(&mid,   &vm);
    ustrand_destroy(&late,  &vm);
    urbi_vm_destroy(&vm);
}

/* Case 10: sched_earliest_wake_us returns the wake_us of the sleep-queue head
   when a single strand is sleeping. */
UTEST(sched_earliest_wake_us_single_sleeper)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    s.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&s, USTRAND_REASON_SLEEP, 42U);

    UASSERT_EQ(sched_earliest_wake_us(&vm), 42U);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 11: sched_earliest_wake_us returns the minimum wake_us when multiple
   strands are sleeping. */
UTEST(sched_earliest_wake_us_picks_minimum)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    a.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&a, USTRAND_REASON_SLEEP, 500U);

    b.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&b, USTRAND_REASON_SLEEP, 200U);

    /* 'b' was inserted with wake_us=200, which is less than 'a' (500). */
    UASSERT_EQ(sched_earliest_wake_us(&vm), 200U);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 12: sched_quiescent (urbi_vm_liveness: runnable + pending + timed == 0)
   returns false when strand_runnable_count is non-zero. */
UTEST(sched_quiescent_false_when_runnable_nonzero)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UASSERT(sched_quiescent(&vm));  /* starts quiescent */

    vm.strand_runnable_count = 1;
    UASSERT(!sched_quiescent(&vm));

    vm.strand_runnable_count = 0;
    UASSERT(sched_quiescent(&vm));

    urbi_vm_destroy(&vm);
}

/* Case 13: sched_quiescent returns false while a sleeper is parked (timed
   work — VM still has live work).  v0.13.3 (SCHED-13): uses a real sleeper
   instead of hand-poking wakeup_pending_count — urbi_vm_liveness derives `timed`
   from the actual earliest deadline, so a non-zero counter with an empty
   sleep queue is an inconsistent (oracle-rejected) state, not a formula
   input. */
UTEST(sched_quiescent_false_when_sleep_q_nonempty)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a;
    ustrand_init(&a, &vm);
    a.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;   /* satisfy block's RUNNING-decrement */
    sched_strand_block(&a, USTRAND_REASON_SLEEP, 1000U);

    UASSERT(!sched_quiescent(&vm));   /* timed: sleeper with a deadline */

    sched_strand_unblock(&a);          /* off sleep_q; READY (+1 runnable) */
    UASSERT(!sched_quiescent(&vm));   /* now runnable */
    sched_strand_unbind_from_ready_queue(&a);
    UASSERT(sched_quiescent(&vm));

    ustrand_destroy(&a, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 15: sched_strand_unblock transitions a SLEEPING strand back to READY
   and decrements wakeup_pending_count. */
UTEST(sched_strand_unblock_from_sleep)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    /* Block into sleep. */
    s.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&s, USTRAND_REASON_SLEEP, 9999U);
    UASSERT_EQ(vm.wakeup_pending_count, 1U);
    UASSERT_EQ(vm.strand_runnable_count, 0U);

    /* Unblock: should move back to READY. */
    sched_strand_unblock(&s);
    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_READY);
    UASSERT_EQ(vm.wakeup_pending_count, 0U);
    UASSERT_EQ(vm.strand_runnable_count, 1U);
    UASSERT(vm.sleep_q_head == NULL);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 16: sched_destroy NULLs the scheduler queues (lifecycle cleanup). */
UTEST(sched_destroy_nulls_queues)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);
    sched_strand_make_runnable(&s);
    UASSERT(vm.ready_head != NULL);

    /* Manually decrement the runnable count so we can destroy cleanly. */
    vm.strand_runnable_count = 0;

    sched_destroy(&vm);
    UASSERT(vm.ready_head   == NULL);
    UASSERT(vm.ready_tail   == NULL);
    UASSERT(vm.sleep_q_head == NULL);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* SCHED-009: sched_destroy must zero strand_runnable_count for symmetry
 * with sched_init.  Pre-fix: sched_destroy zeroed ready_head/ready_tail/
 * sleep_q_head but left strand_runnable_count untouched, so a destroy +
 * stale-query path would observe a non-zero counter.  Post-fix: all four
 * sched-owned scheduler fields are zeroed.  Test sets the counter
 * non-zero, calls destroy, and verifies the zero invariant holds. */
UTEST(sched_destroy_zeros_strand_runnable_count)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    /* Set the scheduler-owned counter non-zero (caller never does this in
     * production — but a stale destroy + re-init scenario or a future
     * standalone destroy-then-query path needs the counter at zero). */
    vm.strand_runnable_count = 7U;

    sched_destroy(&vm);

    UASSERT_EQ(vm.strand_runnable_count, 0U);
    UASSERT(vm.ready_head   == NULL);
    UASSERT(vm.ready_tail   == NULL);
    UASSERT(vm.sleep_q_head == NULL);

    urbi_vm_destroy(&vm);
}

/* Case 17: sched_strand_block with REASON_EVENT stores the event pointer. */
UTEST(sched_strand_block_event_stores_pointer)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    s.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;

    /* Use an arbitrary non-NULL sentinel for the event pointer. */
    uintptr_t event_sentinel = 0xDEADBEEFU;
    sched_strand_block(&s, USTRAND_REASON_EVENT, (uint64_t)event_sentinel);

    UASSERT_EQ((int)(s.state & USTRAND_STATE_MASK), (int)USTRAND_WAITING);
    UASSERT_EQ((int)USTRAND_GET_REASON(&s), (int)USTRAND_REASON_EVENT);
    UASSERT((uintptr_t)s.wait_payload.event == event_sentinel);
    /* Event-wait does not insert into sleep queue. */
    UASSERT_EQ(vm.wakeup_pending_count, 0U);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 18: sched_strand_block with REASON_JOIN stores the join_parent pointer. */
UTEST(sched_strand_block_join_stores_pointer)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand parent, child;
    ustrand_init(&parent, &vm);
    ustrand_init(&child,  &vm);

    child.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;

    sched_strand_block(&child, USTRAND_REASON_JOIN, (uint64_t)(uintptr_t)&parent);

    UASSERT_EQ((int)(child.state & USTRAND_STATE_MASK), (int)USTRAND_WAITING);
    UASSERT_EQ((int)USTRAND_GET_REASON(&child), (int)USTRAND_REASON_JOIN);
    UASSERT(child.wait_payload.join_parent == &parent);
    /* Join-wait does not insert into sleep queue. */
    UASSERT_EQ(vm.wakeup_pending_count, 0U);

    ustrand_destroy(&parent, &vm);
    ustrand_destroy(&child,  &vm);
    urbi_vm_destroy(&vm);
}

/* Case 19: sleep_q_insert advances the while-loop (insert mid after 2+ elements).
   Insert 4 strands in order 400, 100, 300, 200 so that the mid inserts
   exercise the while-loop advancement path. */
UTEST(sched_sleep_q_multi_advance)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s1, s2, s3, s4;
    ustrand_init(&s1, &vm);
    ustrand_init(&s2, &vm);
    ustrand_init(&s3, &vm);
    ustrand_init(&s4, &vm);

    /* Insert 400 first, then 100 (head insert), then 300 (mid), then 200 (mid-mid). */
    s1.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&s1, USTRAND_REASON_SLEEP, 400U);

    s2.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&s2, USTRAND_REASON_SLEEP, 100U);

    s3.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&s3, USTRAND_REASON_SLEEP, 300U);

    s4.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&s4, USTRAND_REASON_SLEEP, 200U);

    /* Expected order: s2(100) → s4(200) → s3(300) → s1(400) */
    UASSERT(vm.sleep_q_head == &s2);
    UASSERT(vm.sleep_q_head->wait_next == &s4);
    UASSERT(vm.sleep_q_head->wait_next->wait_next == &s3);
    UASSERT(vm.sleep_q_head->wait_next->wait_next->wait_next == &s1);
    UASSERT_EQ(vm.wakeup_pending_count, 4U);
    UASSERT_EQ(sched_earliest_wake_us(&vm), 100U);

    ustrand_destroy(&s1, &vm);
    ustrand_destroy(&s2, &vm);
    ustrand_destroy(&s3, &vm);
    ustrand_destroy(&s4, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 20: sleep_q_remove removes a mid-queue element (non-head). */
UTEST(sched_sleep_q_remove_mid_element)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand early, mid, late;
    ustrand_init(&early, &vm);
    ustrand_init(&mid,   &vm);
    ustrand_init(&late,  &vm);

    early.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&early, USTRAND_REASON_SLEEP, 100U);

    late.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&late, USTRAND_REASON_SLEEP, 300U);

    mid.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&mid, USTRAND_REASON_SLEEP, 200U);

    /* Queue: early(100) → mid(200) → late(300). Unblock mid. */
    UASSERT_EQ(vm.wakeup_pending_count, 3U);

    /* sched_strand_unblock calls sleep_q_remove then sched_strand_make_runnable. */
    sched_strand_unblock(&mid);

    /* After removal: early(100) → late(300). */
    UASSERT_EQ(vm.wakeup_pending_count, 2U);
    UASSERT(vm.sleep_q_head == &early);
    UASSERT(vm.sleep_q_head->wait_next == &late);
    UASSERT(late.wait_next == NULL);

    /* mid is now READY. */
    UASSERT_EQ((int)mid.state, (int)USTRAND_STATE_READY);

    /* Clean up ready queue (SCHED-01: unbind owns the count decrement). */
    sched_strand_unbind_from_ready_queue(&mid);

    ustrand_destroy(&early, &vm);
    ustrand_destroy(&mid,   &vm);
    ustrand_destroy(&late,  &vm);
    urbi_vm_destroy(&vm);
}

/* Case 21: sleep_q_insert while-loop advance: insert an element that requires
   walking past 2+ nodes in the sorted sleep queue. */
UTEST(sched_sleep_q_insert_while_advance)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b, c, d;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);
    ustrand_init(&c, &vm);
    ustrand_init(&d, &vm);

    /* Build sorted queue: 100 → 200 → 350 */
    a.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&a, USTRAND_REASON_SLEEP, 100U);

    b.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&b, USTRAND_REASON_SLEEP, 200U);

    c.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&c, USTRAND_REASON_SLEEP, 350U);

    /* Insert 300: cur starts at a(100), 200<=300 is true → advance.
       cur becomes b(200), 350<=300 is false → insert d after b.
       Expected: a(100) → b(200) → d(300) → c(350). */
    d.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&d, USTRAND_REASON_SLEEP, 300U);

    UASSERT(vm.sleep_q_head == &a);
    UASSERT(vm.sleep_q_head->wait_next == &b);
    UASSERT(vm.sleep_q_head->wait_next->wait_next == &d);
    UASSERT(vm.sleep_q_head->wait_next->wait_next->wait_next == &c);
    UASSERT_EQ(vm.wakeup_pending_count, 4U);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    ustrand_destroy(&c, &vm);
    ustrand_destroy(&d, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 22: sleep_q_remove mid-element via while-loop advance (non-head removal
   where the removal target is 2+ nodes from the head). */
UTEST(sched_sleep_q_remove_while_advance)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b, c;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);
    ustrand_init(&c, &vm);

    /* Queue: a(100) → b(200) → c(300). */
    a.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&a, USTRAND_REASON_SLEEP, 100U);
    b.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&b, USTRAND_REASON_SLEEP, 200U);
    c.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&c, USTRAND_REASON_SLEEP, 300U);

    /* Unblock c (tail element): sleep_q_remove must walk from head to find it. */
    sched_strand_unblock(&c);

    /* Queue: a(100) → b(200). */
    UASSERT_EQ(vm.wakeup_pending_count, 2U);
    UASSERT(vm.sleep_q_head == &a);
    UASSERT(vm.sleep_q_head->wait_next == &b);
    UASSERT(b.wait_next == NULL);

    /* Clean up ready queue (c is now READY; SCHED-01: unbind owns the
     * count decrement). */
    sched_strand_unbind_from_ready_queue(&c);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    ustrand_destroy(&c, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 23: armed watchers do NOT block sched_quiescent (refactor-3 SCHED-13,
 * owner decision 2026-06-11: `armed` is external-input work — reported via
 * urbi_vm_has_live_work, excluded from quiescence).  Pre-fix this case
 * pinned the inverse formula, which made any idle-but-armed VM busy-spin. */
UTEST(sched_quiescent_true_when_watcher_active_nonzero)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UASSERT(sched_quiescent(&vm));

    vm.watchers->active_count = 1;
    UASSERT(sched_quiescent(&vm));                       /* armed-only: quiescent */
    UASSERT(urbi_vm_has_live_work(&vm, NULL, NULL, NULL)); /* but reported */

    vm.watchers->active_count = 0;
    UASSERT(sched_quiescent(&vm));
    UASSERT(!urbi_vm_has_live_work(&vm, NULL, NULL, NULL));

    urbi_vm_destroy(&vm);
}

/* Case 24: sched_quiescent returns false on pending reactive work
 * (watchers->dirty_count — urbi_vm_liveness `pending`).  Replaces the deleted
 * event_queue_count pin (vestigial M3 stub removed at v0.13.3/SCHED-13;
 * ISR-ring pendingness is queried live via uevent_ring_has_pending). */
UTEST(sched_quiescent_false_when_watcher_dirty_nonzero)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UASSERT(sched_quiescent(&vm));

    vm.watchers->dirty_count = 1;
    UASSERT(!sched_quiescent(&vm));

    vm.watchers->dirty_count = 0;
    UASSERT(sched_quiescent(&vm));

    urbi_vm_destroy(&vm);
}

/* Case 25: sched_quiescent returns false when host_call_pending_count is non-zero. */
UTEST(sched_quiescent_false_when_host_call_pending_nonzero)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UASSERT(sched_quiescent(&vm));

    vm.host_call_pending_count = 1;
    UASSERT(!sched_quiescent(&vm));

    vm.host_call_pending_count = 0;
    UASSERT(sched_quiescent(&vm));

    urbi_vm_destroy(&vm);
}

/* Case 26: urbi_gc_sched_walk_roots is a no-op stub; calling it doesn't crash. */
UTEST(sched_walk_roots_noop)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_gc_sched_walk_roots(&vm, NULL, NULL);
    UASSERT(1);  /* reached here without crash */
    urbi_vm_destroy(&vm);
}

/* CHSTR-025: wait_payload union arms are correctly tagged by the strand's
 * wait_reason byte after sched_strand_block.  This test exercises all three
 * live arms and confirms the read site contract: each arm's value matches
 * the payload supplied at block time, and USTRAND_GET_REASON is the
 * discriminator. */
UTEST(sched_wait_payload_reason_discriminates_arms)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand sleeper, eventer, joiner, parent;
    ustrand_init(&sleeper, &vm);
    ustrand_init(&eventer, &vm);
    ustrand_init(&joiner,  &vm);
    ustrand_init(&parent,  &vm);

    /* SLEEP arm. */
    sleeper.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;
    sched_strand_block(&sleeper, USTRAND_REASON_SLEEP, 12345ULL);
    UASSERT_EQ((int)USTRAND_GET_REASON(&sleeper), (int)USTRAND_REASON_SLEEP);
    UASSERT_EQ(sleeper.wait_payload.wake_us, 12345ULL);

    /* EVENT arm. */
    eventer.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;
    sched_strand_block(&eventer, USTRAND_REASON_EVENT, (uint64_t)0xCAFEBABEU);
    UASSERT_EQ((int)USTRAND_GET_REASON(&eventer), (int)USTRAND_REASON_EVENT);
    UASSERT((uintptr_t)eventer.wait_payload.event == (uintptr_t)0xCAFEBABEU);

    /* JOIN arm. */
    joiner.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;
    sched_strand_block(&joiner, USTRAND_REASON_JOIN, (uint64_t)(uintptr_t)&parent);
    UASSERT_EQ((int)USTRAND_GET_REASON(&joiner), (int)USTRAND_REASON_JOIN);
    UASSERT(joiner.wait_payload.join_parent == &parent);

    ustrand_destroy(&sleeper, &vm);
    ustrand_destroy(&eventer, &vm);
    ustrand_destroy(&joiner,  &vm);
    ustrand_destroy(&parent,  &vm);
    urbi_vm_destroy(&vm);
}

/* Case 27: sched_wake_due_sleepers skips a due transient and wakes the real
 * strand immediately behind it via the predecessor-pointer re-read (f84ccb59).
 *
 * Queue at entry: A(500µs, transient, due) → B(700µs, real, due)
 *                 → C(2000µs, real, not-due).  now = 1000µs.
 *
 * Walk:
 *   A is due but transient → pp = &A->wait_next; continue.
 *   *pp = B; B is due and real → sched_strand_unblock(B).
 *     sleep_q_remove walks from head, finds A as B's predecessor,
 *     sets A->wait_next = C (B's old successor).  *pp = A->wait_next = C.
 *   *pp = C; C->wake_us (2000) > now (1000) → break.
 *
 * Post-conditions: B READY, A still WAITING at head with A->wait_next = C,
 * C WAITING, wakeup_pending_count = 2 (A and C). */
UTEST(sched_wake_due_sleepers_skips_transient_wakes_real)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    /* Install mock clock; "now" = 1000 µs. */
    g_sched_now_us   = 1000U;
    vm.host_time_us  = sched_mock_clock;

    UStrand a, b, c;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);
    ustrand_init(&c, &vm);

    /* A: transient, due (500 < 1000).  Transient strands bypass runnable
     * counting, so strand_runnable_count is not bumped before blocking. */
    a.is_transient_strand = 1;
    a.state = USTRAND_STATE_RUNNING;
    sched_strand_block(&a, USTRAND_REASON_SLEEP, 500U);

    /* B: real, due (700 < 1000). */
    b.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&b, USTRAND_REASON_SLEEP, 700U);

    /* C: real, not due (2000 > 1000). */
    c.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&c, USTRAND_REASON_SLEEP, 2000U);

    /* Verify sorted queue before the wake walk. */
    UASSERT(vm.sleep_q_head == &a);
    UASSERT(a.wait_next     == &b);
    UASSERT(b.wait_next     == &c);
    UASSERT(c.wait_next     == NULL);
    UASSERT_EQ(vm.wakeup_pending_count, 3U);
    UASSERT_EQ(vm.strand_runnable_count, 0U);

    sched_wake_due_sleepers(&vm);

    /* A: still parked at head (transient skip); wait_next spliced to C after
     * sleep_q_remove evicted B. */
    UASSERT(vm.sleep_q_head == &a);
    UASSERT(USTRAND_IS_WAITING(&a));
    UASSERT(a.wait_next     == &c);

    /* B: woken across the parked transient. */
    UASSERT_EQ((int)b.state, (int)USTRAND_STATE_READY);
    UASSERT_EQ(vm.strand_runnable_count, 1U);

    /* C: not due; loop terminated at the break; still on sleep queue. */
    UASSERT(USTRAND_IS_WAITING(&c));
    UASSERT(c.wait_next == NULL);

    /* Counter: B removed, A (transient, not counted) and C remain. */
    UASSERT_EQ(vm.wakeup_pending_count, 2U);

    /* Teardown: B is on the ready queue; unbind before destroy. */
    sched_strand_unbind_from_ready_queue(&b);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    ustrand_destroy(&c, &vm);
    urbi_vm_destroy(&vm);
}

void test_scheduler_cooperative_suite(void) {
    utest_run("sched_init_empties_ready_queue",           sched_init_empties_ready_queue);
    utest_run("sched_make_runnable_appends_tail",         sched_make_runnable_appends_tail);
    utest_run("sched_earliest_wake_uintmax_on_empty",     sched_earliest_wake_uintmax_on_empty);
    utest_run("sched_make_runnable_sets_state_ready",     sched_make_runnable_sets_state_ready);
    utest_run("sched_strand_block_sleep_moves_to_sleep_q", sched_strand_block_sleep_moves_to_sleep_q);
    utest_run("sched_dequeue_ready_head_advances_queue",  sched_dequeue_ready_head_advances_queue);
    utest_run("sched_pick_next_empty_returns_null",       sched_pick_next_empty_returns_null);
    utest_run("sched_sleep_q_sorted_insertion",           sched_sleep_q_sorted_insertion);
    utest_run("sched_earliest_wake_us_single_sleeper",    sched_earliest_wake_us_single_sleeper);
    utest_run("sched_earliest_wake_us_picks_minimum",     sched_earliest_wake_us_picks_minimum);
    utest_run("sched_quiescent_false_when_runnable_nonzero", sched_quiescent_false_when_runnable_nonzero);
    utest_run("sched_quiescent_false_when_sleep_q_nonempty", sched_quiescent_false_when_sleep_q_nonempty);
    utest_run("sched_strand_unblock_from_sleep",          sched_strand_unblock_from_sleep);
    utest_run("sched_destroy_nulls_queues",               sched_destroy_nulls_queues);
    utest_run("sched_destroy_zeros_strand_runnable_count",
              sched_destroy_zeros_strand_runnable_count);
    utest_run("sched_strand_block_event_stores_pointer",  sched_strand_block_event_stores_pointer);
    utest_run("sched_strand_block_join_stores_pointer",   sched_strand_block_join_stores_pointer);
    utest_run("sched_sleep_q_multi_advance",              sched_sleep_q_multi_advance);
    utest_run("sched_sleep_q_remove_mid_element",         sched_sleep_q_remove_mid_element);
    utest_run("sched_sleep_q_insert_while_advance",       sched_sleep_q_insert_while_advance);
    utest_run("sched_sleep_q_remove_while_advance",       sched_sleep_q_remove_while_advance);
    utest_run("sched_quiescent_true_when_watcher_active_nonzero", sched_quiescent_true_when_watcher_active_nonzero);
    utest_run("sched_quiescent_false_when_watcher_dirty_nonzero", sched_quiescent_false_when_watcher_dirty_nonzero);
    utest_run("sched_quiescent_false_when_host_call_pending_nonzero", sched_quiescent_false_when_host_call_pending_nonzero);
    utest_run("sched_walk_roots_noop",                    sched_walk_roots_noop);
    utest_run("sched_wait_payload_reason_discriminates_arms",
              sched_wait_payload_reason_discriminates_arms);
    utest_run("sched_wake_due_sleepers_skips_transient_wakes_real",
              sched_wake_due_sleepers_skips_transient_wakes_real);
}
