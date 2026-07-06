/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: all 5 DORMANT/WAITING → READY transitions use
 * urbi_sched_strand_make_runnable (single tail-insertion entry point per
 * row 12 §3.4).
 *
 * The five transitions:
 *   (1) DORMANT-start:        ustrand_init → urbi_sched_strand_make_runnable
 *   (2) fork-spawn:           urbi_strand_start (DORMANT → READY)
 *   (3) cooperative-yield:    urbi_sched_strand_yield (RUNNING → READY re-enqueue)
 *   (4) WAITING-unblock:      urbi_sched_strand_unblock (WAITING_SLEEP → READY)
 *   (5) watcher-body-spawn:   urbi_sched_strand_make_runnable called directly
 *                             (simulates what urbi_watcher_spawn_body_coroutine will do at M5)
 *
 * All five go through urbi_sched_strand_make_runnable, which is the single
 * tail-insertion path: s → ready_tail → new tail. */

#include "utest.h"
#include "sched/usched_cooperative.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "urbi/urbi.h"
#include "realm/urealm.h"

#define UTEST(name) static void name(void)

/* Helper: assert strand is at the tail of the ready queue. */
static void assert_at_tail(UVM *vm, UStrand *s)
{
    UASSERT(vm->ready_tail == s);
    UASSERT(s->ready_next == NULL);
}

/* === Transition 1: DORMANT-start via urbi_sched_strand_make_runnable directly === */

UTEST(fifo_transition1_dormant_make_runnable_appends_tail)
{
    /* Verify DORMANT → READY goes to the tail via urbi_sched_strand_make_runnable. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    urbi_sched_strand_make_runnable(&a);
    UASSERT(vm.ready_head == &a);
    UASSERT_EQ(vm.strand_runnable_count, 1U);
    assert_at_tail(&vm, &a);

    urbi_sched_strand_make_runnable(&b);
    /* b appended after a */
    UASSERT(vm.ready_head == &a);
    UASSERT(vm.ready_tail == &b);
    UASSERT_EQ(vm.strand_runnable_count, 2U);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    urbi_vm_destroy(&vm);
}

/* === Transition 2: fork-spawn via urbi_strand_start === */

