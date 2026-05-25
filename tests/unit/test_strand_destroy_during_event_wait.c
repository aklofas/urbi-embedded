/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: ustrand_destroy unregisters event waiters + wakes joiners.
 *
 * Scheduler F1: ustrand_destroy (called from the realm-destroy sweep and any
 * direct urbi_strand_destroy call) did not previously splice the strand out of
 * UEvent.waiters_head or wake join-blocked parents.  Only the dispatch-loop
 * path through exit_strand in uvm.c performed those steps.  A realm-destroy
 * that hit a strand parked on a waituntil() left a dangling pointer in
 * UEvent.waiters_head; a realm-destroy that hit a strand whose child was
 * joined left the joiner permanently WAITING_JOIN.
 *
 * This file tests the convergence fix: strand_cleanup_observers is called by
 * ustrand_destroy so EVERY teardown path has identical external-reference
 * cleanup.
 *
 * Tests:
 *
 *   1. destroy_clears_event_waiter_head:
 *      Park a strand on a UEvent via c_event_waituntil.  Call
 *      urbi_strand_destroy.  Assert UEvent.waiters_head == NULL.
 *
 *   2. destroy_clears_event_waiter_mid_chain:
 *      Park two strands on the same UEvent.  Destroy only the first-parked
 *      (middle of the chain).  Assert the second strand remains on
 *      waiters_head, and the first is gone.
 *
 *   3. destroy_wakes_joiner:
 *      Create a parent strand, manually set it as a joiner on a child strand's
 *      joiners_head, put the parent in WAITING state.  Destroy the child via
 *      urbi_strand_destroy.  Assert the parent has been made READY (runnable
 *      count incremented) and child's joiners_head is NULL. */

#include "utest.h"

#include "event/uevent.h"
#include "event/uevent_emit.h"
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "urbi/urbi.h"

#include <stddef.h>
#include <string.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Case 1: destroy_clears_event_waiter_head
 *
 * Park a strand on a UEvent, then call urbi_strand_destroy.
 * After destroy, UEvent.waiters_head must be NULL — no dangling pointer.
 * =================================================================== */

UTEST(destroy_clears_event_waiter_head)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UEvent *ev = urbi_event_create(&vm);
    UASSERT(ev != NULL);

    /* Allocate a strand that belongs to the realm. */
    UStrand *s = urbi_strand_create(&vm, realm, NULL);
    UASSERT(s != NULL);

    /* Arm the strand so sched_strand_block's decrement does not underflow. */
    s->state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;
    vm.cur_strand = s;

    /* Park the strand on the event (simulates waituntil(ev?)). */
    c_event_waituntil(&vm, ev);

    /* Strand must now be on the waiter chain. */
    UASSERT(ev->waiters_head == s);
    UASSERT(s->wait_event_target == ev);

    vm.cur_strand = NULL;

    /* Destroy the strand directly (the realm-destroy path, but one strand). */
    urbi_strand_destroy(&vm, s);

    /* The event's waiter chain must now be empty — no dangling pointer. */
    UASSERT(ev->waiters_head == NULL);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 2: destroy_clears_event_waiter_mid_chain
 *
 * Park two strands on the same UEvent.  Destroy the first-parked strand
 * (which ends up at the tail after head-insert / FIFO tail-append semantics).
 * Assert the second strand is still on the chain and the first is gone.
 *
 * Note: c_event_waituntil tail-appends to waiters_head, so:
 *   - s1 parked first → s1 is at the head.
 *   - s2 parked second → s2 is at the tail (s1->next_event_waiter == s2).
 * =================================================================== */

