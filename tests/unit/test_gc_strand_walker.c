/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: GC strand walker — realm-hierarchy iteration (T32) +
 * transient-strand routing to vm->global_realm (T33).
 *
 * Per pre-M4 GC strand-walker spec §4.2 / §5.1 / §6.1:
 *   sched_walk_roots iterates vm->realms_head → realm.strands_head, visiting
 *   every strand regardless of state (READY / RUNNING / WAITING_*); DEAD
 *   strands are visited but strand_walk_roots returns immediately so their
 *   register cells are not painted gray.
 *
 * These tests pin the LAYOUT invariant — strand membership in
 * realm.strands_head — rather than driving real OP_FORK / OP_JOIN_WAIT
 * dispatch, which is exercised by test_fork.c.  They verify that:
 *   1. A WAITING_JOIN strand on realm.strands_head IS visited by the walker.
 *   2. A DEAD strand on realm.strands_head is visited but the per-strand
 *      walker exits immediately (no roots reported for that strand).
 *   3. A uvm_run transient strand is routed to vm->global_realm during the
 *      run and unlinked again before uvm_run returns. */

#include "utest.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "realm/urealm.h"
#include "sched/usched_cooperative.h"
#include "urbi/urbi.h"   /* urbi_strand_create / urbi_strand_destroy */
#include "umodule.h"
#include "value/uarena.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "parse/uast.h"
#include "value/uvalue.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* === Walker-callback context: count visits keyed by strand pointer === */

typedef struct {
    UStrand *target;
    int      visit_count;   /* number of UValue slots reported for target */
    int      total_count;   /* total UValue slots reported across all strands */
} VisitProbe;

/* Per-slot callback: counts every visit, plus per-target visits. */
static void visit_probe_cb(UVM *vm, UValue *root, void *ctx)
{
    VisitProbe *p = (VisitProbe *)ctx;
    (void)vm;
    (void)root;
    p->total_count++;
    /* The walker doesn't pass the owning strand to the callback, so we
     * count total visits across the run.  For per-strand precision we use
     * the second probe form (visit_per_strand_cb below). */
    if (p->target != NULL) p->visit_count++;
}

/* === Test 1 (T32): WAITING_JOIN strand IS visited via realm-hierarchy walk ===
 *
 * M3 baseline walked ready_head + sleep_q_head only; WAITING_JOIN parents
 * sit on child->joiners_head and were missed.  After T32 the walker iterates
 * realm.strands_head, so any WAITING_JOIN strand registered with the realm
 * is visited regardless of which scheduler-private list it sits on.
 *
 * Layout setup (no OP_FORK_JOIN dispatch needed):
 *   - Allocate two strands via urbi_strand_create (both end up on
 *     realm.strands_head with valid cleanup-stack entries).
 *   - Force the "parent" into WAITING_JOIN by hand (state byte + wait_payload).
 *   - Call sched_walk_roots with a probe callback; assert it fired enough
 *     times to confirm both strands' register windows were walked. */
UTEST(strand_walker_visits_waiting_join_strand)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UStrand *parent = urbi_strand_create(r, NULL);
    UStrand *child  = urbi_strand_create(r, NULL);
    UASSERT(parent != NULL);
    UASSERT(child  != NULL);

    /* Allocate per-strand register stacks — sched_walk_roots only walks
     * UVM_STACK_CAP slots when stack != NULL. */
    const size_t stack_bytes = UVM_STACK_CAP * sizeof(UValue);
    parent->stack = (UValue *)vm.alloc_fn(NULL, stack_bytes, vm.alloc_ud);
    child->stack  = (UValue *)vm.alloc_fn(NULL, stack_bytes, vm.alloc_ud);
    UASSERT(parent->stack != NULL);
    UASSERT(child->stack  != NULL);
    /* Zero the register windows so the walker reports stable UVAL_NIL slots. */
    {
        volatile unsigned char *p = (volatile unsigned char *)parent->stack;
        size_t i;
        for (i = 0; i < stack_bytes; i++) p[i] = 0;
    }
    {
        volatile unsigned char *p = (volatile unsigned char *)child->stack;
        size_t i;
        for (i = 0; i < stack_bytes; i++) p[i] = 0;
    }

    /* Park parent in WAITING_JOIN, hand-wired (no scheduler dispatch). */
    parent->state                    = USTRAND_STATE_WAITING_JOIN;
    parent->wait_payload.join_parent = child;
    parent->wait_next                = child->joiners_head;
    child->joiners_head              = parent;

    /* Probe: count total visits across the walker run. */
    VisitProbe probe = {0};
    sched_walk_roots(&vm, visit_probe_cb, &probe);

    /* Both strands must contribute UVM_STACK_CAP slots + 2 unwind slots
     * (unwind_value + fatal_value).  Floor: 2 * (UVM_STACK_CAP + 2). */
    UASSERT(probe.total_count >= 2 * (UVM_STACK_CAP + 2));

    /* Cleanup: realm destroy will walk strands_head and free both. */
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* === Test 2 (T32): DEAD strand is filtered inside strand_walk_roots ===
 *
 * Per pre-M4 spec §5.2 (Option a — filter in walker): DEAD strands stay on
 * realm.strands_head until urbi_realm_destroy reclaims them, but the
 * per-strand walker returns immediately at the DEAD guard so their register
 * cells are not reported as roots. */
