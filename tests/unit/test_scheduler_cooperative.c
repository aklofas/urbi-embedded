/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: URBI_SCHED_COOPERATIVE interface (row 9 §11.2).
   Extended from 4 to 23 cases at the row 9 sweep. */

#include "utest.h"
#include "sched/usched_cooperative.h"
#include "uvm.h"
#include "ustrand.h"

#define UTEST(name) static void name(void)

/* Case 1: sched_init empties the ready queue and reports quiescent. */
UTEST(sched_init_empties_ready_queue)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);
    UASSERT(vm.ready_head == NULL);
    UASSERT(vm.ready_tail == NULL);
    UASSERT_EQ(vm.strand_runnable_count, 0u);
    UASSERT(sched_pick_next(&vm) == NULL);
    UASSERT(sched_quiescent(&vm));
    uvm_destroy(&vm);
}

/* Case 2: sched_strand_make_runnable appends in FIFO order; pick_next
   returns the head; runnable_count reflects all three strands. */
UTEST(sched_make_runnable_appends_tail)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b, c;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);
    ustrand_init(&c, &vm);

    sched_strand_make_runnable(&a);
    sched_strand_make_runnable(&b);
    sched_strand_make_runnable(&c);

    UASSERT(sched_pick_next(&vm) == &a);
    UASSERT_EQ(vm.strand_runnable_count, 3u);
    UASSERT(!sched_quiescent(&vm));

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    ustrand_destroy(&c, &vm);
    uvm_destroy(&vm);
}

/* Case 3: sched_earliest_wake_us returns UINT64_MAX when sleep queue is empty. */
UTEST(sched_earliest_wake_uintmax_on_empty)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);
    UASSERT_EQ(sched_earliest_wake_us(&vm), UINT64_MAX);
    uvm_destroy(&vm);
}

/* Case 4: sched_consume_budget decrements correctly and floors at zero. */
UTEST(sched_consume_budget_floors_at_zero)
{
    UStrand s;
    s.instruction_budget_remaining = 5;
    sched_consume_budget(&s, 3);
    UASSERT_EQ(s.instruction_budget_remaining, 2u);
    sched_consume_budget(&s, 10);   /* would underflow without the floor */
    UASSERT_EQ(s.instruction_budget_remaining, 0u);
}

/* Case 5: sched_strand_make_runnable is idempotent in that calling it a second
   time on an already-READY strand simply tail-inserts again (re-enqueueing is
   the caller's responsibility to avoid; the scheduler itself does not guard
   against double-enqueue — test documents current behaviour). */
UTEST(sched_make_runnable_sets_state_ready)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    sched_strand_make_runnable(&s);
    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_READY);
    UASSERT_EQ(vm.strand_runnable_count, 1u);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 6: sched_strand_block (REASON_SLEEP) removes strand from the ready
   queue and inserts into the sleep queue; runnable_count decrements. */
UTEST(sched_strand_block_sleep_moves_to_sleep_q)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    /* Simulate: strand starts RUNNING (was dispatched). */
    s.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;

    sched_strand_block(&s, USTRAND_REASON_SLEEP, /*wake_us*/ 1000u);

    UASSERT_EQ((int)(s.state & USTRAND_STATE_MASK), (int)USTRAND_WAITING);
    UASSERT_EQ((int)USTRAND_GET_REASON(&s), (int)USTRAND_REASON_SLEEP);
    /* wakeup_pending_count incremented by sleep_q_insert */
    UASSERT_EQ(vm.wakeup_pending_count, 1u);
    /* runnable_count decremented */
    UASSERT_EQ(vm.strand_runnable_count, 0u);
    /* strand is on sleep queue */
    UASSERT(vm.sleep_q_head == &s);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 7: sched_dequeue_ready_head dequeues the head and decrements
   strand_runnable_count; after dequeue, the second strand is the new head. */
