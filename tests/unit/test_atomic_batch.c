/* SPDX-License-Identifier: BSD-3-Clause */
/* test_atomic_batch.c — TDD tests for urbi_atomic_begin/end batch dispatch
 * semantics (Gap R, v0.7.1).
 *
 * Three sub-tests:
 *   1. events_held_during_atomic: injecting events while atomic_active is
 *      true does NOT dispatch them (drain is a no-op); urbi_atomic_end
 *      flushes them all in one pass.
 *   2. events_dispatched_after_atomic_end: verifies the drain handler sees
 *      all batched events after urbi_atomic_end, in order.
 *   3. begin_without_end_accumulates: calling urbi_atomic_begin then
 *      injecting events + calling uevent_ring_drain leaves the ring full
 *      (no events dispatched).  urbi_atomic_end clears state afterward. */

#include "utest.h"
#include "vm/uvm.h"
#include "event/uevent_ring.h"
#include "urbi/urbi.h"

#include <stdint.h>

#define UTEST(name) static void name(void)

/* ---- Capture helper ------------------------------------------------------ */

#define CAPTURE_MAX 8
typedef struct {
    uint32_t event_ids[CAPTURE_MAX];
    uint32_t call_count;
} AtomicCapture;

static AtomicCapture g_atomic_cap;

static void atomic_drain_handler(struct UVM *vm, void *ud,
                                 uint32_t event_id,
                                 UValue payload)
{
    (void)vm; (void)ud; (void)payload;
    if (g_atomic_cap.call_count < (uint32_t)CAPTURE_MAX) {
        g_atomic_cap.event_ids[g_atomic_cap.call_count] = event_id;
        g_atomic_cap.call_count++;
    }
}

/* =========================================================================
 * Sub-test 1: events injected while atomic_active is true are NOT drained
 * by an explicit uevent_ring_drain call (atomic gate holds them).
 * ========================================================================= */

UTEST(events_held_during_atomic)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    g_atomic_cap.call_count = 0;
    urbi_register_event_drain(&vm, atomic_drain_handler, NULL);

    urbi_atomic_begin(&vm);
    UASSERT(vm.atomic_active != 0);

    urbi_inject_event(&vm, 1U, NULL, 0U);
    urbi_inject_event(&vm, 2U, NULL, 0U);

    /* Explicit drain during atomic section — must be a no-op. */
    uevent_ring_drain(&vm);
    UASSERT_EQ((long long)g_atomic_cap.call_count, 0LL);

    /* Ring must still have pending entries (not consumed). */
    UASSERT(uevent_ring_has_pending(vm.event_ring));

    /* Clean up: end the section so urbi_vm_destroy can drain cleanly. */
    urbi_atomic_end(&vm);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: after urbi_atomic_end, all batched events arrive in order.
 * ========================================================================= */

UTEST(events_dispatched_after_atomic_end)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    g_atomic_cap.call_count = 0;
    urbi_register_event_drain(&vm, atomic_drain_handler, NULL);

    urbi_atomic_begin(&vm);
    urbi_inject_event(&vm, 10U, NULL, 0U);
    urbi_inject_event(&vm, 20U, NULL, 0U);
    urbi_inject_event(&vm, 30U, NULL, 0U);

    /* No events yet. */
    UASSERT_EQ((long long)g_atomic_cap.call_count, 0LL);

    urbi_atomic_end(&vm);

    /* All three events dispatched in one flush, in order. */
    UASSERT_EQ((long long)g_atomic_cap.call_count, 3LL);
    UASSERT_EQ((long long)g_atomic_cap.event_ids[0], 10LL);
    UASSERT_EQ((long long)g_atomic_cap.event_ids[1], 20LL);
    UASSERT_EQ((long long)g_atomic_cap.event_ids[2], 30LL);

    /* Ring must be empty after flush. */
    UASSERT(!uevent_ring_has_pending(vm.event_ring));

    /* atomic_active must be cleared by urbi_atomic_end. */
    UASSERT(vm.atomic_active == 0);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: begin-without-end — events accumulate, ring stays full after
 * an explicit drain call (which is a no-op while atomic_active).
 * urbi_atomic_end is called at the end to avoid leaving stale state.
 * ========================================================================= */

UTEST(begin_without_end_accumulates)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    g_atomic_cap.call_count = 0;
    urbi_register_event_drain(&vm, atomic_drain_handler, NULL);

    urbi_atomic_begin(&vm);
    urbi_inject_event(&vm, 42U, NULL, 0U);

    /* Explicit drain — no-op because atomic_active. */
    uevent_ring_drain(&vm);
    UASSERT_EQ((long long)g_atomic_cap.call_count, 0LL);
    UASSERT(uevent_ring_has_pending(vm.event_ring));

    /* Clear state properly so urbi_vm_destroy sees a clean VM. */
    urbi_atomic_end(&vm);
    UASSERT_EQ((long long)g_atomic_cap.call_count, 1LL);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point
 * ========================================================================= */

void
test_atomic_batch_suite(void)
{
    utest_run("atomic: drain no-op while atomic_active (events held)",
              events_held_during_atomic);
    utest_run("atomic: all batched events dispatched after urbi_atomic_end",
              events_dispatched_after_atomic_end);
    utest_run("atomic: begin-without-end accumulates events in ring",
              begin_without_end_accumulates);
}
