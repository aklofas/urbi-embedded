/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UStrand state byte encoding + transition macros (T2);
   Strand C API lifecycle: create/start/spawn/destroy (T20). */

#include "utest.h"
#include "sched/ustrand.h"
#include "realm/urealm.h"
#include "sched/usched_cooperative.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* Case 1: after ustrand_init the state byte is DORMANT with reason NONE. */
UTEST(strand_state_dormant_at_init) {
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    UASSERT_EQ(USTRAND_GET_STATE(&s), USTRAND_DORMANT);
    UASSERT_EQ(USTRAND_GET_REASON(&s), USTRAND_REASON_NONE);
    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 2: WAITING composite values round-trip through the helper macros. */
UTEST(strand_state_waiting_macros_round_trip) {
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);

    s.state = USTRAND_STATE_WAITING_SLEEP;
    UASSERT(USTRAND_IS_WAITING(&s));
    UASSERT_EQ(USTRAND_GET_REASON(&s), USTRAND_REASON_SLEEP);

    s.state = USTRAND_STATE_WAITING_EVENT;
    UASSERT(USTRAND_IS_WAITING(&s));
    UASSERT_EQ(USTRAND_GET_REASON(&s), USTRAND_REASON_EVENT);

    s.state = USTRAND_STATE_WAITING_JOIN;
    UASSERT(USTRAND_IS_WAITING(&s));
    UASSERT_EQ(USTRAND_GET_REASON(&s), USTRAND_REASON_JOIN);

    /* v0.13.3 (SCHED-13): restore DORMANT before teardown.  The raw state
     * stamps above bypass sched_strand_block, so the strand was never
     * counted in strand_waiting_count; destroying it in a WAITING state
     * would trip the no-saturation decrement (deliberately — masking
     * forbidden).  This test only exercises the state-byte macros. */
    s.state = USTRAND_STATE_DORMANT;

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 3: RUNNING state is not flagged as WAITING; reason reads NONE. */
UTEST(strand_state_running_not_waiting) {
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    s.state = USTRAND_STATE_RUNNING;
    UASSERT(!USTRAND_IS_WAITING(&s));
    UASSERT_EQ(USTRAND_GET_STATE(&s), USTRAND_RUNNING);
    UASSERT_EQ(USTRAND_GET_REASON(&s), USTRAND_REASON_NONE);
    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 4: the state field is exactly one byte wide. */
UTEST(strand_state_byte_size_one) {
    UStrand s;
    (void)s;
    UASSERT_EQ(sizeof(s.state), (size_t)1);
}

/* ===== T20: Strand C API tests ===== */

/* Helper: set up a VM + realm + scheduler, run f, tear down cleanly. */
static UVM  g_vm;
static URealm *g_realm;

static void setup_vm_realm(void) {
    urbi_vm_init(&g_vm, NULL, NULL);
    sched_init(&g_vm, NULL);
    g_realm = urbi_realm_create(&g_vm);
}

static void teardown_vm_realm(void) {
    urbi_realm_destroy(&g_vm, g_realm);
    g_realm = NULL;
    urbi_vm_destroy(&g_vm);
}

/* Allocator spy: counts allocations and can fail at a specific call. */
typedef struct {
    int alloc_calls;
    int fail_at;  /* -1 means never fail; 0+ triggers failure on that call */
} AllocSpy;

static void *spy_alloc(void *ptr, size_t n, void *ud) {
    AllocSpy *spy = (AllocSpy *)ud;
    if (n > 0 && ptr == NULL) {  /* allocation request (not a free) */
        spy->alloc_calls++;
        if (spy->fail_at >= 0 && spy->alloc_calls > spy->fail_at) {
            return NULL;
        }
    }
    if (n == 0) { free(ptr); return NULL; }
    return realloc(ptr, n);
}

/* Case 5: urbi_strand_create returns a non-NULL pointer and the strand is DORMANT.
   Verifies: realm and entry_closure fields are wired, state is DORMANT, vm back-pointer set. */
UTEST(strand_create_starts_dormant) {
    setup_vm_realm();

    UStrand *s = urbi_strand_create(&g_vm, g_realm, NULL);
    UASSERT(s != NULL);
    UASSERT_EQ(USTRAND_GET_STATE(s), USTRAND_DORMANT);
    UASSERT(s->vm    == &g_vm);
    UASSERT(s->realm == g_realm);
    UASSERT(s->entry_closure == NULL);
    /* cleanup stack should be pre-allocated (non-NULL base) */
    UASSERT(s->cleanup_base != NULL);
    /* strand_runnable_count must be 0 — DORMANT does not increment it */
    UASSERT_EQ(g_vm.strand_runnable_count, 0U);

    urbi_strand_destroy(&g_vm, s);
    teardown_vm_realm();
}

/* Case 6: urbi_strand_start transitions DORMANT → READY and increments counter.
   Does NOT call urbi_step — only verifies state + counter. */
UTEST(strand_start_transitions_to_ready) {
    setup_vm_realm();

    UStrand *s = urbi_strand_create(&g_vm, g_realm, NULL);
    UASSERT(s != NULL);
    UASSERT_EQ(USTRAND_GET_STATE(s), USTRAND_DORMANT);
    UASSERT_EQ(g_vm.strand_runnable_count, 0U);

    urbi_strand_start(&g_vm, s);
    UASSERT_EQ(USTRAND_GET_STATE(s), USTRAND_READY);
    UASSERT_EQ(g_vm.strand_runnable_count, 1U);
    /* The strand should be the ready-queue head. */
    UASSERT(g_vm.ready_head == s);

    /* Dequeue manually to leave the VM in a clean state for teardown. */
    sched_dequeue_ready_head(&g_vm);
    urbi_strand_destroy(&g_vm, s);
    teardown_vm_realm();
}

/* Case 7: urbi_strand_spawn is equivalent to create + start.
   Verifies the strand is READY, counter is 1, and realm/entry fields are set. */
UTEST(strand_spawn_is_create_plus_start) {
    setup_vm_realm();

    UStrand *s = urbi_strand_spawn(&g_vm, g_realm, NULL);
    UASSERT(s != NULL);
    UASSERT_EQ(USTRAND_GET_STATE(s), USTRAND_READY);
    UASSERT(s->vm    == &g_vm);
    UASSERT(s->realm == g_realm);
    UASSERT_EQ(g_vm.strand_runnable_count, 1U);
    UASSERT(g_vm.ready_head == s);

    sched_dequeue_ready_head(&g_vm);
    urbi_strand_destroy(&g_vm, s);
    teardown_vm_realm();
}

/* Case 8: urbi_strand_destroy(NULL) is a safe no-op. */
UTEST(strand_destroy_null_safe) {
    /* Must not crash. */
    urbi_strand_destroy(NULL, NULL);
}

/* Case 9: create + destroy round-trip leaves the VM clean (no leaks, counter zero).
   ASan exercises the allocator path. */
UTEST(strand_create_destroy_round_trip) {
    setup_vm_realm();

    UStrand *s = urbi_strand_create(&g_vm, g_realm, NULL);
    UASSERT(s != NULL);
    UASSERT_EQ(g_vm.strand_runnable_count, 0U);

    urbi_strand_destroy(&g_vm, s);
    /* After destroy: counter must still be 0 (DORMANT never incremented it). */
    UASSERT_EQ(g_vm.strand_runnable_count, 0U);

    teardown_vm_realm();
}

/* Case 10: two strands spawned; both appear on the ready queue in FIFO order. */
UTEST(strand_spawn_two_fifo_order) {
    setup_vm_realm();

    UStrand *a = urbi_strand_spawn(&g_vm, g_realm, NULL);
    UStrand *b = urbi_strand_spawn(&g_vm, g_realm, NULL);
    UASSERT(a != NULL);
    UASSERT(b != NULL);
    UASSERT_EQ(g_vm.strand_runnable_count, 2U);
    /* FIFO: a was enqueued first → head */
    UASSERT(g_vm.ready_head == a);
    UASSERT(g_vm.ready_tail == b);

    sched_dequeue_ready_head(&g_vm);
    sched_dequeue_ready_head(&g_vm);
    urbi_strand_destroy(&g_vm, a);
    urbi_strand_destroy(&g_vm, b);
    teardown_vm_realm();
}

/* Case 11: urbi_strand_create returns NULL when its struct alloc fails.
   Verifies that the strand alloc OOM path is handled cleanly with no leaks. */
UTEST(strand_create_returns_null_on_oom) {
    UVM vm;
    URealm *realm;
    UStrand *s;
    int allocs_after_realm;

    /* First, count how many allocs are needed to set up a realm. */
    AllocSpy spy1 = { 0, -1 };  /* never fail */
    urbi_vm_init(&vm, spy_alloc, &spy1);
    sched_init(&vm, NULL);
    realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);
    allocs_after_realm = spy1.alloc_calls;

    /* Clean up for the real test. */
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);

    /* Now do the real test: allocate realm successfully, fail on strand alloc. */
    AllocSpy spy2 = { 0, allocs_after_realm };  /* fail on the next alloc after realm */
    urbi_vm_init(&vm, spy_alloc, &spy2);
    sched_init(&vm, NULL);
    realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);
    UASSERT_EQ(spy2.alloc_calls, allocs_after_realm);

    /* Attempt strand creation — should fail on the next alloc. */
    s = urbi_strand_create(&vm, realm, NULL);
    UASSERT(s == NULL);  /* OOM: alloc failed */
    UASSERT_EQ(spy2.alloc_calls, allocs_after_realm + 1);  /* tried one more */
    UASSERT_EQ(vm.strand_runnable_count, 0U);  /* counter must stay clean */

    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* CHSTR-042 (T107): sched_strand_make_runnable rejects a DEAD strand silently
   in production (NDEBUG) builds AND asserts in debug builds.  Without this
   guard a re-enqueue of a DEAD strand would re-set state to READY, bump
   strand_runnable_count, and dispatch into a freed register stack.

   Production-build test (NDEBUG): the early return is exercised; default
   builds (assert enabled) trip the URBI_INTERNAL_ASSERT before the early
   return.  Compile-guarded so the test runs only under NDEBUG, which is
   the production-flavour build that observes the silent fail-safe. */
