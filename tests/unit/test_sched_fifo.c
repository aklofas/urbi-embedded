/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: all 5 DORMANT/WAITING → READY transitions use
 * sched_strand_make_runnable (single tail-insertion entry point per
 * row 12 §3.4).
 *
 * The five transitions:
 *   (1) DORMANT-start:        ustrand_init → sched_strand_make_runnable
 *   (2) fork-spawn:           urbi_strand_start (DORMANT → READY)
 *   (3) cooperative-yield:    sched_strand_yield (RUNNING → READY re-enqueue)
 *   (4) WAITING-unblock:      sched_strand_unblock (WAITING_SLEEP → READY)
 *   (5) watcher-body-spawn:   sched_strand_make_runnable called directly
 *                             (simulates what spawn_body_coroutine will do at M5)
 *
 * All five go through sched_strand_make_runnable, which is the single
 * tail-insertion path: s → ready_tail → new tail. */

#include "utest.h"
#include "usched_cooperative.h"
#include "uvm.h"
#include "ustrand.h"
#include "urbi.h"
#include "urealm.h"

#define UTEST(name) static void name(void)

/* Helper: assert strand is at the tail of the ready queue. */
static void assert_at_tail(UVM *vm, UStrand *s)
{
    UASSERT(vm->ready_tail == s);
    UASSERT(s->ready_next == NULL);
}

/* === Transition 1: DORMANT-start via sched_strand_make_runnable directly === */

UTEST(fifo_transition1_dormant_make_runnable_appends_tail)
{
    /* Verify DORMANT → READY goes to the tail via sched_strand_make_runnable. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    sched_strand_make_runnable(&a);
    UASSERT(vm.ready_head == &a);
    UASSERT_EQ(vm.strand_runnable_count, 1u);
    assert_at_tail(&vm, &a);

    sched_strand_make_runnable(&b);
    /* b appended after a */
    UASSERT(vm.ready_head == &a);
    UASSERT(vm.ready_tail == &b);
    UASSERT_EQ(vm.strand_runnable_count, 2u);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    uvm_destroy(&vm);
}

/* === Transition 2: fork-spawn via urbi_strand_start === */

UTEST(fifo_transition2_strand_start_goes_to_tail)
{
    /* urbi_strand_start calls sched_strand_make_runnable (the single entry point).
     * Verify the spawned strand lands at the queue tail behind an existing strand. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* Create a first strand and make it runnable manually. */
    UStrand *first = urbi_strand_create(realm, NULL);
    UASSERT(first != NULL);
    sched_strand_make_runnable(first);
    UASSERT(vm.ready_head == first);

    /* Fork-spawn the second strand via the public API. */
    UStrand *second = urbi_strand_create(realm, NULL);
    UASSERT(second != NULL);
    urbi_strand_start(second);  /* DORMANT → READY via sched_strand_make_runnable */

    /* second must be at the tail, first still at the head. */
    UASSERT(vm.ready_head == first);
    UASSERT(vm.ready_tail == second);
    UASSERT_EQ(vm.strand_runnable_count, 2u);

    urbi_strand_destroy(second);
    urbi_strand_destroy(first);
    urbi_realm_destroy(&vm, realm);
    uvm_destroy(&vm);
}

/* === Transition 3: cooperative-yield re-enqueues at tail === */

UTEST(fifo_transition3_yield_appends_tail)
{
    /* sched_strand_yield is called for RUNNING → READY (soft budget exhaust
     * or explicit OP_YIELD).  Verify the yielded strand goes to the tail. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    /* Enqueue both: head=a, tail=b. */
    sched_strand_make_runnable(&a);
    sched_strand_make_runnable(&b);
    UASSERT(vm.ready_head == &a);
    UASSERT(vm.ready_tail == &b);

    /* Simulate dispatch: dequeue a, set RUNNING. */
    sched_dequeue_ready_head(&vm);
    a.state = USTRAND_STATE_RUNNING;
    UASSERT_EQ(vm.strand_runnable_count, 1u);

    /* a yields: re-enqueues at tail behind b. */
    sched_strand_yield(&a);
    UASSERT(vm.ready_head == &b);
    UASSERT(vm.ready_tail == &a);
    UASSERT_EQ(vm.strand_runnable_count, 2u);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    uvm_destroy(&vm);
}

/* === Transition 4: WAITING-unblock goes to tail === */

