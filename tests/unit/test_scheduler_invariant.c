/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: scheduler-agnostic invariant — every state transition
 * preserves a strand's membership in realm.strands_head.
 *
 * Per pre-M4 GC strand-walker spec §6.1:
 *   Every UStrand whose register window may contain GC-managed UValues
 *   MUST be reachable from vm->realms_head → realm.strands_head → strand.
 *   Scheduler implementations are responsible for maintaining this
 *   invariant; the GC walker assumes it without re-verification.
 *
 * Sweep covers READY → WAITING_SLEEP → READY, READY → RUNNING,
 * RUNNING → WAITING_JOIN, RUNNING → DEAD.  After each transition the
 * strand must still be reachable via realm.strands_head.  DEAD strands
 * remain on the list until urbi_realm_destroy reclaims them. */

#include "utest.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "realm/urealm.h"
#include "sched/usched_cooperative.h"
#include "urbi/urbi.h"   /* urbi_strand_create / urbi_strand_start / urbi_strand_destroy */

#include <stdint.h>
#include <stddef.h>

#define UTEST(name) static void name(void)

/* === Helper: linear search for strand in realm.strands_head list === */

static int strand_on_realm_list(URealm *r, UStrand *s)
{
    UStrand *cur;
    if (r == NULL) return 0;
    for (cur = r->strands_head; cur != NULL; cur = cur->next_in_realm) {
        if (cur == s) return 1;
    }
    return 0;
}

/* === Test: every state transition preserves realm membership === */

UTEST(every_state_transition_preserves_strand_in_realm_list)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    /* Strand 1: drives READY → WAITING_SLEEP → READY → RUNNING → DEAD. */
    UStrand *s = urbi_strand_create(r, NULL);
    UASSERT(s != NULL);

    /* Initial: DORMANT, on realm.strands_head. */
    UASSERT_EQ((int)USTRAND_GET_STATE(s), (int)USTRAND_DORMANT);
    UASSERT(strand_on_realm_list(r, s));

    /* DORMANT → READY. */
    urbi_strand_start(s);
    UASSERT_EQ((int)USTRAND_GET_STATE(s), (int)USTRAND_READY);
    UASSERT(strand_on_realm_list(r, s));

    /* READY → RUNNING (manual transition: scheduler dispatch would do this
     * via sched_dequeue_ready_head + assign state).  The invariant is on
     * realm.strands_head, not on ready_head. */
    sched_dequeue_ready_head(&vm);
    s->state = USTRAND_STATE_RUNNING;
    UASSERT(strand_on_realm_list(r, s));

    /* RUNNING → WAITING_SLEEP (sched_strand_block decrements
     * strand_runnable_count for RUNNING strands). */
    sched_strand_block(s, USTRAND_REASON_SLEEP, /*wake_us*/ 1000U);
    UASSERT_EQ((int)USTRAND_GET_STATE(s), (int)USTRAND_WAITING);
    UASSERT_EQ((int)USTRAND_GET_REASON(s), (int)USTRAND_REASON_SLEEP);
    UASSERT(strand_on_realm_list(r, s));    /* KEY: still on realm list */

    /* WAITING_SLEEP → READY. */
    sched_strand_unblock(s);
    UASSERT_EQ((int)USTRAND_GET_STATE(s), (int)USTRAND_READY);
    UASSERT(strand_on_realm_list(r, s));

    /* READY → RUNNING again. */
    sched_dequeue_ready_head(&vm);
    s->state = USTRAND_STATE_RUNNING;
    UASSERT(strand_on_realm_list(r, s));

    /* RUNNING → WAITING_JOIN.  Spawn a child, park s on its joiners chain. */
    UStrand *child = urbi_strand_create(r, NULL);
    UASSERT(child != NULL);
    UASSERT(strand_on_realm_list(r, child));

    s->wait_next       = child->joiners_head;
    child->joiners_head = s;
    sched_strand_block(s, USTRAND_REASON_JOIN, (uint64_t)(uintptr_t)child);
    UASSERT_EQ((int)USTRAND_GET_STATE(s), (int)USTRAND_WAITING);
    UASSERT_EQ((int)USTRAND_GET_REASON(s), (int)USTRAND_REASON_JOIN);
    UASSERT(strand_on_realm_list(r, s));    /* KEY: WAITING_JOIN still on list */

    /* RUNNING → DEAD (top-level OP_RET path).  M3 does not auto-unlink DEAD
     * strands; they remain on realm.strands_head until urbi_realm_destroy
     * reclaims them, per pre-M4 spec §5.2 (Option a). */
    s->state = USTRAND_STATE_DEAD;
    UASSERT(strand_on_realm_list(r, s));    /* KEY: DEAD still on list */
    UASSERT(strand_on_realm_list(r, child));

    /* Cleanup: realm destroy walks strands_head to free both. */
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* === Test: multiple strands across mixed states all stay on realm list === */

UTEST(mixed_state_strands_all_remain_on_realm_list)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UStrand *a = urbi_strand_create(r, NULL);   /* will stay DORMANT */
    UStrand *b = urbi_strand_create(r, NULL);   /* will go READY */
    UStrand *c = urbi_strand_create(r, NULL);   /* will go WAITING_SLEEP */
    UStrand *d = urbi_strand_create(r, NULL);   /* will go DEAD */
    UASSERT(a && b && c && d);

    /* Drive each into a different state. */
    urbi_strand_start(b);                                         /* DORMANT → READY */

    sched_dequeue_ready_head(&vm);                                /* dequeues b */
    b->state = USTRAND_STATE_RUNNING;
    sched_strand_block(b, USTRAND_REASON_SLEEP, 5000U);           /* RUNNING → WAITING_SLEEP */

    /* The above dequeued b but c was never on the queue.  Park c directly. */
    c->state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;                                 /* satisfy block's RUNNING-decrement */
    sched_strand_block(c, USTRAND_REASON_SLEEP, 200U);            /* RUNNING → WAITING_SLEEP */

    d->state = USTRAND_STATE_DEAD;                                /* DORMANT → DEAD (manual) */

    /* All four must be reachable via realm.strands_head. */
    UASSERT(strand_on_realm_list(r, a));
    UASSERT(strand_on_realm_list(r, b));
    UASSERT(strand_on_realm_list(r, c));
    UASSERT(strand_on_realm_list(r, d));

    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* === Suite entry point === */

void test_scheduler_invariant_suite(void)
{
    printf("  [scheduler_invariant]\n");
    utest_run("every_state_transition_preserves_strand_in_realm_list",
              every_state_transition_preserves_strand_in_realm_list);
    utest_run("mixed_state_strands_all_remain_on_realm_list",
              mixed_state_strands_all_remain_on_realm_list);
}
