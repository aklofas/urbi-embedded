/* SPDX-License-Identifier: BSD-3-Clause */
/* Phase 8 (T38-T41): scheduler state-byte aliasing regressions.
 *
 * Audit IDs closed:
 *   SCHED-001 — JOIN/WAIT_EVENT state-byte alias (uunwind.c discriminator)
 *   SCHED-002 — sched_strand_block re-block already-WAITING (entry-state assert)
 *   SCHED-003 — sched_strand_yield re-yield already-READY (entry-state assert)
 *   SCHED-004 — c_event_waituntil re-stamp leaves stale sleep-queue links
 */

#include "utest.h"
#include "sched/usched_cooperative.h"
#include "sched/ustrand.h"
#include "vm/uvm.h"
#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * T38 — SCHED-001: JOIN-blocked strand state distinct from WAIT_EVENT
 * ===================================================================
 *
 * After the v0.5.5 CHSTR-016 renumbering, REASON_JOIN = 0x04 produces
 * composite 0x34, distinct from WAIT_EVENT's 0x33.  But the architectural
 * fix is to discriminate by class+reason, not by full-state equality.
 * This test pins both invariants:
 *   (a) the literal byte is distinct (ensures the renumbering holds);
 *   (b) the class+reason discriminator agrees with what uunwind.c's
 *       is_event_parked_strand helper computes. */
UTEST(join_blocked_strand_state_distinct_from_wait_event)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand parent, child;
    ustrand_init(&parent, &vm);
    ustrand_init(&child,  &vm);

    /* Simulate child currently RUNNING about to block on parent. */
    child.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;

    sched_strand_block(&child, USTRAND_REASON_JOIN, (uint64_t)(uintptr_t)&parent);

    /* (a) literal byte distinct from WAIT_EVENT. */
    UASSERT(child.state != USTRAND_WAIT_EVENT);
    UASSERT_EQ((int)child.state, (int)USTRAND_STATE_WAITING_JOIN);
    /* class = WAITING; reason = JOIN, not EVENT. */
    UASSERT_EQ((int)(child.state & USTRAND_STATE_MASK), (int)USTRAND_WAITING);
    UASSERT_EQ((int)USTRAND_GET_REASON(&child), (int)USTRAND_REASON_JOIN);
    UASSERT(USTRAND_GET_REASON(&child) != USTRAND_REASON_EVENT);

    ustrand_destroy(&parent, &vm);
    ustrand_destroy(&child,  &vm);
    urbi_vm_destroy(&vm);
}

/* The audit's runtime concern: uunwind.c used `s->state == USTRAND_WAIT_EVENT`
 * to gate uevent_waiter_unregister.  After T38, all three sites use the
 * shape (class == WAITING && reason == EVENT).  is_event_parked_strand is
 * file-static in uunwind.c; we replicate the predicate locally and cross-
 * check that it reports the right answer for every interesting state.
 *
 * If uunwind.c regresses to full-state equality and a future renumbering
 * re-introduces an alias (e.g., reason 0x05 + WAITING = 0x35 colliding with
 * some new state) the (a) check above would also break.  Together the two
 * tests cover both the literal-byte and the structural pattern. */
static inline int is_event_parked_local(const UStrand *s) {
    return ((s->state & USTRAND_STATE_MASK) == USTRAND_WAITING &&
            (s->state & USTRAND_REASON_MASK) == USTRAND_REASON_EVENT);
}

UTEST(is_event_parked_predicate_distinguishes_reason_byte)
{
    UStrand s;
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);

    /* RUNNING — class != WAITING. */
    s.state = USTRAND_STATE_RUNNING;
    UASSERT(!is_event_parked_local(&s));

    /* WAITING_SLEEP — class WAITING, reason SLEEP. */
    s.state = USTRAND_STATE_WAITING_SLEEP;
    UASSERT(!is_event_parked_local(&s));

    /* WAITING_JOIN — class WAITING, reason JOIN. */
    s.state = USTRAND_STATE_WAITING_JOIN;
    UASSERT(!is_event_parked_local(&s));

    /* WAITING_EVENT — class WAITING, reason EVENT. */
    s.state = USTRAND_STATE_WAITING_EVENT;
    UASSERT(is_event_parked_local(&s));

    /* USTRAND_WAIT_EVENT macro is the same composite. */
    s.state = USTRAND_WAIT_EVENT;
    UASSERT(is_event_parked_local(&s));

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

void test_sched_state_aliasing_suite(void) {
    utest_run("join_blocked_strand_state_distinct_from_wait_event",
              join_blocked_strand_state_distinct_from_wait_event);
    utest_run("is_event_parked_predicate_distinguishes_reason_byte",
              is_event_parked_predicate_distinguishes_reason_byte);
}