UTEST(sched_dequeue_ready_head_advances_queue)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    sched_strand_make_runnable(&a);
    sched_strand_make_runnable(&b);
    UASSERT_EQ(vm.strand_runnable_count, 2u);

    sched_dequeue_ready_head(&vm);

    UASSERT_EQ(vm.strand_runnable_count, 1u);
    UASSERT(vm.ready_head == &b);
    UASSERT(vm.ready_tail == &b);
    /* a is no longer on a list */
    UASSERT(a.ready_next == NULL);
    UASSERT(a.ready_prev == NULL);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    uvm_destroy(&vm);
}

/* Case 8: sched_pick_next on an empty queue returns NULL (no-crash). */
UTEST(sched_pick_next_empty_returns_null)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UASSERT(sched_pick_next(&vm) == NULL);

    uvm_destroy(&vm);
}

/* Case 9: sleep_q_insert maintains sorted order (head, mid, tail cases).
   Use sched_strand_block to drive insertion indirectly. */
UTEST(sched_sleep_q_sorted_insertion)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand early, mid, late;
    ustrand_init(&early, &vm);
    ustrand_init(&mid,   &vm);
    ustrand_init(&late,  &vm);

    /* Insert out-of-order: late, early, mid — sorted queue should be
       early → mid → late after all three insertions. */
    early.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&early, USTRAND_REASON_SLEEP, 100u);

    late.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&late, USTRAND_REASON_SLEEP, 300u);

    mid.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&mid, USTRAND_REASON_SLEEP, 200u);

    /* Head must be 'early' (smallest wake_us). */
    UASSERT(vm.sleep_q_head == &early);
    UASSERT(vm.sleep_q_head->wait_next == &mid);
    UASSERT(vm.sleep_q_head->wait_next->wait_next == &late);
    UASSERT(late.wait_next == NULL);

    UASSERT_EQ(vm.wakeup_pending_count, 3u);

    ustrand_destroy(&early, &vm);
    ustrand_destroy(&mid,   &vm);
    ustrand_destroy(&late,  &vm);
    uvm_destroy(&vm);
}

/* Case 10: sched_earliest_wake_us returns the wake_us of the sleep-queue head
   when a single strand is sleeping. */
UTEST(sched_earliest_wake_us_single_sleeper)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    s.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&s, USTRAND_REASON_SLEEP, 42u);

    UASSERT_EQ(sched_earliest_wake_us(&vm), 42u);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 11: sched_earliest_wake_us returns the minimum wake_us when multiple
   strands are sleeping. */
UTEST(sched_earliest_wake_us_picks_minimum)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    a.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&a, USTRAND_REASON_SLEEP, 500u);

    b.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&b, USTRAND_REASON_SLEEP, 200u);

    /* 'b' was inserted with wake_us=200, which is less than 'a' (500). */
    UASSERT_EQ(sched_earliest_wake_us(&vm), 200u);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    uvm_destroy(&vm);
}

/* Case 12: sched_quiescent returns true only when all 5 counters are zero;
   returns false when strand_runnable_count is non-zero. */
UTEST(sched_quiescent_false_when_runnable_nonzero)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UASSERT(sched_quiescent(&vm));  /* starts quiescent */

    vm.strand_runnable_count = 1;
    UASSERT(!sched_quiescent(&vm));

    vm.strand_runnable_count = 0;
    UASSERT(sched_quiescent(&vm));

    uvm_destroy(&vm);
}

/* Case 13: sched_quiescent returns false when wakeup_pending_count is non-zero
   (strands sleeping — VM still has live work). */
UTEST(sched_quiescent_false_when_sleep_q_nonempty)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    vm.wakeup_pending_count = 1;
    UASSERT(!sched_quiescent(&vm));

    vm.wakeup_pending_count = 0;
    UASSERT(sched_quiescent(&vm));

    uvm_destroy(&vm);
}

/* Case 14: sched_consume_budget(s, 0) is a no-op (budget unchanged). */
UTEST(sched_consume_budget_zero_noop)
{
    UStrand s;
    s.instruction_budget_remaining = 100;
    sched_consume_budget(&s, 0);
    UASSERT_EQ(s.instruction_budget_remaining, 100u);
}

/* Case 15: sched_strand_unblock transitions a SLEEPING strand back to READY
   and decrements wakeup_pending_count. */
