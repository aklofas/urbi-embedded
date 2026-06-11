/* SPDX-License-Identifier: BSD-3-Clause */
/* test_strand_cancel_wake — refactor-3 VM-08/SCHED-04: urbi_strand_cancel
 * must route parked strands through the scheduler wake helpers.
 *
 * Pre-fix, urbi_strand_cancel stamped state = USTRAND_STATE_READY in
 * place on a WAITING strand (failure modes OBSERVED on the pre-fix tree):
 *   - a SLEEP-parked strand stayed linked on vm->sleep_q_head with the
 *     reason nibble cleared (sleep-queue invariant violation: every
 *     queue member must be REASON_SLEEP) and was never enqueued on the
 *     ready queue, so the CANCEL unwind never ran (zombie).  The very
 *     next urbi_step then tripped URBI_INTERNAL_ASSERT(reason == SLEEP)
 *     in sched_wake_due_sleepers (SIGABRT); in an assert-elided build
 *     the expiring timer would call sched_strand_unblock on the READY-
 *     stamped strand, driving the READY -> READY make_runnable
 *     transition that corrupts the ready queue (SCHED-005);
 *   - an EVENT-parked strand was unregistered from the waiter chain but
 *     likewise never made runnable (zombie: still on the realm list,
 *     never dispatched, CANCEL never delivered).
 *
 * Post-fix contract (mirrors the urbi_tag_stop wake block):
 *   - SLEEP reason routes through sched_strand_unblock (removes from
 *     sleep_q, decrements wakeup_pending_count, makes runnable);
 *   - other reasons route through sched_strand_make_runnable;
 *   - the strand dispatches on the next urbi_step, the CANCEL unwind
 *     runs at the first safepoint (backward branch of the while loop)
 *     and escalates to fatal: state DEAD, fatal_status UEXEC_CANCEL,
 *     urbi_step returns URBI_STEP_FATAL with vm->fatal_strand wired;
 *   - both queues are consistent (sleep queue empty,
 *     wakeup_pending_count 0).
 *
 * NOTE on strand_runnable_count (updated v0.13.3, refactor-3 SCHED-01): a
 * parked WAITING strand contributes 0 under the single-writer scheme
 * (count == |READY| + |RUNNING|), so the bounded pump now reports WAKE_AT
 * (sleeper parked) / QUIESCENT (event-parked) instead of the pre-refactor
 * RUNNING (which was the phantom count), and waking via
 * sched_strand_unblock / sched_strand_make_runnable leaves the counter at
 * exactly 1 (the READY strand). */

#include "utest.h"

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"
#include "chunk/uchunk.h"
#include "realm/urealm.h"
#include "value/uarena.h"
#include "runtime/umacros.h"
#include "event/uevent.h"
#include "lex/ulex.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "emit/uemit.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers (compile + drive boilerplate mirrors test_tag_self_block.c)
 * =================================================================== */

/* Compile `src` into a heap-allocated module (heap so a fatal loader
 * strand's undischarged root_proto ref routes through rescued_protos
 * instead of dangling on a stack address). */
static UProto *
compile_heap_chunk(UVM *vm, const char *src)
{
    UProto *module = (UProto *)vm->alloc_fn(NULL, sizeof(UProto), vm->alloc_ud);
    if (module == NULL) return NULL;
    urbi_zero(module, sizeof(*module));
    module->heap_allocated = true;
    module->alloc_fn       = vm->alloc_fn;
    module->alloc_ud       = vm->alloc_ud;

    UArena arena;
    uarena_init(&arena, 4096);

    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UEmitter e;
    uemit_init(&e, module, &arena, vm, NULL);
    UParser p;
    uparse_init(&p, &lex, &arena);

    bool ok = true;
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) { ok = false; break; }
        if (uemit_statement(&e, node) != EMIT_OK) { ok = false; break; }
        uarena_reset(&arena);
    }
    if (ok && uemit_finish(&e) != EMIT_OK) ok = false;
    uarena_destroy(&arena);

    if (!ok) {
        uchunk_destroy(module, vm);
        return NULL;
    }
    return module;
}

/* Pump urbi_step until it stops reporting RUNNING (or the cap is hit).
 * Returns the last UStepResult. */
