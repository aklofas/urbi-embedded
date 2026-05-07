/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: ISR-safe SPSC event ring (row 9 §11.3 / T18).
   Tests cover init, inject, drain, overflow, payload round-trip,
   and the Linux-only multi-threaded fuzz path. */

#include "utest.h"
#include "event/uevent_ring.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ---- helpers ----------------------------------------------------------- */

/* Allocate a UEventRing on the stack and init it.  Used by tests that do not
   need a full VM (direct ring manipulation). */
static UEventRing *make_ring_static(UEventRing *storage)
{
    uevent_ring_init(storage);
    return storage;
}

/* ---- Case 1: fresh ring is not pending --------------------------------- */
UTEST(event_ring_init_empty)
{
    UEventRing r;
    make_ring_static(&r);

    UASSERT(!uevent_ring_has_pending(&r));

    /* overflow_count should start at zero. */
    uint32_t ov;
    __atomic_load(&r.overflow_count, &ov, __ATOMIC_RELAXED);
    UASSERT_EQ((long long)ov, 0LL);
}

/* ---- Case 2: single inject → ring becomes pending ---------------------- */
UTEST(event_ring_inject_then_pending)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT(vm.event_ring != NULL);
    UASSERT(!uevent_ring_has_pending(vm.event_ring));

    int rc = urbi_inject_event(&vm, 42U, NULL, 0U);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(uevent_ring_has_pending(vm.event_ring));

    uvm_destroy(&vm);
}

/* ---- Case 3: fill ring → next inject returns RING_FULL + overflow++ ---- */
UTEST(event_ring_overflow_returns_error_with_counter)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT(vm.event_ring != NULL);

    /* Fill all URBI_EVENT_RING_DEPTH - 1 slots (SPSC: one slot reserved as
       sentinel to distinguish full from empty). */
    int i;
    for (i = 0; i < URBI_EVENT_RING_DEPTH - 1; i++) {
        int rc = urbi_inject_event(&vm, (uint32_t)i, NULL, 0U);
        UASSERT_EQ(rc, URBI_OK);
    }

    /* Ring is now full: next inject must fail. */
    int rc = urbi_inject_event(&vm, 9999U, NULL, 0U);
    UASSERT_EQ(rc, URBI_ERR_EVENT_RING_FULL);

    /* overflow_count must be exactly 1. */
    uint32_t ov;
    __atomic_load(&vm.event_ring->overflow_count, &ov, __ATOMIC_RELAXED);
    UASSERT_EQ((long long)ov, 1LL);

    uvm_destroy(&vm);
}

/* ---- Case 4: drain after 2 injects → ring becomes empty ---------------- */
UTEST(event_ring_drain_advances_read_idx)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT(vm.event_ring != NULL);

    urbi_inject_event(&vm, 1U, NULL, 0U);
    urbi_inject_event(&vm, 2U, NULL, 0U);
    UASSERT(uevent_ring_has_pending(vm.event_ring));

    uevent_ring_drain(&vm);
    UASSERT(!uevent_ring_has_pending(vm.event_ring));

    /* Drain should not trip any overflow. */
    uint32_t ov;
    __atomic_load(&vm.event_ring->overflow_count, &ov, __ATOMIC_RELAXED);
    UASSERT_EQ((long long)ov, 0LL);

    uvm_destroy(&vm);
}

/* ---- Case 5: payload round-trip (direct ring inspection) --------------- */
UTEST(event_ring_payload_round_trip)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT(vm.event_ring != NULL);

    uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    int rc = urbi_inject_event(&vm, 0xCAFEU, payload, sizeof(payload));
    UASSERT_EQ(rc, URBI_OK);

    /* Inspect the ring entry at write_idx - 1 (= 0 since ring was empty). */
    UEventRingEntry *e = &vm.event_ring->ring[0];
    UASSERT_EQ((long long)e->event_id,    (long long)0xCAFEU);
    UASSERT_EQ((long long)e->payload_len, (long long)sizeof(payload));
    UASSERT_EQ((long long)e->payload[0],  (long long)0xDEU);
    UASSERT_EQ((long long)e->payload[1],  (long long)0xADU);
    UASSERT_EQ((long long)e->payload[2],  (long long)0xBEU);
    UASSERT_EQ((long long)e->payload[3],  (long long)0xEFU);

    uvm_destroy(&vm);
}