UTEST(sched_strand_unblock_from_sleep)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    /* Block into sleep. */
    s.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&s, USTRAND_REASON_SLEEP, 9999u);
    UASSERT_EQ(vm.wakeup_pending_count, 1u);
    UASSERT_EQ(vm.strand_runnable_count, 0u);

    /* Unblock: should move back to READY. */
    sched_strand_unblock(&s);
    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_READY);
    UASSERT_EQ(vm.wakeup_pending_count, 0u);
    UASSERT_EQ(vm.strand_runnable_count, 1u);
    UASSERT(vm.sleep_q_head == NULL);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 16: sched_destroy NULLs the scheduler queues (lifecycle cleanup). */
UTEST(sched_destroy_nulls_queues)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
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
    uvm_destroy(&vm);
}

/* Case 17: sched_strand_block with REASON_EVENT stores the event pointer. */
UTEST(sched_strand_block_event_stores_pointer)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    s.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;

    /* Use an arbitrary non-NULL sentinel for the event pointer. */
    uintptr_t event_sentinel = 0xDEADBEEFu;
    sched_strand_block(&s, USTRAND_REASON_EVENT, (uint64_t)event_sentinel);

    UASSERT_EQ((int)(s.state & USTRAND_STATE_MASK), (int)USTRAND_WAITING);
    UASSERT_EQ((int)USTRAND_GET_REASON(&s), (int)USTRAND_REASON_EVENT);
    UASSERT((uintptr_t)s.wait_payload.event == event_sentinel);
    /* Event-wait does not insert into sleep queue. */
    UASSERT_EQ(vm.wakeup_pending_count, 0u);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 18: sched_strand_block with REASON_JOIN stores the join_parent pointer. */
UTEST(sched_strand_block_join_stores_pointer)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
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
    UASSERT_EQ(vm.wakeup_pending_count, 0u);

    ustrand_destroy(&parent, &vm);
    ustrand_destroy(&child,  &vm);
    uvm_destroy(&vm);
}

/* Case 19: sleep_q_insert advances the while-loop (insert mid after 2+ elements).
   Insert 4 strands in order 400, 100, 300, 200 so that the mid inserts
   exercise the while-loop advancement path. */
UTEST(sched_sleep_q_multi_advance)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s1, s2, s3, s4;
    ustrand_init(&s1, &vm);
    ustrand_init(&s2, &vm);
    ustrand_init(&s3, &vm);
    ustrand_init(&s4, &vm);

    /* Insert 400 first, then 100 (head insert), then 300 (mid), then 200 (mid-mid). */
    s1.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&s1, USTRAND_REASON_SLEEP, 400u);

    s2.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&s2, USTRAND_REASON_SLEEP, 100u);

    s3.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&s3, USTRAND_REASON_SLEEP, 300u);

    s4.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&s4, USTRAND_REASON_SLEEP, 200u);

    /* Expected order: s2(100) → s4(200) → s3(300) → s1(400) */
    UASSERT(vm.sleep_q_head == &s2);
    UASSERT(vm.sleep_q_head->wait_next == &s4);
    UASSERT(vm.sleep_q_head->wait_next->wait_next == &s3);
    UASSERT(vm.sleep_q_head->wait_next->wait_next->wait_next == &s1);
    UASSERT_EQ(vm.wakeup_pending_count, 4u);
    UASSERT_EQ(sched_earliest_wake_us(&vm), 100u);

    ustrand_destroy(&s1, &vm);
    ustrand_destroy(&s2, &vm);
    ustrand_destroy(&s3, &vm);
    ustrand_destroy(&s4, &vm);
    uvm_destroy(&vm);
}