#ifdef NDEBUG
UTEST(make_runnable_rejects_dead_strand) {
    setup_vm_realm();

    UStrand *s = urbi_strand_create(&g_vm, g_realm, NULL);
    UASSERT(s != NULL);
    UASSERT_EQ(USTRAND_GET_STATE(s), USTRAND_DORMANT);

    /* Force-mark DEAD without going through the proper teardown path so
       we can exercise the make_runnable guard directly. */
    s->state = USTRAND_STATE_DEAD;
    UASSERT_EQ(g_vm.strand_runnable_count, 0U);
    UASSERT(g_vm.ready_head == NULL);

    /* Production fail-safe: should be a no-op. */
    sched_strand_make_runnable(s);
    UASSERT_EQ(USTRAND_GET_STATE(s), USTRAND_DEAD);  /* state UNCHANGED */
    UASSERT_EQ(g_vm.strand_runnable_count, 0U);       /* counter UNCHANGED */
    UASSERT(g_vm.ready_head == NULL);                 /* queue still empty */
    UASSERT(g_vm.ready_tail == NULL);

    /* Reset state so urbi_strand_destroy doesn't trip its own asserts. */
    s->state = USTRAND_STATE_DORMANT;
    urbi_strand_destroy(&g_vm, s);
    teardown_vm_realm();
}
#endif