/* ---- Case 6: payload too large → rejected, overflow++ ----------------- */
UTEST(event_ring_payload_too_large_rejected)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT(vm.event_ring != NULL);

    uint8_t big[URBI_EVENT_PAYLOAD_MAX + 1];
    int i;
    for (i = 0; i < URBI_EVENT_PAYLOAD_MAX + 1; i++) big[i] = (uint8_t)i;

    int rc = urbi_inject_event(&vm, 7U, big, sizeof(big));
    UASSERT_EQ(rc, URBI_ERR_EVENT_PAYLOAD_TOO_LARGE);

    /* Ring should remain empty. */
    UASSERT(!uevent_ring_has_pending(vm.event_ring));

    /* Overflow counter bumped. */
    uint32_t ov;
    __atomic_load(&vm.event_ring->overflow_count, &ov, __ATOMIC_RELAXED);
    UASSERT_EQ((long long)ov, 1LL);

    uvm_destroy(&vm);
}

/* ---- Case 7: drain-when-empty is a no-op (no crash, no counter drift) - */
UTEST(event_ring_drain_empty_noop)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT(vm.event_ring != NULL);
    UASSERT(!uevent_ring_has_pending(vm.event_ring));

    uint32_t eq_before = vm.event_queue_count;
    uevent_ring_drain(&vm);

    /* No events drained from an empty ring: event_queue_count unchanged. */
    UASSERT_EQ((long long)vm.event_queue_count, (long long)eq_before);
    UASSERT(!uevent_ring_has_pending(vm.event_ring));

    uvm_destroy(&vm);
}

/* ---- Case 8: overflow_count not bumped on successful inject ------------ */
UTEST(event_ring_overflow_not_bumped_on_success)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT(vm.event_ring != NULL);

    int rc = urbi_inject_event(&vm, 1U, NULL, 0U);
    UASSERT_EQ(rc, URBI_OK);

    uint32_t ov;
    __atomic_load(&vm.event_ring->overflow_count, &ov, __ATOMIC_RELAXED);
    UASSERT_EQ((long long)ov, 0LL);

    uvm_destroy(&vm);
}

/* ---- Case 9: zero-len payload with NULL pointer accepted --------------- */
UTEST(event_ring_zero_payload_null_ptr_accepted)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT(vm.event_ring != NULL);

    int rc = urbi_inject_event(&vm, 55U, NULL, 0U);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(uevent_ring_has_pending(vm.event_ring));

    UEventRingEntry *e = &vm.event_ring->ring[0];
    UASSERT_EQ((long long)e->event_id,    (long long)55U);
    UASSERT_EQ((long long)e->payload_len, (long long)0U);

    uvm_destroy(&vm);
}

/* ---- Case 10: event_queue_count tracks drain count --------------------- */
UTEST(event_ring_drain_increments_event_queue_count)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT(vm.event_ring != NULL);

    uint32_t before = vm.event_queue_count;

    urbi_inject_event(&vm, 1U, NULL, 0U);
    urbi_inject_event(&vm, 2U, NULL, 0U);
    urbi_inject_event(&vm, 3U, NULL, 0U);

    uevent_ring_drain(&vm);

    /* Each drained entry should increment event_queue_count by 1. */
    UASSERT_EQ((long long)vm.event_queue_count, (long long)(before + 3U));
    UASSERT(!uevent_ring_has_pending(vm.event_ring));

    uvm_destroy(&vm);
}

/* ---- Case 11 (Linux-only): multi-threaded fuzz — 100k events ----------- */
#ifdef __linux__
#include <pthread.h>
#include <stdint.h>

#define FUZZ_EVENT_COUNT 100000U

typedef struct {
    UEventRing *ring;
    uint32_t    count;
    uint64_t    checksum;   /* sum of all event_ids injected */
} FuzzProducerArgs;

typedef struct {
    UEventRing *ring;
    uint32_t    count;
    uint64_t    checksum;   /* sum of all event_ids drained */
} FuzzConsumerArgs;