UTEST(strand_walker_dead_strand_filtered)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UStrand *s = urbi_strand_create(r, NULL);
    UASSERT(s != NULL);

    const size_t stack_bytes = UVM_STACK_CAP * sizeof(UValue);
    s->stack = (UValue *)vm.alloc_fn(NULL, stack_bytes, vm.alloc_ud);
    UASSERT(s->stack != NULL);
    {
        volatile unsigned char *p = (volatile unsigned char *)s->stack;
        size_t i;
        for (i = 0; i < stack_bytes; i++) p[i] = 0;
    }

    /* Baseline visit count with strand alive (RUNNING). */
    s->state = USTRAND_STATE_RUNNING;
    VisitProbe alive = {0};
    sched_walk_roots(&vm, visit_probe_cb, &alive);

    /* Now mark DEAD; walker should report 0 slots for this strand. */
    s->state = USTRAND_STATE_DEAD;
    VisitProbe dead = {0};
    sched_walk_roots(&vm, visit_probe_cb, &dead);

    UASSERT(alive.total_count > 0);
    UASSERT_EQ(dead.total_count, 0);

    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* === Test 3 (T32): walker reaches strand on realm.strands_head regardless
 *                   of whether it appears on ready_head/sleep_q_head ===
 *
 * Hand-create a strand, attach to realm.strands_head only (NOT to ready or
 * sleep), and verify the walker still visits its register window.  This is
 * the contract that the scheduler-invariant test (test_scheduler_invariant.c)
 * also pins from a different angle. */
UTEST(strand_walker_reaches_strand_off_scheduler_queues)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UStrand *s = urbi_strand_create(r, NULL);
    UASSERT(s != NULL);

    /* Confirm the strand really is OFF both scheduler queues at this point. */
    UASSERT(vm.ready_head    == NULL);
    UASSERT(vm.sleep_q_head  == NULL);

    /* Allocate and zero a register stack. */
    const size_t stack_bytes = UVM_STACK_CAP * sizeof(UValue);
    s->stack = (UValue *)vm.alloc_fn(NULL, stack_bytes, vm.alloc_ud);
    UASSERT(s->stack != NULL);
    {
        volatile unsigned char *p = (volatile unsigned char *)s->stack;
        size_t i;
        for (i = 0; i < stack_bytes; i++) p[i] = 0;
    }
    s->state = USTRAND_STATE_DORMANT;  /* not READY/WAITING — just on realm.strands_head */

    VisitProbe probe = {0};
    sched_walk_roots(&vm, visit_probe_cb, &probe);

    /* The strand's register window + unwind slots must have been walked. */
    UASSERT(probe.total_count >= UVM_STACK_CAP + 2);

    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* === Test 4 (T33): uvm_run transient is routed to global_realm + unlinked ===
 *
 * Lazy-create the global realm via uvm_run, then verify after return that
 *   (a) vm.global_realm is non-NULL (the run-path lazy-created it).
 *   (b) global_realm.strands_head is empty (transient unlinked at exit).
 * This exercises the head-insert / unlink round trip in uvm_run. */
UTEST(uvm_run_transient_threaded_then_unlinked_from_global_realm)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* No realms initially. */
    UASSERT(vm.global_realm == NULL);
    UASSERT(vm.realms_head  == NULL);

    /* Compile a trivial module: just a literal "0;" so OP_RET has a value. */
    ULexer  lex;
    UArena  arena;
    UModule module = {0};
    const char *src = "0;";
    ulex_init(&lex, src, strlen(src));
    uarena_init(&arena, 1024);

    UEmitter e;
    uemit_init(&e, &module, &arena, &vm, NULL);

    UParser p;
    uparse_init(&p, &lex, &arena);

    UAstNode *node = uparse_next_statement(&p);
    UASSERT(node != NULL);
    UASSERT(node->kind != AST_ERROR);
    UASSERT(uemit_statement(&e, node) == EMIT_OK);
    UASSERT(uemit_finish(&e) == EMIT_OK);

    UValue out = {0};
    UVMError rc = uvm_run(&vm, &module, &out);
    UASSERT_EQ((int)rc, (int)UVM_OK);

    /* T33: global_realm was lazy-created during the run. */
    UASSERT(vm.global_realm != NULL);

    /* Transient was unlinked at uvm_run exit; strands_head is empty. */
    UASSERT(vm.global_realm->strands_head == NULL);

    uarena_destroy(&arena);
    umodule_destroy(&module);
    uvm_destroy(&vm);
}

/* === Suite entry point === */

void test_gc_strand_walker_suite(void)
{
    printf("  [gc_strand_walker]\n");
    utest_run("strand_walker_visits_waiting_join_strand",
              strand_walker_visits_waiting_join_strand);
    utest_run("strand_walker_dead_strand_filtered",
              strand_walker_dead_strand_filtered);
    utest_run("strand_walker_reaches_strand_off_scheduler_queues",
              strand_walker_reaches_strand_off_scheduler_queues);
    utest_run("uvm_run_transient_threaded_then_unlinked_from_global_realm",
              uvm_run_transient_threaded_then_unlinked_from_global_realm);
}