/* CHSTR-015 (T103): urbi_strand_destroy on a READY strand must unlink it from
   the ready queue's neighbour pointers BEFORE clearing local ready_next/prev.
   Otherwise the queue head/tail or surviving siblings hold dangling pointers
   into freed memory.  Test: spawn three strands, destroy the middle one, and
   verify head/tail and the surviving strands' ready_next/prev form a clean
   2-element list with no references to the freed strand. */
UTEST(strand_destroy_unlinks_from_ready_queue_first) {
    setup_vm_realm();

    UStrand *a = urbi_strand_spawn(&g_vm, g_realm, NULL);
    UStrand *b = urbi_strand_spawn(&g_vm, g_realm, NULL);
    UStrand *c = urbi_strand_spawn(&g_vm, g_realm, NULL);
    UASSERT(a != NULL && b != NULL && c != NULL);
    UASSERT_EQ(g_vm.strand_runnable_count, 3U);
    UASSERT(g_vm.ready_head == a);
    UASSERT(g_vm.ready_tail == c);
    UASSERT(a->ready_next == b);
    UASSERT(b->ready_prev == a);
    UASSERT(b->ready_next == c);
    UASSERT(c->ready_prev == b);

    /* Destroy middle strand b — must splice cleanly out of the queue. */
    urbi_strand_destroy(&g_vm, b);

    /* Survivors form a 2-element queue: a → c. */
    UASSERT_EQ(g_vm.strand_runnable_count, 2U);
    UASSERT(g_vm.ready_head == a);
    UASSERT(g_vm.ready_tail == c);
    UASSERT(a->ready_next == c);
    UASSERT(c->ready_prev == a);
    UASSERT(a->ready_prev == NULL);
    UASSERT(c->ready_next == NULL);

    /* Drain remaining strands cleanly. */
    sched_dequeue_ready_head(&g_vm);
    sched_dequeue_ready_head(&g_vm);
    urbi_strand_destroy(&g_vm, a);
    urbi_strand_destroy(&g_vm, c);
    teardown_vm_realm();
}

/* CHSTR-013 (T101): ustrand_destroy must not underflow vm->host_call_pending_count
   when it decrements via sched_strand_account_destroy.  Construct a strand with
   cross_strand_stop_pending set but vm->host_call_pending_count == 0 and verify
   the counter stays at 0 (uint32_t underflow would produce 0xFFFFFFFF).  Pins
   the existing guard at usched_cooperative.c:261 against future reorderings. */