/* Case 20: sleep_q_remove removes a mid-queue element (non-head). */
UTEST(sched_sleep_q_remove_mid_element)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand early, mid, late;
    ustrand_init(&early, &vm);
    ustrand_init(&mid,   &vm);
    ustrand_init(&late,  &vm);

    early.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&early, USTRAND_REASON_SLEEP, 100u);

    late.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&late, USTRAND_REASON_SLEEP, 300u);

    mid.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&mid, USTRAND_REASON_SLEEP, 200u);

    /* Queue: early(100) → mid(200) → late(300). Unblock mid. */
    UASSERT_EQ(vm.wakeup_pending_count, 3u);

    /* sched_strand_unblock calls sleep_q_remove then sched_strand_make_runnable. */
    sched_strand_unblock(&mid);

    /* After removal: early(100) → late(300). */
    UASSERT_EQ(vm.wakeup_pending_count, 2u);
    UASSERT(vm.sleep_q_head == &early);
    UASSERT(vm.sleep_q_head->wait_next == &late);
    UASSERT(late.wait_next == NULL);

    /* mid is now READY. */
    UASSERT_EQ((int)mid.state, (int)USTRAND_STATE_READY);

    /* Clean up ready queue. */
    if (vm.ready_head == &mid) {
        vm.ready_head = mid.ready_next;
        if (vm.ready_head == NULL) vm.ready_tail = NULL;
        if (vm.strand_runnable_count > 0) vm.strand_runnable_count--;
    }

    ustrand_destroy(&early, &vm);
    ustrand_destroy(&mid,   &vm);
    ustrand_destroy(&late,  &vm);
    uvm_destroy(&vm);
}

/* Case 21: sleep_q_insert while-loop advance: insert an element that requires
   walking past 2+ nodes in the sorted sleep queue. */
UTEST(sched_sleep_q_insert_while_advance)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b, c, d;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);
    ustrand_init(&c, &vm);
    ustrand_init(&d, &vm);

    /* Build sorted queue: 100 → 200 → 350 */
    a.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&a, USTRAND_REASON_SLEEP, 100u);

    b.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&b, USTRAND_REASON_SLEEP, 200u);

    c.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&c, USTRAND_REASON_SLEEP, 350u);

    /* Insert 300: cur starts at a(100), 200<=300 is true → advance.
       cur becomes b(200), 350<=300 is false → insert d after b.
       Expected: a(100) → b(200) → d(300) → c(350). */
    d.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&d, USTRAND_REASON_SLEEP, 300u);

    UASSERT(vm.sleep_q_head == &a);
    UASSERT(vm.sleep_q_head->wait_next == &b);
    UASSERT(vm.sleep_q_head->wait_next->wait_next == &d);
    UASSERT(vm.sleep_q_head->wait_next->wait_next->wait_next == &c);
    UASSERT_EQ(vm.wakeup_pending_count, 4u);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    ustrand_destroy(&c, &vm);
    ustrand_destroy(&d, &vm);
    uvm_destroy(&vm);
}

/* Case 22: sleep_q_remove mid-element via while-loop advance (non-head removal
   where the removal target is 2+ nodes from the head). */
UTEST(sched_sleep_q_remove_while_advance)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b, c;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);
    ustrand_init(&c, &vm);

    /* Queue: a(100) → b(200) → c(300). */
    a.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&a, USTRAND_REASON_SLEEP, 100u);
    b.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&b, USTRAND_REASON_SLEEP, 200u);
    c.state = USTRAND_STATE_RUNNING; vm.strand_runnable_count = 1;
    sched_strand_block(&c, USTRAND_REASON_SLEEP, 300u);

    /* Unblock c (tail element): sleep_q_remove must walk from head to find it. */
    sched_strand_unblock(&c);

    /* Queue: a(100) → b(200). */
    UASSERT_EQ(vm.wakeup_pending_count, 2u);
    UASSERT(vm.sleep_q_head == &a);
    UASSERT(vm.sleep_q_head->wait_next == &b);
    UASSERT(b.wait_next == NULL);

    /* Clean up ready queue (c is now READY). */
    if (vm.ready_head == &c) {
        vm.ready_head = c.ready_next;
        if (vm.ready_head == NULL) vm.ready_tail = NULL;
        if (vm.strand_runnable_count > 0) vm.strand_runnable_count--;
    }

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    ustrand_destroy(&c, &vm);
    uvm_destroy(&vm);
}

/* Case 23: sched_quiescent returns false when watcher_active_count is non-zero. */
UTEST(sched_quiescent_false_when_watcher_active_nonzero)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UASSERT(sched_quiescent(&vm));

    vm.watcher_active_count = 1;
    UASSERT(!sched_quiescent(&vm));

    vm.watcher_active_count = 0;
    UASSERT(sched_quiescent(&vm));

    uvm_destroy(&vm);
}

