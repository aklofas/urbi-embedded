/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: ISR drain handler (T57 / spec #3 §9).
   Covers registration, per-entry dispatch, fallback to event_queue_count,
   and NULL-handler removal. */

#include "utest.h"
#include "vm/uvm.h"
#include "uevent_ring.h"
#include "urbi/urbi.h"
#include <stdint.h>
#include <stdio.h>

#define UTEST(name) static void name(void)

/* ---- helpers ----------------------------------------------------------- */

/* Drain-capture context: records up to 8 calls. */
#define CAPTURE_MAX 8
typedef struct {
    uint32_t event_ids[CAPTURE_MAX];
    uint8_t  payload_kinds[CAPTURE_MAX];
    uint32_t call_count;
} DrainCapture;

static DrainCapture g_capture;

static void capture_drain_handler(struct UVM *vm,
                                  uint32_t event_id,
                                  UValue payload)
{
    (void)vm;
    if (g_capture.call_count < (uint32_t)CAPTURE_MAX) {
        g_capture.event_ids[g_capture.call_count]    = event_id;
        g_capture.payload_kinds[g_capture.call_count] = payload.kind;
        g_capture.call_count++;
    }
}

/* ---- Case 1: registered handler called once per ring entry ------------- */
UTEST(drain_handler_called_per_entry)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    g_capture.call_count = 0u;
    urbi_register_event_drain(&vm, capture_drain_handler);

    urbi_inject_event(&vm, 10u, NULL, 0u);
    urbi_inject_event(&vm, 20u, NULL, 0u);
    urbi_inject_event(&vm, 30u, NULL, 0u);

    uevent_ring_drain(&vm);

    UASSERT_EQ((long long)g_capture.call_count, 3LL);
    UASSERT_EQ((long long)g_capture.event_ids[0], 10LL);
    UASSERT_EQ((long long)g_capture.event_ids[1], 20LL);
    UASSERT_EQ((long long)g_capture.event_ids[2], 30LL);

    /* Ring must be empty after drain. */
    UASSERT(!uevent_ring_has_pending(vm.event_ring));

    uvm_destroy(&vm);
}

/* ---- Case 2: handler receives NIL payload ------------------------------ */
UTEST(drain_handler_payload_is_nil)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    g_capture.call_count = 0u;
    urbi_register_event_drain(&vm, capture_drain_handler);

    urbi_inject_event(&vm, 99u, NULL, 0u);
    uevent_ring_drain(&vm);

    UASSERT_EQ((long long)g_capture.call_count, 1LL);
    UASSERT_EQ((long long)g_capture.payload_kinds[0], (long long)UVAL_NIL);

    uvm_destroy(&vm);
}

/* ---- Case 3: no handler → falls back to event_queue_count -------------- */
UTEST(no_handler_increments_event_queue_count)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* No drain handler registered (default NULL from uvm_init). */
    uint32_t before = vm.event_queue_count;

    urbi_inject_event(&vm, 1u, NULL, 0u);
    urbi_inject_event(&vm, 2u, NULL, 0u);
    uevent_ring_drain(&vm);

    UASSERT_EQ((long long)vm.event_queue_count, (long long)(before + 2u));
    UASSERT(!uevent_ring_has_pending(vm.event_ring));

    uvm_destroy(&vm);
}

/* ---- Case 4: NULL-handler removal stops dispatch ----------------------- */
UTEST(null_handler_removes_drain_callback)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    g_capture.call_count = 0u;
    urbi_register_event_drain(&vm, capture_drain_handler);
    /* Now remove it. */
    urbi_register_event_drain(&vm, NULL);

    urbi_inject_event(&vm, 7u, NULL, 0u);
    uint32_t before = vm.event_queue_count;
    uevent_ring_drain(&vm);

    /* Handler must NOT have been called. */
    UASSERT_EQ((long long)g_capture.call_count, 0LL);
    /* Fallback path must have incremented event_queue_count. */
    UASSERT_EQ((long long)vm.event_queue_count, (long long)(before + 1u));

    uvm_destroy(&vm);
}

/* ---- Case 5: drain on empty ring does not call handler ----------------- */
UTEST(drain_empty_ring_does_not_call_handler)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    g_capture.call_count = 0u;
    urbi_register_event_drain(&vm, capture_drain_handler);

    UASSERT(!uevent_ring_has_pending(vm.event_ring));
    uevent_ring_drain(&vm);

    UASSERT_EQ((long long)g_capture.call_count, 0LL);

    uvm_destroy(&vm);
}

/* ---- Case 6: handler receives correct event_id ordering (FIFO) --------- */
UTEST(drain_handler_fifo_ordering)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    g_capture.call_count = 0u;
    urbi_register_event_drain(&vm, capture_drain_handler);

    urbi_inject_event(&vm, 100u, NULL, 0u);
    urbi_inject_event(&vm, 200u, NULL, 0u);
    urbi_inject_event(&vm, 300u, NULL, 0u);
    urbi_inject_event(&vm, 400u, NULL, 0u);

    uevent_ring_drain(&vm);

    UASSERT_EQ((long long)g_capture.call_count, 4LL);
    UASSERT_EQ((long long)g_capture.event_ids[0], 100LL);
    UASSERT_EQ((long long)g_capture.event_ids[1], 200LL);
    UASSERT_EQ((long long)g_capture.event_ids[2], 300LL);
    UASSERT_EQ((long long)g_capture.event_ids[3], 400LL);

    uvm_destroy(&vm);
}

/* ---- Test suite registration ------------------------------------------ */

void test_isr_event_drain_suite(void)
{
    printf("test_isr_event_drain\n");
    utest_run("drain_handler_called_per_entry",
              drain_handler_called_per_entry);
    utest_run("drain_handler_payload_is_nil",
              drain_handler_payload_is_nil);
    utest_run("no_handler_increments_event_queue_count",
              no_handler_increments_event_queue_count);
    utest_run("null_handler_removes_drain_callback",
              null_handler_removes_drain_callback);
    utest_run("drain_empty_ring_does_not_call_handler",
              drain_empty_ring_does_not_call_handler);
    utest_run("drain_handler_fifo_ordering",
              drain_handler_fifo_ordering);
}