static UStepResult
pump_steps(UVM *vm, int max_steps)
{
    UStepResult rc = URBI_STEP_RUNNING;
    for (int i = 0; i < max_steps; i++) {
        rc = urbi_step(vm, 1000, NULL);
        if (rc != URBI_STEP_RUNNING) break;
    }
    return rc;
}

/* Arm a loader strand for `module` and drive urbi_step bounded.
 * *out_loader receives the strand pointer — valid for ADDRESS COMPARISON
 * only; deref it only after find_strand_in_realm confirms it is still
 * linked (post_dispatch eager-reaps DEAD strands). */
static UStepResult
drive_chunk(UVM *vm, UProto *module, UStrand **out_loader, int max_steps)
{
    URealm *realm = urbi_realm_global(vm);

    UStrand *loader = urbi_strand_create_for_module(vm, realm, module);
    if (out_loader != NULL) *out_loader = loader;
    if (loader == NULL) return URBI_STEP_FATAL;

    return pump_steps(vm, max_steps);
}

/* Walk realm->strands_head for a strand by address.  Returns the strand
 * if still linked (safe to deref), NULL if it has been reaped. */
static UStrand *
find_strand_in_realm(UVM *vm, const UStrand *needle)
{
    URealm *realm = urbi_realm_global(vm);
    for (UStrand *p = realm->strands_head; p != NULL; p = p->next_in_realm)
        if (p == needle) return p;
    return NULL;
}

/* Walk vm->sleep_q_head for a strand by address. */
static bool
sleep_q_contains(UVM *vm, const UStrand *needle)
{
    for (UStrand *p = vm->sleep_q_head; p != NULL; p = p->wait_next)
        if (p == needle) return true;
    return false;
}

/* ===================================================================
 * Case 1: cancel of a SLEEP-parked strand unparks it (sleep queue →
 * ready queue) and the CANCEL unwind runs to DEAD on the next step.
 * =================================================================== */