static void *fuzz_producer(void *arg)
{
    FuzzProducerArgs *a = (FuzzProducerArgs *)arg;
    uint32_t i;
    for (i = 0U; i < a->count; ) {
        /* Use a minimal VM struct wrapper: we need urbi_inject_event but we
           want to drive the ring directly.  We create a tiny stub VM with
           just event_ring set — sufficient for urbi_inject_event. */
        UVM stub_vm;
        /* Zero the stub so no UB from uninitialized fields in the producer
           path (which only touches vm->event_ring and vm->alloc_fn via the
           NULL guard). */
        memset(&stub_vm, 0, sizeof(stub_vm));
        stub_vm.event_ring = a->ring;
        int rc = urbi_inject_event(&stub_vm, i, NULL, 0U);
        if (rc == URBI_OK) {
            a->checksum += i;
            i++;
        }
        /* On RING_FULL: spin; consumer will drain. */
    }
    return NULL;
}

static void *fuzz_consumer(void *arg)
{
    FuzzConsumerArgs *a = (FuzzConsumerArgs *)arg;
    uint32_t drained = 0U;
    while (drained < a->count) {
        UEventRing *r = a->ring;
        uint32_t w  = __atomic_load_n(&r->write_idx, __ATOMIC_ACQUIRE);
        uint32_t rd = __atomic_load_n(&r->read_idx,  __ATOMIC_RELAXED);
        while (rd != w) {
            UEventRingEntry *e = &r->ring[rd];
            a->checksum += e->event_id;
            rd = (rd + 1U) & (uint32_t)(URBI_EVENT_RING_DEPTH - 1U);
            drained++;
            w = __atomic_load_n(&r->write_idx, __ATOMIC_ACQUIRE);
        }
        __atomic_store_n(&r->read_idx, rd, __ATOMIC_RELEASE);
    }
    return NULL;
}

UTEST(event_ring_multi_thread_fuzz_100k)
{
    UEventRing ring;
    uevent_ring_init(&ring);

    FuzzProducerArgs pargs = { &ring, FUZZ_EVENT_COUNT, 0U };
    FuzzConsumerArgs cargs = { &ring, FUZZ_EVENT_COUNT, 0U };

    pthread_t prod, cons;
    pthread_create(&prod, NULL, fuzz_producer, &pargs);
    pthread_create(&cons, NULL, fuzz_consumer, &cargs);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    /* Both producer and consumer tallied the same sum of 0..N-1.
       This proves no events were lost or duplicated.
       (overflow_count may be positive because the producer spins on RING_FULL,
       incrementing overflow_count on each retry — that is correct behaviour.) */
    uint64_t expected = (uint64_t)FUZZ_EVENT_COUNT * ((uint64_t)FUZZ_EVENT_COUNT - 1U) / 2U;
    UASSERT_EQ((long long)pargs.checksum, (long long)expected);
    UASSERT_EQ((long long)cargs.checksum, (long long)expected);
}
#endif /* __linux__ */

/* ---- Test suite registration ------------------------------------------ */

void test_event_ring_suite(void)
{
    utest_run("event_ring_init_empty",
              event_ring_init_empty);
    utest_run("event_ring_inject_then_pending",
              event_ring_inject_then_pending);
    utest_run("event_ring_overflow_returns_error_with_counter",
              event_ring_overflow_returns_error_with_counter);
    utest_run("event_ring_drain_advances_read_idx",
              event_ring_drain_advances_read_idx);
    utest_run("event_ring_payload_round_trip",
              event_ring_payload_round_trip);
    utest_run("event_ring_payload_too_large_rejected",
              event_ring_payload_too_large_rejected);
    utest_run("event_ring_drain_empty_noop",
              event_ring_drain_empty_noop);
    utest_run("event_ring_overflow_not_bumped_on_success",
              event_ring_overflow_not_bumped_on_success);
    utest_run("event_ring_zero_payload_null_ptr_accepted",
              event_ring_zero_payload_null_ptr_accepted);
    utest_run("event_ring_drain_increments_event_queue_count",
              event_ring_drain_increments_event_queue_count);
#ifdef __linux__
    utest_run("event_ring_multi_thread_fuzz_100k",
              event_ring_multi_thread_fuzz_100k);
#endif
}