UTEST(fifo_transition2_strand_start_goes_to_tail)
{
    /* urbi_strand_start calls urbi_sched_strand_make_runnable (the single entry point).
     * Verify the spawned strand lands at the queue tail behind an existing strand. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* Create a first strand and make it runnable manually. */
    UStrand *first = urbi_strand_create(&vm, realm, NULL);
    UASSERT(first != NULL);
    urbi_sched_strand_make_runnable(first);
    UASSERT(vm.ready_head == first);

    /* Fork-spawn the second strand via the public API. */
    UStrand *second = urbi_strand_create(&vm, realm, NULL);
    UASSERT(second != NULL);
    urbi_strand_start(&vm, second);  /* DORMANT → READY via urbi_sched_strand_make_runnable */

    /* second must be at the tail, first still at the head. */
    UASSERT(vm.ready_head == first);
    UASSERT(vm.ready_tail == second);
    UASSERT_EQ(vm.strand_runnable_count, 2U);

    urbi_strand_destroy(&vm, second);
    urbi_strand_destroy(&vm, first);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* === Transition 3: cooperative-yield re-enqueues at tail === */

UTEST(fifo_transition3_yield_appends_tail)
{
    /* urbi_sched_strand_yield is called for RUNNING → READY (soft budget exhaust
     * or explicit OP_YIELD).  Verify the yielded strand goes to the tail. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    /* Enqueue both: head=a, tail=b. */
    urbi_sched_strand_make_runnable(&a);
    urbi_sched_strand_make_runnable(&b);
    UASSERT(vm.ready_head == &a);
    UASSERT(vm.ready_tail == &b);

    /* Simulate dispatch: dequeue a, set RUNNING.  SCHED-01: count-neutral
     * (READY -> RUNNING stays inside the counted set). */
    urbi_sched_dequeue_ready_head(&vm);
    a.state = USTRAND_STATE_RUNNING;
    UASSERT_EQ(vm.strand_runnable_count, 2U);

    /* a yields: re-enqueues at tail behind b — count-neutral too. */
    urbi_sched_strand_yield(&a);
    UASSERT(vm.ready_head == &b);
    UASSERT(vm.ready_tail == &a);
    UASSERT_EQ(vm.strand_runnable_count, 2U);

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    urbi_vm_destroy(&vm);
}

/* === Transition 4: WAITING-unblock goes to tail === */

UTEST(fifo_transition4_unblock_appends_tail)
{
    /* urbi_sched_strand_unblock transitions WAITING_SLEEP → READY via make_runnable.
     * To correctly set up the counter invariant, we enqueue a first, then
     * simulate dispatch (dequeue + set RUNNING) before blocking on sleep. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_sched_init(&vm, NULL);

    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    /* Enqueue both: head=a, tail=b. */
    urbi_sched_strand_make_runnable(&a);
    urbi_sched_strand_make_runnable(&b);
    UASSERT_EQ(vm.strand_runnable_count, 2U);
    UASSERT(vm.ready_head == &a);

    /* Simulate dispatch: dequeue a, set RUNNING.  SCHED-01: count-neutral
     * (READY -> RUNNING stays inside the counted set). */
    urbi_sched_dequeue_ready_head(&vm);
    a.state = USTRAND_STATE_RUNNING;
    UASSERT_EQ(vm.strand_runnable_count, 2U);
    UASSERT(vm.ready_head == &b);

    /* a blocks on sleep (RUNNING → WAITING): the single decrement site.
     * WAITING strands are NOT counted under SCHED-01 (the pre-refactor
     * ustep.c WAITING re-increment is gone). */
    urbi_sched_strand_block(&a, USTRAND_REASON_SLEEP, 999999U);
    UASSERT(USTRAND_GET_STATE(&a) == USTRAND_WAITING);
    UASSERT_EQ(vm.strand_runnable_count, 1U);
    UASSERT_EQ(vm.wakeup_pending_count, 1U);

    /* Unblock a: sleep_q_remove (wakeup_pending 1→0) + make_runnable (count 1→2).
     * a must go to the tail behind b. */
    urbi_sched_strand_unblock(&a);
    UASSERT(vm.ready_head == &b);
    UASSERT(vm.ready_tail == &a);
    UASSERT_EQ(vm.wakeup_pending_count, 0U);
    UASSERT_EQ(vm.strand_runnable_count, 2U);

    urbi_sched_dequeue_ready_head(&vm);  /* drain b */
    urbi_sched_dequeue_ready_head(&vm);  /* drain a */

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    urbi_vm_destroy(&vm);
}

/* === Transition 5: watcher-body-spawn goes to tail ===
 *
 * At M5, urbi_watcher_spawn_body_coroutine calls urbi_sched_strand_make_runnable for the new
 * watcher body strand.  Simulate this at M3 by calling make_runnable directly
 * (the same path urbi_watcher_spawn_body_coroutine takes) and verify tail insertion. */

UTEST(fifo_transition5_watcher_body_spawn_appends_tail)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_sched_init(&vm, NULL);

    UStrand parent, watcher_body;
    ustrand_init(&parent, &vm);
    ustrand_init(&watcher_body, &vm);

    /* Parent is running.  SCHED-01: dequeue is count-neutral (the RUNNING
     * parent stays in the counted set). */
    urbi_sched_strand_make_runnable(&parent);
    urbi_sched_dequeue_ready_head(&vm);
    parent.state = USTRAND_STATE_RUNNING;
    UASSERT_EQ(vm.strand_runnable_count, 1U);

    /* Watcher body spawned while parent is running: goes to tail. */
    urbi_sched_strand_make_runnable(&watcher_body);
    UASSERT(vm.ready_head == &watcher_body);
    UASSERT(vm.ready_tail == &watcher_body);
    UASSERT_EQ(vm.strand_runnable_count, 2U);

    /* Re-make parent runnable (yield): appends after watcher body.
     * Count-neutral (RUNNING -> READY). */
    urbi_sched_strand_yield(&parent);
    UASSERT(vm.ready_head == &watcher_body);
    UASSERT(vm.ready_tail == &parent);
    UASSERT_EQ(vm.strand_runnable_count, 2U);

    ustrand_destroy(&parent, &vm);
    ustrand_destroy(&watcher_body, &vm);
    urbi_vm_destroy(&vm);
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