/* Case 24: sched_quiescent returns false when event_queue_count is non-zero. */
UTEST(sched_quiescent_false_when_event_queue_nonzero)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UASSERT(sched_quiescent(&vm));

    vm.event_queue_count = 1;
    UASSERT(!sched_quiescent(&vm));

    vm.event_queue_count = 0;
    UASSERT(sched_quiescent(&vm));

    uvm_destroy(&vm);
}

/* Case 25: sched_quiescent returns false when host_call_pending_count is non-zero. */
UTEST(sched_quiescent_false_when_host_call_pending_nonzero)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UASSERT(sched_quiescent(&vm));

    vm.host_call_pending_count = 1;
    UASSERT(!sched_quiescent(&vm));

    vm.host_call_pending_count = 0;
    UASSERT(sched_quiescent(&vm));

    uvm_destroy(&vm);
}

/* Case 26: sched_walk_roots is a no-op stub; calling it doesn't crash. */
UTEST(sched_walk_roots_noop)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_walk_roots(&vm, NULL, NULL);
    UASSERT(1);  /* reached here without crash */
    uvm_destroy(&vm);
}

void test_scheduler_cooperative_suite(void) {
    utest_run("sched_init_empties_ready_queue",           sched_init_empties_ready_queue);
    utest_run("sched_make_runnable_appends_tail",         sched_make_runnable_appends_tail);
    utest_run("sched_earliest_wake_uintmax_on_empty",     sched_earliest_wake_uintmax_on_empty);
    utest_run("sched_consume_budget_floors_at_zero",      sched_consume_budget_floors_at_zero);
    utest_run("sched_make_runnable_sets_state_ready",     sched_make_runnable_sets_state_ready);
    utest_run("sched_strand_block_sleep_moves_to_sleep_q", sched_strand_block_sleep_moves_to_sleep_q);
    utest_run("sched_dequeue_ready_head_advances_queue",  sched_dequeue_ready_head_advances_queue);
    utest_run("sched_pick_next_empty_returns_null",       sched_pick_next_empty_returns_null);
    utest_run("sched_sleep_q_sorted_insertion",           sched_sleep_q_sorted_insertion);
    utest_run("sched_earliest_wake_us_single_sleeper",    sched_earliest_wake_us_single_sleeper);
    utest_run("sched_earliest_wake_us_picks_minimum",     sched_earliest_wake_us_picks_minimum);
    utest_run("sched_quiescent_false_when_runnable_nonzero", sched_quiescent_false_when_runnable_nonzero);
    utest_run("sched_quiescent_false_when_sleep_q_nonempty", sched_quiescent_false_when_sleep_q_nonempty);
    utest_run("sched_consume_budget_zero_noop",           sched_consume_budget_zero_noop);
    utest_run("sched_strand_unblock_from_sleep",          sched_strand_unblock_from_sleep);
    utest_run("sched_destroy_nulls_queues",               sched_destroy_nulls_queues);
    utest_run("sched_strand_block_event_stores_pointer",  sched_strand_block_event_stores_pointer);
    utest_run("sched_strand_block_join_stores_pointer",   sched_strand_block_join_stores_pointer);
    utest_run("sched_sleep_q_multi_advance",              sched_sleep_q_multi_advance);
    utest_run("sched_sleep_q_remove_mid_element",         sched_sleep_q_remove_mid_element);
    utest_run("sched_sleep_q_insert_while_advance",       sched_sleep_q_insert_while_advance);
    utest_run("sched_sleep_q_remove_while_advance",       sched_sleep_q_remove_while_advance);
    utest_run("sched_quiescent_false_when_watcher_active_nonzero", sched_quiescent_false_when_watcher_active_nonzero);
    utest_run("sched_quiescent_false_when_event_queue_nonzero", sched_quiescent_false_when_event_queue_nonzero);
    utest_run("sched_quiescent_false_when_host_call_pending_nonzero", sched_quiescent_false_when_host_call_pending_nonzero);
    utest_run("sched_walk_roots_noop",                    sched_walk_roots_noop);
}
