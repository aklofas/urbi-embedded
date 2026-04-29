/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UStrand state byte encoding + transition macros (T2);
   Strand C API lifecycle: create/start/spawn/destroy (T20). */

#include "utest.h"
#include "ustrand.h"
#include "realm/urealm.h"
#include "sched/usched_cooperative.h"
#include "uvm.h"
#include "urbi/urbi.h"
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* Case 1: after ustrand_init the state byte is DORMANT with reason NONE. */
UTEST(strand_state_dormant_at_init) {
    UVM vm;
    UStrand s;
    uvm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    UASSERT_EQ(USTRAND_GET_STATE(&s), USTRAND_DORMANT);
    UASSERT_EQ(USTRAND_GET_REASON(&s), USTRAND_REASON_NONE);
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 2: WAITING composite values round-trip through the helper macros. */
UTEST(strand_state_waiting_macros_round_trip) {
    UVM vm;
    UStrand s;
    uvm_init(&vm, NULL, NULL);
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

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 3: RUNNING state is not flagged as WAITING; reason reads NONE. */
UTEST(strand_state_running_not_waiting) {
    UVM vm;
    UStrand s;
    uvm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    s.state = USTRAND_STATE_RUNNING;
    UASSERT(!USTRAND_IS_WAITING(&s));
    UASSERT_EQ(USTRAND_GET_STATE(&s), USTRAND_RUNNING);
    UASSERT_EQ(USTRAND_GET_REASON(&s), USTRAND_REASON_NONE);
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
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
    uvm_init(&g_vm, NULL, NULL);
    sched_init(&g_vm, NULL);
    g_realm = urbi_realm_create(&g_vm);
}

static void teardown_vm_realm(void) {
    urbi_realm_destroy(&g_vm, g_realm);
    g_realm = NULL;
    uvm_destroy(&g_vm);
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
    /* Use stdlib for the actual allocation. */
    return realloc(ptr, n);
}

/* Case 5: urbi_strand_create returns a non-NULL pointer and the strand is DORMANT.
   Verifies: realm and entry_closure fields are wired, state is DORMANT, vm back-pointer set. */
UTEST(strand_create_starts_dormant) {
    setup_vm_realm();

    UStrand *s = urbi_strand_create(g_realm, NULL);
    UASSERT(s != NULL);
    UASSERT_EQ(USTRAND_GET_STATE(s), USTRAND_DORMANT);
    UASSERT(s->vm    == &g_vm);
    UASSERT(s->realm == g_realm);
    UASSERT(s->entry_closure == NULL);
    /* cleanup stack should be pre-allocated (non-NULL base) */
    UASSERT(s->cleanup_base != NULL);
    /* strand_runnable_count must be 0 — DORMANT does not increment it */
    UASSERT_EQ(g_vm.strand_runnable_count, 0u);

    urbi_strand_destroy(s);
    teardown_vm_realm();
}

/* Case 6: urbi_strand_start transitions DORMANT → READY and increments counter.
   Does NOT call urbi_step — only verifies state + counter. */
UTEST(strand_start_transitions_to_ready) {
    setup_vm_realm();

    UStrand *s = urbi_strand_create(g_realm, NULL);
    UASSERT(s != NULL);
    UASSERT_EQ(USTRAND_GET_STATE(s), USTRAND_DORMANT);
    UASSERT_EQ(g_vm.strand_runnable_count, 0u);

    urbi_strand_start(s);
    UASSERT_EQ(USTRAND_GET_STATE(s), USTRAND_READY);
    UASSERT_EQ(g_vm.strand_runnable_count, 1u);
    /* The strand should be the ready-queue head. */
    UASSERT(g_vm.ready_head == s);

    /* Dequeue manually to leave the VM in a clean state for teardown. */
    sched_dequeue_ready_head(&g_vm);
    urbi_strand_destroy(s);
    teardown_vm_realm();
}

/* Case 7: urbi_strand_spawn is equivalent to create + start.
   Verifies the strand is READY, counter is 1, and realm/entry fields are set. */
UTEST(strand_spawn_is_create_plus_start) {
    setup_vm_realm();

    UStrand *s = urbi_strand_spawn(g_realm, NULL);
    UASSERT(s != NULL);
    UASSERT_EQ(USTRAND_GET_STATE(s), USTRAND_READY);
    UASSERT(s->vm    == &g_vm);
    UASSERT(s->realm == g_realm);
    UASSERT_EQ(g_vm.strand_runnable_count, 1u);
    UASSERT(g_vm.ready_head == s);

    sched_dequeue_ready_head(&g_vm);
    urbi_strand_destroy(s);
    teardown_vm_realm();
}

/* Case 8: urbi_strand_destroy(NULL) is a safe no-op. */
UTEST(strand_destroy_null_safe) {
    /* Must not crash. */
    urbi_strand_destroy(NULL);
}

/* Case 9: create + destroy round-trip leaves the VM clean (no leaks, counter zero).
   ASan exercises the allocator path. */
UTEST(strand_create_destroy_round_trip) {
    setup_vm_realm();

    UStrand *s = urbi_strand_create(g_realm, NULL);
    UASSERT(s != NULL);
    UASSERT_EQ(g_vm.strand_runnable_count, 0u);

    urbi_strand_destroy(s);
    /* After destroy: counter must still be 0 (DORMANT never incremented it). */
    UASSERT_EQ(g_vm.strand_runnable_count, 0u);

    teardown_vm_realm();
}

/* Case 10: two strands spawned; both appear on the ready queue in FIFO order. */
UTEST(strand_spawn_two_fifo_order) {
    setup_vm_realm();

    UStrand *a = urbi_strand_spawn(g_realm, NULL);
    UStrand *b = urbi_strand_spawn(g_realm, NULL);
    UASSERT(a != NULL);
    UASSERT(b != NULL);
    UASSERT_EQ(g_vm.strand_runnable_count, 2u);
    /* FIFO: a was enqueued first → head */
    UASSERT(g_vm.ready_head == a);
    UASSERT(g_vm.ready_tail == b);

    sched_dequeue_ready_head(&g_vm);
    sched_dequeue_ready_head(&g_vm);
    urbi_strand_destroy(a);
    urbi_strand_destroy(b);
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
    uvm_init(&vm, spy_alloc, &spy1);
    sched_init(&vm, NULL);
    realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);
    allocs_after_realm = spy1.alloc_calls;

    /* Clean up for the real test. */
    urbi_realm_destroy(&vm, realm);
    uvm_destroy(&vm);

    /* Now do the real test: allocate realm successfully, fail on strand alloc. */
    AllocSpy spy2 = { 0, allocs_after_realm };  /* fail on the next alloc after realm */
    uvm_init(&vm, spy_alloc, &spy2);
    sched_init(&vm, NULL);
    realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);
    UASSERT_EQ(spy2.alloc_calls, allocs_after_realm);

    /* Attempt strand creation — should fail on the next alloc. */
    s = urbi_strand_create(realm, NULL);
    UASSERT(s == NULL);  /* OOM: alloc failed */
    UASSERT_EQ(spy2.alloc_calls, allocs_after_realm + 1);  /* tried one more */
    UASSERT_EQ(vm.strand_runnable_count, 0u);  /* counter must stay clean */

    urbi_realm_destroy(&vm, realm);
    uvm_destroy(&vm);
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
}
