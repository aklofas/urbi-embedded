/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_sched_post_dispatch_alt_driver.c
 *
 * Unit tests for urbi_sched_post_dispatch (scheduler audit F3).
 *
 * Previously the four post-dispatch fix-up steps lived exclusively in urbi_step:
 *   1. Runnable-count DEAD decrement (refactor-3 SCHED-01 single-writer
 *      scheme; the pre-refactor step 1 was a WAITING re-increment).
 *   2. Eager DEAD-strand reap via urbi_strand_destroy (heap strands only).
 *   3. Sleep-queue wake for strands whose wake_us <= now.
 *   4. Periodic pump (every()-body re-spawn).
 *
 * Any alternative driver (urbi_vm_run, future RT scheduler) had to replicate
 * these steps or silently lose forward progress / leak memory.  The helper
 * centralises them.
 *
 * These tests exercise each step in isolation by constructing VM state that
 * exercises the step, calling urbi_sched_post_dispatch directly, and asserting the
 * expected post-condition. */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"
#include "sched/usched_post_dispatch.h"
#include "realm/urealm.h"
#include "runtime/uclosure.h"
#include "chunk/uchunk.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* =========================================================================
 * Helpers: trivial proto + closure factory.
 *
 * urbi_strand_create needs a UClosure with a valid UProto; the proto only
 * needs OP_RET so the strand dispatches cleanly.
 * ========================================================================= */

static uint32_t ret_instr[1];
static UValue   ret_const[1];
static UProto   ret_proto;
static UClosure ret_cl;

static void
init_trivial_closure(void)
{
    ret_instr[0]      = (uint32_t)OP_RET;
    ret_const[0].kind = (uint8_t)UVAL_INT;
    ret_const[0].v.i  = 0;
    memset(&ret_proto, 0, sizeof(ret_proto));
    ret_proto.instructions = ret_instr;
    ret_proto.instr_count  = 1;
    ret_proto.constants    = ret_const;
    ret_proto.const_count  = 1;
    memset(&ret_cl, 0, sizeof(ret_cl));
    ret_cl.proto = &ret_proto;
}

/* Static time source for step 3 tests.  Returns a value set by the test. */
static uint64_t s_mock_time_us = 0;
static uint64_t mock_time_fn(void) { return s_mock_time_us; }

/* v0.13.3 (SCHED-13): thread a stack-local strand onto the global realm's
 * strands_head for the duration of a test (mirrors urbi_vm_run's transient
 * setup).  urbi_sched_post_dispatch's debug recount oracle walks realms_head ->
 * strands_head to verify strand_waiting_count, so a WAITING stub the
 * scheduler can see MUST be realm-reachable (the §6.1 invariant).  Callers
 * MUST unthread before ustrand_destroy/urbi_vm_destroy or realm teardown
 * would urbi_strand_destroy a stack address. */
static void
thread_on_global_realm(UVM *vm, UStrand *s)
{
    URealm *r = urbi_realm_global(vm);
    s->realm           = r;
    s->next_in_realm   = r->strands_head;
    r->strands_head    = s;
}

static void
unthread_from_realm(UStrand *s)
{
    UStrand **pp;
    if (s->realm == NULL) return;
    pp = &s->realm->strands_head;
    while (*pp != NULL) {
        if (*pp == s) { *pp = s->next_in_realm; break; }
        pp = &(*pp)->next_in_realm;
    }
    s->next_in_realm = NULL;
    s->realm         = NULL;
}

/* =========================================================================
 * Step 1 (SCHED-01, v0.13.3): a WAITING strand gets NO count adjustment.
 *
 * Under the single-writer scheme the parking transition (urbi_sched_strand_block)
 * already decremented the count; urbi_sched_post_dispatch must leave a WAITING
 * strand alone.  (The pre-refactor step 1 re-incremented here, pairing with
 * a decrement in urbi_sched_dequeue_ready_head — that pair produced the B10
 * phantom-count leak.)
 * ========================================================================= */

UTEST(post_dispatch_step1_waiting_strand_count_unchanged)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    vm.host_time_us = mock_time_fn;

    UStrand strand;
    ustrand_init(&strand, &vm);
    strand.is_transient_strand = 0U;

    /* The dispatch cycle that leads here: make_runnable (+1), dequeue
     * (count-neutral), RUNNING, urbi_sched_strand_block (-1) -> count 0 with the
     * strand WAITING.  Represent the post-block state directly. */
    vm.strand_runnable_count = 0U;
    strand.state = USTRAND_STATE_WAITING_SLEEP;  /* any WAITING substate */

    UASSERT(USTRAND_IS_WAITING(&strand));
    UASSERT_EQ(vm.strand_runnable_count, 0U);

    /* urbi_sched_post_dispatch: no adjustment for WAITING. */
    urbi_sched_post_dispatch(&vm, &strand);

    UASSERT_EQ(vm.strand_runnable_count, 0U);

    /* Cleanup: strand was not reaped (it's WAITING, not DEAD). */
    strand.state = USTRAND_STATE_DORMANT;  /* quiet teardown */
    ustrand_destroy(&strand, &vm);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Step 1 (SCHED-01): a DEAD transient strand gets NO decrement.
 *
 * Transient strands (urbi_vm_run) never participate in the runnable count
 * (urbi_sched_runnable_inc/dec both skip them); a decrement here would underflow
 * the counter the transient never incremented.
 * ========================================================================= */

