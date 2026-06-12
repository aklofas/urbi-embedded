/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: waiter unlink on tag-stop / cancel / panic (spec #3 §6.4).
 *
 * Cases:
 *   1. tag_stop_during_waituntil_unlinks_waiter:
 *      Park a strand on waituntil; tag-stop it; verify unlinked from
 *      e->waiters_head and wait_event_target cleared.
 *   2. cancel_during_waituntil_unlinks_waiter:
 *      Park a strand on waituntil; urbi_strand_cancel; verify unlinked.
 *   3. panic_during_waituntil_unlinks_waiter:
 *      Park a strand on waituntil; urbi_strand_panic; verify unlinked.
 *   4. tag_stop_middle_waiter_unlinks_correctly:
 *      Three waiters on the same event; tag-stop the middle one; verify
 *      head and tail remain linked, middle is gone.
 *   5. unregister_idempotent_when_not_waiting:
 *      tag-stop a strand that is not on any event chain (wait_event_target
 *      == NULL); no crash or corruption. */

#include "utest.h"

#include "event/uevent.h"
#include "event/uevent_emit.h"
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"  /* sched_strand_block (waiter park) */
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "tag/utag.h"
#include "runtime/uunwind.h"
#include "urbi/urbi.h"

#include <stddef.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers
 * =================================================================== */

static UValue
make_nil(void)
{
    UValue v;
    v.kind = UVAL_NIL;
    v.v.i  = 0;
    return v;
}

/* Park a stack-local strand as an event waiter (tail-append to e->waiters_head).
 * Mirrors the setup in c_event_waituntil without going through the scratch
 * guard.  v0.13.3 (SCHED-13): the WAIT_EVENT transition goes through
 * sched_strand_block (as c_event_waituntil does) so strand_waiting_count is
 * maintained — a raw state stamp would trip the wake paths' no-saturation
 * decrement. */
static void
park_strand_on_event(UStrand *s, UEvent *e, struct UVM *vm)
{
    s->state = USTRAND_STATE_RUNNING;
    vm->strand_runnable_count++;    /* satisfy block's RUNNING-decrement */
    sched_strand_block(s, USTRAND_REASON_EVENT, (uint64_t)(uintptr_t)e);
    s->wait_event_target   = e;
    s->next_event_waiter   = NULL;
    s->last_event_payload  = make_nil();

    /* Tail-append. */
    if (!e->waiters_head) {
        e->waiters_head = s;
    } else {
        UStrand *t = e->waiters_head;
        while (t->next_event_waiter) t = t->next_event_waiter;
        t->next_event_waiter = s;
    }
}

/* ===================================================================
 * Case 1: tag_stop_during_waituntil_unlinks_waiter
 *
 * Strand parked on waituntil; urbi_tag_stop; verify:
 *   - e->waiters_head == NULL (strand removed).
 *   - s->wait_event_target == NULL.
 *   - s->next_event_waiter == NULL.
 *   - s->last_event_payload remains NIL (no payload deposited).
 * =================================================================== */

UTEST(tag_stop_during_waituntil_unlinks_waiter)
{
    UVM    vm;
    UValue nil = make_nil();

    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UASSERT(r->tag != NULL);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    /* Create a realm-managed strand so it appears in tag's member list. */
    UStrand *s = urbi_strand_create(&vm, r, NULL);
    UASSERT(s != NULL);

    /* Park s on the event. */
    park_strand_on_event(s, e, &vm);
    UASSERT(e->waiters_head == s);
    UASSERT(s->wait_event_target == e);

    /* Fire tag-stop — deposits unwind and wakes/unregisters s. */
    int rc = urbi_tag_stop(&vm, r->tag, nil);
    UASSERT_EQ(rc, URBI_OK);

    /* Strand must be unlinked from the event's waiter chain. */
    UASSERT(e->waiters_head == NULL);
    UASSERT(s->wait_event_target == NULL);
    UASSERT(s->next_event_waiter == NULL);

    /* last_event_payload must remain NIL (not overwritten by emit). */
    UASSERT_EQ((int)s->last_event_payload.kind, (int)UVAL_NIL);

    urbi_strand_destroy(&vm, s);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 2: cancel_during_waituntil_unlinks_waiter
 *
 * Park a strand on an event; urbi_strand_cancel; verify unlinked.
 * =================================================================== */

UTEST(cancel_during_waituntil_unlinks_waiter)
{
    UVM    vm;
    UValue nil = make_nil();

    urbi_vm_init(&vm, NULL, NULL);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    /* Stack-local strand — no realm needed for cancel. */
    UStrand s;
    ustrand_init(&s, &vm);

    park_strand_on_event(&s, e, &vm);
    UASSERT(e->waiters_head == &s);

    int rc = urbi_strand_cancel(&vm, &s, nil);
    UASSERT_EQ(rc, URBI_OK);

    /* Unlinked. */
    UASSERT(e->waiters_head == NULL);
    UASSERT(s.wait_event_target == NULL);
    UASSERT(s.next_event_waiter == NULL);

    /* Pending unwind must be CANCEL. */
    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_CANCEL);

    /* Strand must have been transitioned out of WAIT_EVENT. */
    UASSERT(s.state != USTRAND_WAIT_EVENT);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 3: panic_during_waituntil_unlinks_waiter
 *
 * Park a strand on an event; urbi_strand_panic; verify unlinked.
 * =================================================================== */

UTEST(panic_during_waituntil_unlinks_waiter)
{
    UVM    vm;

    urbi_vm_init(&vm, NULL, NULL);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    park_strand_on_event(&s, e, &vm);
    UASSERT(e->waiters_head == &s);

    int rc = urbi_strand_panic(&vm, &s, "test panic");
    UASSERT_EQ(rc, URBI_OK);

    /* Unlinked. */
    UASSERT(e->waiters_head == NULL);
    UASSERT(s.wait_event_target == NULL);
    UASSERT(s.next_event_waiter == NULL);

    /* Strand must be DEAD. */
    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_DEAD);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 4: tag_stop_middle_waiter_unlinks_correctly
 *
 * Three waiters (s1, s2, s3) on the same event.  Tag-stop s2 only
 * (give s1 and s3 a different cleanup entry so the tag doesn't fire on
 * them; simpler: use urbi_strand_cancel directly on s2 which calls
 * uevent_waiter_unregister directly).
 *
 * Actually: use urbi_strand_cancel on s2 (middle waiter) to splice it
 * out, then verify s1 → s3 chain is intact.
 * =================================================================== */

UTEST(tag_stop_middle_waiter_unlinks_correctly)
{
    UVM    vm;
    UValue nil = make_nil();

    urbi_vm_init(&vm, NULL, NULL);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    UStrand s1, s2, s3;
    ustrand_init(&s1, &vm);
    ustrand_init(&s2, &vm);
    ustrand_init(&s3, &vm);

    /* Park in order: s1, s2, s3. */
    park_strand_on_event(&s1, e, &vm);
    park_strand_on_event(&s2, e, &vm);
    park_strand_on_event(&s3, e, &vm);

    /* Chain: e->waiters_head → s1 → s2 → s3 → NULL. */
    UASSERT(e->waiters_head == &s1);
    UASSERT(s1.next_event_waiter == &s2);
    UASSERT(s2.next_event_waiter == &s3);
    UASSERT(s3.next_event_waiter == NULL);

    /* Cancel the middle waiter. */
    int rc = urbi_strand_cancel(&vm, &s2, nil);
    UASSERT_EQ(rc, URBI_OK);

    /* s2 must be unlinked; s1 → s3 chain remains. */
    UASSERT(e->waiters_head == &s1);
    UASSERT(s1.next_event_waiter == &s3);
    UASSERT(s3.next_event_waiter == NULL);
    UASSERT(s2.wait_event_target == NULL);
    UASSERT(s2.next_event_waiter == NULL);

    /* Clean up remaining waiters by emitting. */
    c_event_emit_async(&vm, e, nil);
    UASSERT(e->waiters_head == NULL);

    ustrand_destroy(&s1, &vm);
    ustrand_destroy(&s2, &vm);
    ustrand_destroy(&s3, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 5: unregister_idempotent_when_not_waiting
 *
 * A strand not on any event (wait_event_target == NULL); tag-stop it
 * anyway — no crash, no corruption, event chain untouched.
 * =================================================================== */

UTEST(unregister_idempotent_when_not_waiting)
{
    UVM    vm;
    UValue nil = make_nil();

    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    /* A second strand that IS on the event, to verify it isn't disturbed. */
    UStrand bystander;
    ustrand_init(&bystander, &vm);
    park_strand_on_event(&bystander, e, &vm);

    /* Create a realm strand NOT on the event. */
    UStrand *s = urbi_strand_create(&vm, r, NULL);
    UASSERT(s != NULL);
    UASSERT(s->wait_event_target == NULL);

    /* Tag-stop fires on s (realm member), but s is not on e->waiters_head. */
    int rc = urbi_tag_stop(&vm, r->tag, nil);
    UASSERT_EQ(rc, URBI_OK);

    /* Bystander must still be on the event chain — tag-stop didn't touch it. */
    UASSERT(e->waiters_head == &bystander);
    UASSERT(bystander.wait_event_target == e);

    /* s must have received the TAG_STOP unwind. */
    UASSERT_EQ((int)s->pending_unwind, (int)UEXEC_TAG_STOP);

    /* Clean up. */
    bystander.state = USTRAND_STATE_DORMANT;
    bystander.wait_event_target = NULL;
    e->waiters_head = NULL;
    ustrand_destroy(&bystander, &vm);

    urbi_strand_destroy(&vm, s);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_waituntil_tag_stop_suite(void)
{
    printf("test_waituntil_tag_stop\n");
    utest_run("tag_stop_during_waituntil_unlinks_waiter",
              tag_stop_during_waituntil_unlinks_waiter);
    utest_run("cancel_during_waituntil_unlinks_waiter",
              cancel_during_waituntil_unlinks_waiter);
    utest_run("panic_during_waituntil_unlinks_waiter",
              panic_during_waituntil_unlinks_waiter);
    utest_run("tag_stop_middle_waiter_unlinks_correctly",
              tag_stop_middle_waiter_unlinks_correctly);
    utest_run("unregister_idempotent_when_not_waiting",
              unregister_idempotent_when_not_waiting);
}