UTEST(fifo_transition4_unblock_appends_tail)
{
    /* sched_strand_unblock transitions WAITING_SLEEP → READY via make_runnable.
     * To correctly set up the counter invariant, we enqueue a first, then
     * simulate dispatch (dequeue + set RUNNING) before blocking on sleep. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    /* Enqueue both: head=a, tail=b. */
    sched_strand_make_runnable(&a);
    sched_strand_make_runnable(&b);
    UASSERT_EQ(vm.strand_runnable_count, 2u);
    UASSERT(vm.ready_head == &a);

    /* Simulate dispatch: dequeue a, set RUNNING.
     * sched_dequeue_ready_head decrements count (2 → 1). */
    sched_dequeue_ready_head(&vm);
    a.state = USTRAND_STATE_RUNNING;
    UASSERT_EQ(vm.strand_runnable_count, 1u);
    UASSERT(vm.ready_head == &b);

    /* a blocks on sleep (RUNNING → WAITING).  sched_strand_block decrements
     * count again because it sees state==RUNNING: count goes 1 → 0. */
    sched_strand_block(&a, USTRAND_REASON_SLEEP, 999999u);
    UASSERT(USTRAND_GET_STATE(&a) == USTRAND_WAITING);
    UASSERT_EQ(vm.strand_runnable_count, 0u);
    UASSERT_EQ(vm.wakeup_pending_count, 1u);

    /* ustep.c re-increments for WAITING transitions during dispatch; simulate
     * that here to restore the invariant (count should account for a being
     * live-but-waiting). */
    vm.strand_runnable_count++;  /* mirrors ustep.c re-increment after WAITING */
    UASSERT_EQ(vm.strand_runnable_count, 1u);

    /* Unblock a: sleep_q_remove (wakeup_pending 1→0) + make_runnable (count 1→2).
     * a must go to the tail behind b. */
    sched_strand_unblock(&a);
    UASSERT(vm.ready_head == &b);
    UASSERT(vm.ready_tail == &a);
    UASSERT_EQ(vm.wakeup_pending_count, 0u);
    UASSERT_EQ(vm.strand_runnable_count, 2u);

    sched_dequeue_ready_head(&vm);  /* drain b */
    sched_dequeue_ready_head(&vm);  /* drain a */

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    uvm_destroy(&vm);
}

/* === Transition 5: watcher-body-spawn goes to tail ===
 *
 * At M5, spawn_body_coroutine calls sched_strand_make_runnable for the new
 * watcher body strand.  Simulate this at M3 by calling make_runnable directly
 * (the same path spawn_body_coroutine takes) and verify tail insertion. */

UTEST(fifo_transition5_watcher_body_spawn_appends_tail)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand parent, watcher_body;
    ustrand_init(&parent, &vm);
    ustrand_init(&watcher_body, &vm);

    /* Parent is running. */
    sched_strand_make_runnable(&parent);
    sched_dequeue_ready_head(&vm);
    parent.state = USTRAND_STATE_RUNNING;
    UASSERT_EQ(vm.strand_runnable_count, 0u);

    /* Watcher body spawned while parent is running: goes to tail. */
    sched_strand_make_runnable(&watcher_body);
    UASSERT(vm.ready_head == &watcher_body);
    UASSERT(vm.ready_tail == &watcher_body);
    UASSERT_EQ(vm.strand_runnable_count, 1u);

    /* Re-make parent runnable (yield): appends after watcher body. */
    sched_strand_yield(&parent);
    UASSERT(vm.ready_head == &watcher_body);
    UASSERT(vm.ready_tail == &parent);
    UASSERT_EQ(vm.strand_runnable_count, 2u);

    ustrand_destroy(&parent, &vm);
    ustrand_destroy(&watcher_body, &vm);
    uvm_destroy(&vm);
}

/* === Suite registration === */

void test_sched_fifo_suite(void)
{
    utest_run("fifo_transition1_dormant_make_runnable_appends_tail",
              fifo_transition1_dormant_make_runnable_appends_tail);
    utest_run("fifo_transition2_strand_start_goes_to_tail",
              fifo_transition2_strand_start_goes_to_tail);
    utest_run("fifo_transition3_yield_appends_tail",
              fifo_transition3_yield_appends_tail);
    utest_run("fifo_transition4_unblock_appends_tail",
              fifo_transition4_unblock_appends_tail);
    utest_run("fifo_transition5_watcher_body_spawn_appends_tail",
              fifo_transition5_watcher_body_spawn_appends_tail);
}