UTEST(post_dispatch_step1_dead_transient_strand_no_decrement)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    vm.host_time_us = mock_time_fn;

    UStrand strand;
    ustrand_init(&strand, &vm);
    strand.is_transient_strand = 1U;  /* urbi_vm_run marker */

    vm.strand_runnable_count = 0U;
    strand.state = USTRAND_STATE_DEAD;

    /* DEAD + transient: step 1 dec skipped (and step 2 reap skipped). */
    urbi_sched_post_dispatch(&vm, &strand);

    UASSERT_EQ(vm.strand_runnable_count, 0U);  /* unchanged: no underflow */

    strand.state = USTRAND_STATE_DORMANT;
    ustrand_destroy(&strand, &vm);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Step 2: Heap-allocated DEAD strand is eagerly reaped.
 *
 * Create a heap-allocated strand via urbi_strand_create, mark it DEAD, call
 * urbi_sched_post_dispatch, and verify it is no longer on realm->strands_head
 * (urbi_strand_destroy unlinks from the realm list + frees backing storage).
 * ========================================================================= */

UTEST(post_dispatch_step2_dead_heap_strand_is_reaped)
{
    init_trivial_closure();
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    vm.host_time_us = mock_time_fn;

    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Create a heap-allocated strand. */
    UStrand *s = urbi_strand_create(&vm, realm, &ret_cl);
    UASSERT(s != NULL);

    /* Verify it is on realm->strands_head. */
    bool found_before = false;
    {
        UStrand *cur = realm->strands_head;
        while (cur != NULL) {
            if (cur == s) { found_before = true; break; }
            cur = cur->next_in_realm;
        }
    }
    UASSERT(found_before);

    /* Mark DEAD to trigger step 2.  The strand is not a transient.
     * SCHED-01: a DEAD strand arriving at post_dispatch was RUNNING and
     * therefore counted — seed the count; step 1 decrements it. */
    UASSERT_EQ(s->is_transient_strand, 0U);
    s->state = USTRAND_STATE_DEAD;

    vm.strand_runnable_count = 1U;

    /* urbi_sched_post_dispatch: step 1 decrements (DEAD), step 2 reaps. */
    urbi_sched_post_dispatch(&vm, s);
    /* s is freed now — do NOT dereference. */

    UASSERT_EQ(vm.strand_runnable_count, 0U);  /* step 1 DEAD decrement */

    /* Verify the strand is no longer on realm->strands_head. */
    bool found_after = false;
    {
        UStrand *cur = realm->strands_head;
        while (cur != NULL) {
            /* We check by scan — s is freed so comparing address is UB only
             * if we dereference.  ASan will catch a live dereference; we only
             * scan addresses. */
            if (cur == s) { found_after = true; break; }
            cur = cur->next_in_realm;
        }
    }
    UASSERT(!found_after);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Step 2 transient guard: transient DEAD strand is NOT reaped.
 *
 * urbi_vm_run strands are stack-local; calling urbi_strand_destroy on them
 * would free a stack address (UB).  Verify step 2 is skipped.
 * ========================================================================= */

UTEST(post_dispatch_step2_dead_transient_strand_not_reaped)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    vm.host_time_us = mock_time_fn;

    UStrand strand;
    ustrand_init(&strand, &vm);
    strand.is_transient_strand = 1U;
    strand.state = USTRAND_STATE_DEAD;

    /* realm is NULL for the transient in this test; step 2 guard must fire
     * before urbi_strand_destroy (which would walk realm->strands_head).
     * This call must complete without crashing. */
    vm.strand_runnable_count = 0U;
    urbi_sched_post_dispatch(&vm, &strand);

    /* strand must still be addressable — not freed. */
    UASSERT_EQ(strand.is_transient_strand, 1U);

    /* Cleanup via the safe path. */
    strand.state = USTRAND_STATE_DORMANT;
    ustrand_destroy(&strand, &vm);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Step 3: Sleep-queue strand woken when wake_us <= now.
 *
 * Install a strand on the sleep queue with wake_us = 500 and set mock clock
 * to 1000 (past the deadline).  urbi_sched_post_dispatch step 3 must wake it.
 * ========================================================================= */

UTEST(post_dispatch_step3_sleep_queue_strand_is_woken)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    vm.host_time_us = mock_time_fn;

    /* Sleeper strand — realm-threaded so the debug recount oracle's
     * waiting walk sees it (SCHED-13). */
    UStrand sleeper;
    ustrand_init(&sleeper, &vm);
    thread_on_global_realm(&vm, &sleeper);
    sleeper.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1U;   /* SCHED-01: RUNNING is counted */

    /* Put it on the sleep queue with wake_us = 500. */
    urbi_sched_strand_block(&sleeper, USTRAND_REASON_SLEEP, 500U);
    UASSERT(USTRAND_IS_WAITING(&sleeper));
    UASSERT_EQ(vm.wakeup_pending_count, 1U);

    /* A dummy non-transient strand to pass as the "dispatched" strand. */
    UStrand driver;
    ustrand_init(&driver, &vm);
    driver.is_transient_strand = 0U;
    driver.state = USTRAND_STATE_DEAD;  /* suppress steps 1 and 2 for it */
    /* We need it DEAD but not heap-allocated so step 2 fires — but since it
     * is stack-local, set is_transient_strand to skip step 2 reap.
     * Actually, keep it non-DEAD to avoid step 2 entirely: */
    driver.state = USTRAND_STATE_DORMANT;
    vm.strand_runnable_count = 0U;

    /* Set mock time to 1000 (past sleeper's wake_us=500). */
    s_mock_time_us = 1000U;

    /* urbi_sched_post_dispatch step 3: sleeper must be woken. */
    urbi_sched_post_dispatch(&vm, &driver);

    /* Sleeper should now be READY (urbi_sched_strand_unblock makes it runnable). */
    UASSERT(!USTRAND_IS_WAITING(&sleeper));
    UASSERT_EQ(vm.wakeup_pending_count, 0U);
    UASSERT_EQ(vm.strand_runnable_count, 1U);  /* sleeper became READY */

    /* Cleanup. */
    urbi_sched_dequeue_ready_head(&vm);  /* remove sleeper from ready queue */
    sleeper.state = USTRAND_STATE_DORMANT;
    unthread_from_realm(&sleeper);
    ustrand_destroy(&sleeper, &vm);
    ustrand_destroy(&driver, &vm);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Step 3: Sleep-queue strand NOT woken when wake_us > now.
 *
 * Same setup but mock clock is 200 (before the deadline).  Strand stays
 * on the sleep queue.
 * ========================================================================= */

UTEST(post_dispatch_step3_sleep_queue_strand_not_woken_early)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    vm.host_time_us = mock_time_fn;

    UStrand sleeper;
    ustrand_init(&sleeper, &vm);
    thread_on_global_realm(&vm, &sleeper);   /* SCHED-13 oracle visibility */
    sleeper.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1U;   /* SCHED-01: RUNNING is counted */
    urbi_sched_strand_block(&sleeper, USTRAND_REASON_SLEEP, 1000U);
    UASSERT_EQ(vm.wakeup_pending_count, 1U);

    UStrand driver;
    ustrand_init(&driver, &vm);
    driver.is_transient_strand = 0U;
    driver.state = USTRAND_STATE_DORMANT;
    vm.strand_runnable_count = 0U;

    /* Clock is 200, wake_us is 1000 — not yet due. */
    s_mock_time_us = 200U;
    urbi_sched_post_dispatch(&vm, &driver);

    UASSERT(USTRAND_IS_WAITING(&sleeper));       /* still sleeping */
    UASSERT_EQ(vm.wakeup_pending_count, 1U);     /* still on queue */
    UASSERT_EQ(vm.strand_runnable_count, 0U);    /* not made runnable */

    /* Cleanup via unblock (removes from sleep queue). */
    urbi_sched_strand_unblock(&sleeper);
    sleeper.state = USTRAND_STATE_DORMANT;
    unthread_from_realm(&sleeper);
    ustrand_destroy(&sleeper, &vm);
    ustrand_destroy(&driver, &vm);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Step 3 absent when host_time_us is NULL: no crash, no wake.
 *
 * Step 3 is gated on vm->host_time_us != NULL.  Without a time source we
 * cannot compare wake_us; the helper must not attempt to walk the sleep queue.
 * ========================================================================= */

UTEST(post_dispatch_step3_skipped_without_time_fn)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    vm.host_time_us = NULL;  /* no time source */

    UStrand sleeper;
    ustrand_init(&sleeper, &vm);
    thread_on_global_realm(&vm, &sleeper);   /* SCHED-13 oracle visibility */
    sleeper.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1U;   /* SCHED-01: RUNNING is counted */
    urbi_sched_strand_block(&sleeper, USTRAND_REASON_SLEEP, 0U);  /* wake_us=0: overdue */
    UASSERT_EQ(vm.wakeup_pending_count, 1U);

    UStrand driver;
    ustrand_init(&driver, &vm);
    driver.state = USTRAND_STATE_DORMANT;
    vm.strand_runnable_count = 0U;

    /* Must not crash even though the strand is overdue and queue is non-empty. */
    urbi_sched_post_dispatch(&vm, &driver);

    /* Strand stays WAITING (step 3 skipped). */
    UASSERT(USTRAND_IS_WAITING(&sleeper));
    UASSERT_EQ(vm.wakeup_pending_count, 1U);

    /* Cleanup. */
    urbi_sched_strand_unblock(&sleeper);
    sleeper.state = USTRAND_STATE_DORMANT;
    unthread_from_realm(&sleeper);
    ustrand_destroy(&sleeper, &vm);
    ustrand_destroy(&driver, &vm);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * All four steps integrated: DEAD heap strand + overdue sleeper.
 *
 * Construct a VM with:
 *   - a heap strand that just died (step 2 should reap it),
 *   - a sleep-queue strand that is overdue (step 3 should wake it),
 *   - mock time past both deadlines.
 *
 * After urbi_sched_post_dispatch:
 *   - dead strand is gone from realm->strands_head,
 *   - sleeper is READY (wakeup_pending_count == 0, strand_runnable_count == 1).
 * ========================================================================= */

UTEST(post_dispatch_all_steps_integrated)
{
    init_trivial_closure();
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    vm.host_time_us = mock_time_fn;
    s_mock_time_us = 5000U;

    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Heap strand that just finished — will be reaped by step 2. */
    UStrand *dead_s = urbi_strand_create(&vm, realm, &ret_cl);
    UASSERT(dead_s != NULL);
    dead_s->state = USTRAND_STATE_DEAD;

    /* Sleeper overdue at wake_us=1000 (now=5000). */
    UStrand sleeper;
    ustrand_init(&sleeper, &vm);
    thread_on_global_realm(&vm, &sleeper);   /* SCHED-13 oracle visibility */
    sleeper.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1U;   /* SCHED-01: RUNNING is counted */
    urbi_sched_strand_block(&sleeper, USTRAND_REASON_SLEEP, 1000U);
    UASSERT_EQ(vm.wakeup_pending_count, 1U);

    /* strand_runnable_count: dead_s was RUNNING when it died, so it is
     * still in the counted set on entry (SCHED-01); step 1 decrements. */
    vm.strand_runnable_count = 1U;

    /* Call the helper with dead_s as the "just dispatched" strand. */
    urbi_sched_post_dispatch(&vm, dead_s);
    /* dead_s is freed — do not dereference. */

    /* Step 2: dead strand gone from realm. */
    bool dead_found = false;
    {
        UStrand *cur = realm->strands_head;
        while (cur != NULL) {
            if (cur == dead_s) { dead_found = true; break; }
            cur = cur->next_in_realm;
        }
    }
    UASSERT(!dead_found);

    /* Step 3: sleeper is now READY. */
    UASSERT(!USTRAND_IS_WAITING(&sleeper));
    UASSERT_EQ(vm.wakeup_pending_count, 0U);
    UASSERT_EQ(vm.strand_runnable_count, 1U);

    /* Cleanup. */
    urbi_sched_dequeue_ready_head(&vm);
    sleeper.state = USTRAND_STATE_DORMANT;
    unthread_from_realm(&sleeper);
    ustrand_destroy(&sleeper, &vm);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_sched_post_dispatch_alt_driver_suite(void)
{
    utest_run("post_dispatch step1: WAITING strand count unchanged (SCHED-01)",
              post_dispatch_step1_waiting_strand_count_unchanged);
    utest_run("post_dispatch step1: DEAD transient strand not decremented",
              post_dispatch_step1_dead_transient_strand_no_decrement);
    utest_run("post_dispatch step2: DEAD heap strand is eagerly reaped",
              post_dispatch_step2_dead_heap_strand_is_reaped);
    utest_run("post_dispatch step2: DEAD transient strand is NOT reaped",
              post_dispatch_step2_dead_transient_strand_not_reaped);
    utest_run("post_dispatch step3: overdue sleep-queue strand is woken",
              post_dispatch_step3_sleep_queue_strand_is_woken);
    utest_run("post_dispatch step3: early sleep-queue strand stays asleep",
              post_dispatch_step3_sleep_queue_strand_not_woken_early);
    utest_run("post_dispatch step3: no crash when host_time_us is NULL",
              post_dispatch_step3_skipped_without_time_fn);
    utest_run("post_dispatch all steps: dead heap strand + overdue sleeper",
              post_dispatch_all_steps_integrated);
}