UTEST(strand_destroy_does_not_underflow_host_call_pending) {
    setup_vm_realm();

    UStrand *s = urbi_strand_create(&g_vm, g_realm, NULL);
    UASSERT(s != NULL);

    /* Manually mark the strand as having a cross-strand stop deposited
       without bumping vm->host_call_pending_count.  This mirrors the
       hypothetical out-of-balance path: a deposit was rolled back (or the
       counter was reset by a bug) but the strand-level flag still asserts.
       Without the guard, ustrand_destroy would underflow. */
    s->cross_strand_stop_pending = 1U;
    UASSERT_EQ(g_vm.host_call_pending_count, 0U);

    urbi_strand_destroy(&g_vm, s);
    /* Counter must NOT have wrapped to UINT32_MAX. */
    UASSERT_EQ(g_vm.host_call_pending_count, 0U);

    teardown_vm_realm();
}

/* CHSTR-001 (T98): urbi_strand_create returns NULL when the cleanup-stack
   allocation inside ustrand_init fails (the second alloc, after the strand
   struct itself).  Verifies the post-strand-alloc OOM path frees the partial
   allocation rather than returning a DEAD-but-allocated strand.  Closes the
   "fully-functional strand or NULL" contract pinned at urbi.h:194. */
UTEST(strand_create_returns_null_on_cleanup_oom) {
    UVM vm;
    URealm *realm;
    UStrand *s;
    int allocs_after_realm;

    /* Count baseline allocs needed for VM + realm setup. */
    AllocSpy spy1 = { 0, -1 };
    urbi_vm_init(&vm, spy_alloc, &spy1);
    sched_init(&vm, NULL);
    realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);
    allocs_after_realm = spy1.alloc_calls;

    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);

    /* Real test: succeed on the strand struct alloc, fail on the cleanup-
       stack alloc that follows it inside ustrand_init.  fail_at = N means
       "fail starting on alloc N+1"; here we want to succeed for the strand
       struct (alloc allocs_after_realm + 1) and fail on the cleanup-stack
       alloc (allocs_after_realm + 2). */
    AllocSpy spy2 = { 0, allocs_after_realm + 1 };
    urbi_vm_init(&vm, spy_alloc, &spy2);
    sched_init(&vm, NULL);
    realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);
    UASSERT_EQ(spy2.alloc_calls, allocs_after_realm);

    s = urbi_strand_create(&vm, realm, NULL);
    UASSERT(s == NULL);  /* cleanup-stack alloc failed → strand_create returns NULL */
    /* Two allocs were attempted: the strand struct (success) + cleanup
       stack (failure).  Anything else means the failure path leaked allocs. */
    UASSERT_EQ(spy2.alloc_calls, allocs_after_realm + 2);
    UASSERT_EQ(vm.strand_runnable_count, 0U);
    /* realm->strands_head must NOT contain a partial strand. */
    UASSERT(realm->strands_head == NULL);

    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

void test_strand_suite(void) {
    utest_run("strand_state_dormant_at_init",          strand_state_dormant_at_init);
    utest_run("strand_state_waiting_macros_round_trip", strand_state_waiting_macros_round_trip);
    utest_run("strand_state_running_not_waiting",       strand_state_running_not_waiting);
    utest_run("strand_state_byte_size_one",             strand_state_byte_size_one);
    /* T20 lifecycle API */
    utest_run("strand_create_starts_dormant",           strand_create_starts_dormant);
    utest_run("strand_start_transitions_to_ready",      strand_start_transitions_to_ready);
    utest_run("strand_spawn_is_create_plus_start",      strand_spawn_is_create_plus_start);
    utest_run("strand_destroy_null_safe",               strand_destroy_null_safe);
    utest_run("strand_create_destroy_round_trip",       strand_create_destroy_round_trip);
    utest_run("strand_spawn_two_fifo_order",            strand_spawn_two_fifo_order);
    utest_run("strand_create_returns_null_on_oom",      strand_create_returns_null_on_oom);
    utest_run("strand_create_returns_null_on_cleanup_oom", strand_create_returns_null_on_cleanup_oom);
    utest_run("strand_destroy_does_not_underflow_host_call_pending",
              strand_destroy_does_not_underflow_host_call_pending);
    utest_run("strand_destroy_unlinks_from_ready_queue_first",
              strand_destroy_unlinks_from_ready_queue_first);
#ifdef NDEBUG
    utest_run("make_runnable_rejects_dead_strand",
              make_runnable_rejects_dead_strand);
#endif
}