UTEST(cancel_sleeping_strand_unparks)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* The while loop guarantees a backward-branch safepoint right after
     * resume so the deposited CANCEL is consumed deterministically. */
    UProto *module = compile_heap_chunk(&vm,
        "sleep(10s); while (true) { Realm.hit9 = 1 }");
    UASSERT(module != NULL);

    UStrand *loader = NULL;
    UStepResult rc = drive_chunk(&vm, module, &loader, 20);

    /* Parked on the sleep queue.  SCHED-01: a WAITING strand is NOT
     * counted, so the pump reports WAKE_AT (only a not-yet-due sleeper
     * remains); the 10s timer never expires within the pump, so the only
     * wake source is the cancel below. */
    UASSERT_EQ((int)URBI_STEP_WAKE_AT, (int)rc);
    UStrand *s = find_strand_in_realm(&vm, loader);
    UASSERT(s != NULL);
    if (s != NULL) {
        UASSERT_EQ((unsigned)USTRAND_WAITING, (unsigned)USTRAND_GET_STATE(s));
        UASSERT_EQ((unsigned)USTRAND_REASON_SLEEP,
                   (unsigned)USTRAND_GET_REASON(s));
        UASSERT(sleep_q_contains(&vm, s));
        UASSERT_EQ(1U, vm.wakeup_pending_count);
        UASSERT_EQ(0U, vm.strand_runnable_count);  /* WAITING: not counted */

        /* Cancel.  Post-fix: the strand comes OFF the sleep queue and
         * onto the ready queue.  Pre-fix: READY stamped in place — still
         * linked on the sleep queue, never enqueued (zombie). */
        UASSERT_EQ(URBI_OK, urbi_strand_cancel(&vm, s, urbi_make_nil()));
        UASSERT(!sleep_q_contains(&vm, s));
        UASSERT(vm.sleep_q_head == NULL);
        UASSERT_EQ(0U, vm.wakeup_pending_count);
        UASSERT_EQ((unsigned)USTRAND_READY, (unsigned)USTRAND_GET_STATE(s));
        UASSERT(vm.ready_head == s);
        UASSERT_EQ(1U, vm.strand_runnable_count);  /* see header NOTE */

        /* Drive: the strand dispatches, the CANCEL unwind runs at the
         * first safepoint and escalates to fatal (empty cleanup stack). */
        rc = pump_steps(&vm, 100);
        UASSERT_EQ((int)URBI_STEP_FATAL, (int)rc);
        UASSERT(vm.fatal_strand == s);
        UASSERT(find_strand_in_realm(&vm, s) != NULL);  /* fatal: not reaped */
        UASSERT_EQ((unsigned)USTRAND_DEAD, (unsigned)USTRAND_GET_STATE(s));
        UASSERT_EQ((unsigned)UEXEC_CANCEL, (unsigned)s->fatal_status);

        /* Both queues consistent. */
        UASSERT(vm.sleep_q_head == NULL);
        UASSERT_EQ(0U, vm.wakeup_pending_count);
        UASSERT(vm.ready_head == NULL);
    }

    uchunk_destroy(module, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 2: cancel of an EVENT-parked strand (waituntil(e?)) — the
 * waiter-unregister path already existed in cancel; pin that the strand
 * is ALSO made runnable and runs its CANCEL unwind end-to-end.
 * =================================================================== */
UTEST(cancel_event_parked_strand_unparks)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UProto *module = compile_heap_chunk(&vm,
        "var e = Event.new(); waituntil(e?); while (true) { Realm.hit9 = 1 }");
    UASSERT(module != NULL);

    UStrand *loader = NULL;
    UStepResult rc = drive_chunk(&vm, module, &loader, 20);

    /* Parked on the event waiter chain.  SCHED-01: a WAITING strand is NOT
     * counted, and an event-parked strand has no timer either, so the
     * bounded pump reports QUIESCENT (liveness for "armed" event waits is
     * the SCHED-13 vm_liveness work, next task in this arc). */
    UASSERT_EQ((int)URBI_STEP_QUIESCENT, (int)rc);
    UStrand *s = find_strand_in_realm(&vm, loader);
    UASSERT(s != NULL);
    if (s != NULL) {
        UASSERT_EQ((unsigned)USTRAND_WAITING, (unsigned)USTRAND_GET_STATE(s));
        UASSERT_EQ((unsigned)USTRAND_REASON_EVENT,
                   (unsigned)USTRAND_GET_REASON(s));
        UEvent *ev = s->wait_event_target;
        UASSERT(ev != NULL);
        UASSERT(ev->waiters_head == s);
        UASSERT_EQ(0U, vm.strand_runnable_count);  /* WAITING: not counted */

        /* Cancel.  Both pre- and post-fix unregister from the waiter
         * chain; only post-fix also makes the strand runnable. */
        UASSERT_EQ(URBI_OK, urbi_strand_cancel(&vm, s, urbi_make_nil()));
        UASSERT(ev->waiters_head == NULL);
        UASSERT(s->wait_event_target == NULL);
        UASSERT_EQ((unsigned)USTRAND_READY, (unsigned)USTRAND_GET_STATE(s));
        UASSERT(vm.ready_head == s);
        UASSERT_EQ(1U, vm.strand_runnable_count);  /* see header NOTE */

        rc = pump_steps(&vm, 100);
        UASSERT_EQ((int)URBI_STEP_FATAL, (int)rc);
        UASSERT(vm.fatal_strand == s);
        UASSERT(find_strand_in_realm(&vm, s) != NULL);
        UASSERT_EQ((unsigned)USTRAND_DEAD, (unsigned)USTRAND_GET_STATE(s));
        UASSERT_EQ((unsigned)UEXEC_CANCEL, (unsigned)s->fatal_status);
        UASSERT(vm.ready_head == NULL);
    }

    uchunk_destroy(module, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry point.
 * =================================================================== */

void test_strand_cancel_wake_suite(void);

void
test_strand_cancel_wake_suite(void)
{
    utest_run("strand_cancel_wake: cancel of a SLEEP-parked strand removes "
              "it from the sleep queue and runs the CANCEL unwind",
              cancel_sleeping_strand_unparks);
    utest_run("strand_cancel_wake: cancel of an EVENT-parked strand "
              "unregisters the waiter and runs the CANCEL unwind",
              cancel_event_parked_strand_unparks);
}