UTEST(destroy_clears_event_waiter_mid_chain)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UEvent *ev = urbi_event_create(&vm);
    UASSERT(ev != NULL);

    /* Strand 1. */
    UStrand *s1 = urbi_strand_create(&vm, realm, NULL);
    UASSERT(s1 != NULL);
    s1->state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;
    vm.cur_strand = s1;
    c_event_waituntil(&vm, ev);
    vm.cur_strand = NULL;

    /* Strand 2. */
    UStrand *s2 = urbi_strand_create(&vm, realm, NULL);
    UASSERT(s2 != NULL);
    s2->state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;
    vm.cur_strand = s2;
    c_event_waituntil(&vm, ev);
    vm.cur_strand = NULL;

    /* Chain must be s1 → s2. */
    UASSERT(ev->waiters_head == s1);
    UASSERT(s1->next_event_waiter == s2);
    UASSERT(s2->next_event_waiter == NULL);

    /* Destroy s1 (head of chain). */
    urbi_strand_destroy(&vm, s1);

    /* s2 must still be on the chain; s1 gone. */
    UASSERT(ev->waiters_head == s2);
    UASSERT(s2->wait_event_target == ev);
    UASSERT(s2->next_event_waiter == NULL);

    /* Cleanup: destroy s2 normally. */
    urbi_strand_destroy(&vm, s2);
    UASSERT(ev->waiters_head == NULL);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 3: destroy_wakes_joiner
 *
 * Manually construct a joiner relationship: a "parent" strand (joiner) is
 * on the child's joiners_head, in WAITING_JOIN state.  Destroy the child.
 * Assert the parent is woken (READY / strand_runnable_count incremented)
 * and child->joiners_head is NULL.
 *
 * This tests the fork_wake_joiners call added to strand_cleanup_observers.
 * We bypass the VM bytecode (no need to compile & run OP_FORK_JOIN /
 * OP_JOIN_WAIT) because we are testing the teardown path, not the opcode.
 * =================================================================== */

UTEST(destroy_wakes_joiner)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Child strand — will be destroyed. */
    UStrand *child = urbi_strand_create(&vm, realm, NULL);
    UASSERT(child != NULL);

    /* Parent (joiner) strand — manually placed on child->joiners_head. */
    UStrand *parent = urbi_strand_create(&vm, realm, NULL);
    UASSERT(parent != NULL);

    /* Put the parent in WAITING_JOIN state (mirrors what OP_JOIN_WAIT does).
     * sched_strand_block would decrement strand_runnable_count; simulate by
     * pre-setting state and bumping the counter so runnable_count stays
     * non-negative after the wake-driven increment. */
    parent->state = USTRAND_STATE_WAITING_JOIN;
    /* strand_runnable_count starts at 0 (after create).  fork_wake_joiners
     * calls sched_strand_make_runnable which increments it by 1.  We leave
     * it at 0 here to confirm the increment happens in the wake call. */
    vm.strand_runnable_count = 0;

    /* Thread parent onto child's joiners_head (mirrors op_join_wait internals). */
    parent->wait_next   = NULL;
    child->joiners_head = parent;

    /* Sanity: parent is WAITING_JOIN before we destroy child. */
    UASSERT_EQ((int)parent->state, (int)USTRAND_STATE_WAITING_JOIN);
    UASSERT_EQ(vm.strand_runnable_count, 0);

    /* Destroy the child — should wake the parent. */
    urbi_strand_destroy(&vm, child);

    /* joiners_head must be cleared (fork_wake_joiners clears it before walking). */
    /* child is freed at this point; do not dereference it. */

    /* Parent must be READY and runnable_count must be 1. */
    UASSERT_EQ((int)parent->state, (int)USTRAND_STATE_READY);
    UASSERT_EQ(vm.strand_runnable_count, 1);

    /* Cleanup: destroy parent. */
    urbi_strand_destroy(&vm, parent);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_strand_destroy_during_event_wait_suite(void)
{
    printf("test_strand_destroy_during_event_wait\n");
    utest_run("destroy_clears_event_waiter_head",
              destroy_clears_event_waiter_head);
    utest_run("destroy_clears_event_waiter_mid_chain",
              destroy_clears_event_waiter_mid_chain);
    utest_run("destroy_wakes_joiner",
              destroy_wakes_joiner);
}
