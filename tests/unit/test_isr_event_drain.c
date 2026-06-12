/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: ISR drain handler (T57 / spec #3 §9).
   Covers registration, per-entry dispatch, ring consumption without a
   handler, and NULL-handler removal.  (The M3-compat event_queue_count
   fallback was removed at T26; the vestigial counter itself was deleted
   at v0.13.3 / SCHED-13 — ring pendingness is queried live via
   uevent_ring_has_pending.) */

#include "utest.h"
#include "vm/uvm.h"
#include "event/uevent_ring.h"
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

static void capture_drain_handler(struct UVM *vm, void *ud,
                                  uint32_t event_id,
                                  UValue payload)
{
    (void)vm; (void)ud;
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
    urbi_vm_init(&vm, NULL, NULL);

    g_capture.call_count = 0U;
    urbi_register_event_drain(&vm, capture_drain_handler, NULL);

    urbi_inject_event(&vm, 10U, NULL, 0U);
    urbi_inject_event(&vm, 20U, NULL, 0U);
    urbi_inject_event(&vm, 30U, NULL, 0U);

    uevent_ring_drain(&vm);

    UASSERT_EQ((long long)g_capture.call_count, 3LL);
    UASSERT_EQ((long long)g_capture.event_ids[0], 10LL);
    UASSERT_EQ((long long)g_capture.event_ids[1], 20LL);
    UASSERT_EQ((long long)g_capture.event_ids[2], 30LL);

    /* Ring must be empty after drain. */
    UASSERT(!uevent_ring_has_pending(vm.event_ring));

    urbi_vm_destroy(&vm);
}

/* ---- Case 2: handler receives NIL payload ------------------------------ */
UTEST(drain_handler_payload_is_nil)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    g_capture.call_count = 0U;
    urbi_register_event_drain(&vm, capture_drain_handler, NULL);

    urbi_inject_event(&vm, 99U, NULL, 0U);
    uevent_ring_drain(&vm);

    UASSERT_EQ((long long)g_capture.call_count, 1LL);
    UASSERT_EQ((long long)g_capture.payload_kinds[0], (long long)UVAL_NIL);

    urbi_vm_destroy(&vm);
}

/* ---- Case 3 (T26 / EVENT-017): no handler → entries silently discarded -- */
UTEST(no_handler_silently_discards_entries)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* No drain handler registered (default NULL from urbi_vm_init). */
    urbi_inject_event(&vm, 1U, NULL, 0U);
    urbi_inject_event(&vm, 2U, NULL, 0U);
    uevent_ring_drain(&vm);

    /* T26: M3-compat fallback removed; entries are consumed from the ring
     * (read_idx advances) but don't surface as UEvents in the scheduler.
     * (The vestigial event_queue_count counter was deleted at v0.13.3.) */
    UASSERT(!uevent_ring_has_pending(vm.event_ring));

    urbi_vm_destroy(&vm);
}

/* ---- Case 4: NULL-handler removal stops dispatch ----------------------- */
UTEST(null_handler_removes_drain_callback)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    g_capture.call_count = 0U;
    urbi_register_event_drain(&vm, capture_drain_handler, NULL);
    /* Now remove it. */
    urbi_register_event_drain(&vm, NULL, NULL);

    urbi_inject_event(&vm, 7U, NULL, 0U);
    uevent_ring_drain(&vm);

    /* Handler must NOT have been called; the entry is consumed anyway
     * (T26: discarded, no M3-compat counter fallback). */
    UASSERT_EQ((long long)g_capture.call_count, 0LL);
    UASSERT(!uevent_ring_has_pending(vm.event_ring));

    urbi_vm_destroy(&vm);
}

/* ---- Case 5: drain on empty ring does not call handler ----------------- */
UTEST(drain_empty_ring_does_not_call_handler)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    g_capture.call_count = 0U;
    urbi_register_event_drain(&vm, capture_drain_handler, NULL);

    UASSERT(!uevent_ring_has_pending(vm.event_ring));
    uevent_ring_drain(&vm);

    UASSERT_EQ((long long)g_capture.call_count, 0LL);

    urbi_vm_destroy(&vm);
}

/* ---- Case 6: handler receives correct event_id ordering (FIFO) --------- */
UTEST(drain_handler_fifo_ordering)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    g_capture.call_count = 0U;
    urbi_register_event_drain(&vm, capture_drain_handler, NULL);

    urbi_inject_event(&vm, 100U, NULL, 0U);
    urbi_inject_event(&vm, 200U, NULL, 0U);
    urbi_inject_event(&vm, 300U, NULL, 0U);
    urbi_inject_event(&vm, 400U, NULL, 0U);

    uevent_ring_drain(&vm);

    UASSERT_EQ((long long)g_capture.call_count, 4LL);
    UASSERT_EQ((long long)g_capture.event_ids[0], 100LL);
    UASSERT_EQ((long long)g_capture.event_ids[1], 200LL);
    UASSERT_EQ((long long)g_capture.event_ids[2], 300LL);
    UASSERT_EQ((long long)g_capture.event_ids[3], 400LL);

    urbi_vm_destroy(&vm);
}

/* ---- Test suite registration ------------------------------------------ */

void test_isr_event_drain_suite(void)
{
    printf("test_isr_event_drain\n");
    utest_run("drain_handler_called_per_entry",
              drain_handler_called_per_entry);
    utest_run("drain_handler_payload_is_nil",
              drain_handler_payload_is_nil);
    utest_run("no_handler_silently_discards_entries",
              no_handler_silently_discards_entries);
    utest_run("null_handler_removes_drain_callback",
              null_handler_removes_drain_callback);
    utest_run("drain_empty_ring_does_not_call_handler",
              drain_empty_ring_does_not_call_handler);
    utest_run("drain_handler_fifo_ordering",
              drain_handler_fifo_ordering);
}
