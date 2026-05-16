/* SPDX-License-Identifier: BSD-3-Clause */
/* Regression test for the at-handler dispatch event-loss bug found
 * while debugging the ESP-IDF eye_demo port.
 *
 * Original symptom on hardware: high-frequency blob_seen events at 5-25 Hz
 * caused at-handler bodies to stop firing after a few seconds.  Bisected
 * via the variants below to v0.7.1's `URBI_WATCHER_PENDING_REFIRE` flag —
 * a single bit that collapsed N events arriving while a body was in flight
 * into ONE pending refire, capping `uevent_ring_drain` throughput at 2
 * firings per drain call regardless of how many events were in the ring.
 *
 * The fix (v0.7.x) widened PENDING_REFIRE from a flag bit to a uint8_t
 * counter (pending_refire_count) bounded by max_refire_queue (default 15,
 * matching URBI_EVENT_RING_DEPTH-1).  This file's variants exercise the
 * full pipeline at counts that would have wedged under the old code; all
 * should pass under the new counter.  Variants are listed in order of
 * increasing eye_demo fidelity:
 *
 *   1. SINGLE_TRIVIAL: `at (e?) Realm.counter = Realm.counter + 1` —
 *      inject + drain + step 1000 times; assert counter == 1000.  If
 *      this fails on host, the runtime is unambiguously the bug.
 *
 *   2. SINGLE_HOST_FN: destructure_fn writes a Realm slot; handler is
 *      `at (e?) Realm.b = Realm.x`.  Mirrors eye_demo's slot-routing
 *      pattern more closely.  Tests whether destructure-side Realm
 *      writes interact badly with the handler dispatch.
 *
 *   3. CHAINED_THREE: three chained handlers on the same event,
 *      mirroring the original eye_demo blob_seen chain that wedged
 *      fastest in production.
 *
 * Pass criteria: each variant should reach its expected counter value.
 * Failure mode (= bug reproduced on host): final counter < expected,
 * indicating that handler dispatch stopped firing at some iteration N.
 *
 * If all three pass, the bug requires something the host doesn't have
 * — extend `tests/qemu/reactive_smoke/` next.
 */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "event/uevent_ring.h"
#include "event/uevent_registry.h"

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* How many events to fire per variant.  Tuned to exceed the observed
 * eye_demo wedge threshold (~200 events at 5 Hz on hardware) by 5×, so
 * any threshold-based bug should be reliably triggered. */
#define WEDGE_REPRO_EVENTS 1000

/* Trivial destructure_fn: returns 0 args (no payload threading).  Used
 * by SINGLE_TRIVIAL where the at-body has no parameters. */
static int
noop_destruct_fn(struct UVM *vm,
                 const urbi_event_payload_t *payload, size_t payload_len,
                 UValue *out_args, int max_args, void *ud)
{
    (void)vm; (void)payload; (void)payload_len;
    (void)out_args; (void)max_args; (void)ud;
    return 0;
}

/* Slot-writing destructure_fn: reads payload.u32[0] and writes it into
 * Realm.x.  Mirrors eye_demo's destructure_blob (which writes
 * Realm.last_blob_x via urbi_realm_set_global from the safepoint
 * drain).  Returns 0 args (same routing pattern eye_demo uses). */
static int
slot_writing_destruct_fn(struct UVM *vm,
                         const urbi_event_payload_t *payload, size_t payload_len,
                         UValue *out_args, int max_args, void *ud)
{
    (void)out_args; (void)max_args; (void)ud;
    if (payload == NULL || payload_len < sizeof(uint32_t)) {
        return 0;
    }
    struct URealm *r = urbi_realm_global(vm);
    if (r != NULL) {
        urbi_realm_set_global(vm, r, "x", 1U,
                              utest_e2e_make_int((int64_t)payload->u32[0]));
    }
    return 0;
}

/* Helper: pump one event end-to-end (inject -> drain -> drive strands
 * to quiescence).  Returns 1 if the event reached drain quiescence
 * without a fatal strand, 0 on iteration-cap exhaustion, -1 on fatal. */
static int
pump_one_event(UVM *vm, urbi_event_id_t id,
               const urbi_event_payload_t *payload, size_t payload_len)
{
    int rc = urbi_inject_event(vm, (uint32_t)id, payload, payload_len);
    if (rc != URBI_OK) return -1;
    uevent_ring_drain(vm);
    return utest_e2e_run_to_no_runnable(vm);
}

/* =========================================================================
 * Variant 1: single trivial at-handler, no payload.
 *
 *   `at (e?) Realm.counter = Realm.counter + 1`
 *
 * Loop 1000 events.  Expected: Realm.counter == 1000 at end.
 * If counter < 1000, runtime dispatched the body N < 1000 times — that
 * N is the wedge threshold on host.
 * ========================================================================= */
UTEST(wedge_single_trivial_handler)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    int rc = urbi_realm_set_global(&vm, r, "counter", 7,
                                   utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    urbi_event_id_t id = urbi_event_register(&vm, r, "e",
                                              noop_destruct_fn, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    rc = utest_e2e_compile_and_run(&vm,
        "at (e?) Realm.counter = Realm.counter + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    int last_seen = 0;
    int wedge_iter = -1;
    for (int i = 0; i < WEDGE_REPRO_EVENTS; i++) {
        int q = pump_one_event(&vm, id, NULL, 0U);
        if (q < 0) {
            fprintf(stderr, "FATAL strand at iter %d\n", i);
            break;
        }
        /* Spot-check progress every 100 iters; record first stall. */
        if ((i % 100) == 99) {
            UValue v = utest_e2e_make_nil();
            (void)urbi_realm_get_global(&vm, r, "counter", 7, &v);
            int now = (int)v.v.i;
            if (now == last_seen && wedge_iter < 0) {
                wedge_iter = i;
                fprintf(stderr,
                        "WEDGE: counter stalled at %d after iter %d "
                        "(100 events fired with zero handler increments)\n",
                        now, i);
            }
            last_seen = now;
        }
    }

    UValue counter = utest_e2e_make_nil();
    rc = urbi_realm_get_global(&vm, r, "counter", 7, &counter);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)counter.kind);
    if ((int)counter.v.i != WEDGE_REPRO_EVENTS) {
        fprintf(stderr,
                "WEDGE REPRODUCED on host: expected counter=%d, got %lld "
                "(wedge threshold ≈ %d events)\n",
                WEDGE_REPRO_EVENTS, (long long)counter.v.i, wedge_iter);
    }
    UASSERT_EQ((int64_t)WEDGE_REPRO_EVENTS, counter.v.i);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Variant 2: single at-handler with destructure-fn slot routing.
 *
 *   destructure_fn writes Realm.x = payload.u32[0]
 *   `at (e?) Realm.b = Realm.x`
 *
 * Mirrors eye_demo's slot-routing dispatch pattern (destructure writes
 * a Realm slot from the safepoint drain; the at-handler reads it).
 * Tests whether the urbi_realm_set_global call from destructure
 * interacts with subsequent handler dispatch.
 * ========================================================================= */
UTEST(wedge_single_host_fn_handler)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    int rc;
    rc = urbi_realm_set_global(&vm, r, "x", 1, utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);
    rc = urbi_realm_set_global(&vm, r, "b", 1, utest_e2e_make_int(-1));
    UASSERT_EQ(URBI_OK, rc);

    urbi_event_id_t id = urbi_event_register(&vm, r, "e2",
                                              slot_writing_destruct_fn, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    rc = utest_e2e_compile_and_run(&vm,
        "at (e2?) Realm.b = Realm.x",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    for (int i = 0; i < WEDGE_REPRO_EVENTS; i++) {
        urbi_event_payload_t p = {0};
        p.u32[0] = (uint32_t)i;
        int q = pump_one_event(&vm, id, &p, sizeof(p));
        if (q < 0) {
            fprintf(stderr, "FATAL strand at iter %d\n", i);
            break;
        }
    }

    /* If dispatch worked end-to-end, Realm.b should equal the last
     * payload value (WEDGE_REPRO_EVENTS - 1).  If dispatch wedged
     * after K events, Realm.b == K-1 — gives the wedge threshold. */
    UValue b = utest_e2e_make_nil();
    rc = urbi_realm_get_global(&vm, r, "b", 1, &b);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)b.kind);
    if ((int)b.v.i != WEDGE_REPRO_EVENTS - 1) {
        fprintf(stderr,
                "WEDGE REPRODUCED on host (slot-routing variant): "
                "expected b=%d, got %lld\n",
                WEDGE_REPRO_EVENTS - 1, (long long)b.v.i);
    }
    UASSERT_EQ((int64_t)(WEDGE_REPRO_EVENTS - 1), b.v.i);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Variant 3: three chained at-handlers on the same event.
 *
 *   `at (e?) Realm.a = Realm.a + 1`
 *   `at (e?) Realm.b = Realm.b + 1`
 *   `at (e?) Realm.c = Realm.c + 1`
 *
 * Mirrors the original eye_demo blob_seen chain (3 handlers per event)
 * that wedged fastest in production.  Loop is shorter (300 events) so
 * total handler invocations match Variant 1 (300 × 3 = 900 ≈ 1000).
 * ========================================================================= */
UTEST(wedge_chained_three_handlers)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    int rc;
    rc = urbi_realm_set_global(&vm, r, "a", 1, utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);
    rc = urbi_realm_set_global(&vm, r, "b", 1, utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);
    rc = urbi_realm_set_global(&vm, r, "c", 1, utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    urbi_event_id_t id = urbi_event_register(&vm, r, "e3",
                                              noop_destruct_fn, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* Chunk-level statements need `;` separators (newlines alone don't
     * terminate urbi statements; the parser would read all three lines
     * as one runaway expression). */
    rc = utest_e2e_compile_and_run(&vm,
        "at (e3?) Realm.a = Realm.a + 1;"
        "at (e3?) Realm.b = Realm.b + 1;"
        "at (e3?) Realm.c = Realm.c + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    const int chained_events = WEDGE_REPRO_EVENTS / 3;  /* ≈333 */
    for (int i = 0; i < chained_events; i++) {
        int q = pump_one_event(&vm, id, NULL, 0U);
        if (q < 0) {
            fprintf(stderr, "FATAL strand at iter %d\n", i);
            break;
        }
    }

    UValue a = utest_e2e_make_nil();
    UValue b = utest_e2e_make_nil();
    UValue c = utest_e2e_make_nil();
    rc = urbi_realm_get_global(&vm, r, "a", 1, &a);
    UASSERT_EQ(URBI_OK, rc);
    rc = urbi_realm_get_global(&vm, r, "b", 1, &b);
    UASSERT_EQ(URBI_OK, rc);
    rc = urbi_realm_get_global(&vm, r, "c", 1, &c);
    UASSERT_EQ(URBI_OK, rc);

    if ((int)a.v.i != chained_events || (int)b.v.i != chained_events
            || (int)c.v.i != chained_events) {
        fprintf(stderr,
                "WEDGE REPRODUCED on host (chained variant): "
                "expected a=b=c=%d, got a=%lld b=%lld c=%lld\n",
                chained_events,
                (long long)a.v.i, (long long)b.v.i, (long long)c.v.i);
    }
    UASSERT_EQ((int64_t)chained_events, a.v.i);
    UASSERT_EQ((int64_t)chained_events, b.v.i);
    UASSERT_EQ((int64_t)chained_events, c.v.i);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Variant 4: batched drain — inject N events back-to-back, then drain.
 *
 * On host the previous variants serialize inject + drain + step per event;
 * on ESP the camera task can inject several events into the ring before
 * the urbi task gets to drain them.  This variant mimics that: inject
 * 16 events (the SPSC ring depth - 1) then drain once and step.  After
 * BATCHES iterations the handler should have fired BATCHES * 16 times.
 *
 * If this wedges, the bug is in batched-drain dispatch (one drain call
 * spawning multiple strands at once).  If it passes, the bug requires
 * true concurrent injection from a different thread / ISR.
 * ========================================================================= */
UTEST(wedge_batched_drain)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    int rc = urbi_realm_set_global(&vm, r, "counter", 7,
                                   utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    urbi_event_id_t id = urbi_event_register(&vm, r, "e4",
                                              noop_destruct_fn, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    rc = utest_e2e_compile_and_run(&vm,
        "at (e4?) Realm.counter = Realm.counter + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Ring depth is 16 (URBI_EVENT_RING_DEPTH); usable capacity is 15
     * (one slot reserved to distinguish full from empty).  Inject 15
     * per batch so urbi_inject_event never returns URBI_ERR_EVENT_RING_FULL. */
    const int events_per_batch = 15;
    const int batches = 60;  /* total = 900 events */
    int total_injected = 0;
    for (int b = 0; b < batches; b++) {
        for (int i = 0; i < events_per_batch; i++) {
            int irc = urbi_inject_event(&vm, (uint32_t)id, NULL, 0U);
            if (irc == URBI_OK) total_injected++;
        }
        uevent_ring_drain(&vm);
        int q = utest_e2e_run_to_no_runnable(&vm);
        if (q < 0) {
            fprintf(stderr, "FATAL strand at batch %d\n", b);
            break;
        }
    }

    UValue counter = utest_e2e_make_nil();
    rc = urbi_realm_get_global(&vm, r, "counter", 7, &counter);
    UASSERT_EQ(URBI_OK, rc);
    if ((int)counter.v.i != total_injected) {
        fprintf(stderr,
                "WEDGE REPRODUCED on host (batched-drain variant): "
                "injected %d, counter=%lld\n",
                total_injected, (long long)counter.v.i);
    }
    UASSERT_EQ((int64_t)total_injected, counter.v.i);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Variant 5: with wake_fn registered.
 *
 * The ESP port registers a wake_fn via urbi_set_wake_fn.  If something
 * about wake_fn presence affects the spawn path (e.g. an ACQUIRE/RELEASE
 * fence reorders state) it would show up here.  Wake hook is a no-op
 * counter so we can detect that it actually fires.
 * ========================================================================= */
static uint32_t g_wake_count = 0;
static void test_wake_fn(void *ud)
{
    (void)ud;
    g_wake_count++;
}

UTEST(wedge_with_wake_fn)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    g_wake_count = 0;
    urbi_set_wake_fn(&vm, test_wake_fn, NULL);

    struct URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    int rc = urbi_realm_set_global(&vm, r, "counter", 7,
                                   utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    urbi_event_id_t id = urbi_event_register(&vm, r, "e5",
                                              noop_destruct_fn, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    rc = utest_e2e_compile_and_run(&vm,
        "at (e5?) Realm.counter = Realm.counter + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    for (int i = 0; i < WEDGE_REPRO_EVENTS; i++) {
        int q = pump_one_event(&vm, id, NULL, 0U);
        if (q < 0) break;
    }

    UValue counter = utest_e2e_make_nil();
    rc = urbi_realm_get_global(&vm, r, "counter", 7, &counter);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int64_t)WEDGE_REPRO_EVENTS, counter.v.i);

    /* wake_fn should fire exactly once per inject. */
    UASSERT_EQ((uint32_t)WEDGE_REPRO_EVENTS, g_wake_count);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Variant 6: fine-grained characterization of the batched-drain wedge.
 *
 * Records counter after EVERY batch, plus VM internal state (strand_runnable_count,
 * watcher_pool_in_use, ring overflow_count) at the wedge.  Tells us:
 *   - exact batch at which dispatch first failed
 *   - whether wedge is partial (some events fired) or full (zero fired)
 *   - what internal state is stuck
 *   - whether the wedge is per-batch threshold or cumulative-events threshold
 * ========================================================================= */
UTEST(wedge_batched_drain_characterization)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    int rc = urbi_realm_set_global(&vm, r, "counter", 7,
                                   utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    urbi_event_id_t id = urbi_event_register(&vm, r, "e6",
                                              noop_destruct_fn, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    rc = utest_e2e_compile_and_run(&vm,
        "at (e6?) Realm.counter = Realm.counter + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    const int events_per_batch = 15;
    const int batches = 12;
    int prev_counter = 0;
    int wedge_batch = -1;

    for (int b = 0; b < batches; b++) {
        for (int i = 0; i < events_per_batch; i++) {
            (void)urbi_inject_event(&vm, (uint32_t)id, NULL, 0U);
        }
        uevent_ring_drain(&vm);
        int q = utest_e2e_run_to_no_runnable(&vm);
        if (q < 0) {
            fprintf(stderr, "  FATAL strand at batch %d\n", b);
            break;
        }

        UValue v = utest_e2e_make_nil();
        (void)urbi_realm_get_global(&vm, r, "counter", 7, &v);
        int now = (int)v.v.i;
        int delta = now - prev_counter;
        if (delta < events_per_batch && wedge_batch < 0) {
            /* Pre-fix this triggered on batch 0 (delta=2 for batch=15).
             * Post-fix it should never trigger.  When it DOES trigger,
             * dump the full per-batch table — that diagnostic detail is
             * what the original wedge investigation relied on. */
            wedge_batch = b;
            fprintf(stderr,
                "WEDGE: batch %d, counter=%d, delta=%d (expected %d), "
                "runnable=%u, pool_in_use=%u, overflow=%u\n",
                b, now, delta, events_per_batch,
                (unsigned)vm.strand_runnable_count,
                (unsigned)vm.watcher_pool_in_use,
                (unsigned)(vm.event_ring ? vm.event_ring->overflow_count : 0U));
        }
        prev_counter = now;
    }

    /* Diagnostic only — never asserts the final counter (the asserting
     * version is wedge_batched_drain above).  Stays quiet on the happy
     * path; only logs when a regression brings the wedge back. */
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Variant 7: smaller batches to find the per-batch threshold.
 *
 * If batch=15 wedges and batch=1 doesn't, what's the threshold?  Run
 * with several batch sizes and report the wedge point for each.
 * ========================================================================= */
static void run_batch_size_probe(int events_per_batch, int max_batches)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    struct URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);
    int rc = urbi_realm_set_global(&vm, r, "counter", 7, utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);
    urbi_event_id_t id = urbi_event_register(&vm, r, "eprobe",
                                              noop_destruct_fn, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);
    rc = utest_e2e_compile_and_run(&vm,
        "at (eprobe?) Realm.counter = Realm.counter + 1", NULL);
    UASSERT_EQ(URBI_OK, rc);

    int prev = 0;
    int wedge_at = -1;
    for (int b = 0; b < max_batches; b++) {
        for (int i = 0; i < events_per_batch; i++) {
            (void)urbi_inject_event(&vm, (uint32_t)id, NULL, 0U);
        }
        uevent_ring_drain(&vm);
        if (utest_e2e_run_to_no_runnable(&vm) < 0) break;
        UValue v = utest_e2e_make_nil();
        (void)urbi_realm_get_global(&vm, r, "counter", 7, &v);
        int now = (int)v.v.i;
        int delta = now - prev;
        if (delta < events_per_batch && wedge_at < 0) {
            wedge_at = b;
            fprintf(stderr,
                "  batch_size=%2d: wedge at batch %d, final counter=%d "
                "(expected %d), strand_runnable=%u\n",
                events_per_batch, b, now, (b + 1) * events_per_batch,
                (unsigned)vm.strand_runnable_count);
            break;
        }
        prev = now;
    }
    /* Stays quiet on the happy path; only the WEDGE branch above prints. */
    urbi_vm_destroy(&vm);
}

UTEST(wedge_batch_size_probe)
{
    /* Probe stays quiet on the happy path; only prints when a wedge
     * surfaces (see run_batch_size_probe).  Pre-fix output showed
     * batch_size>=3 wedging at batch 0; post-fix all probes complete. */
    run_batch_size_probe(1,  200);
    run_batch_size_probe(2,  100);
    run_batch_size_probe(3,   80);
    run_batch_size_probe(5,   60);
    run_batch_size_probe(8,   50);
    run_batch_size_probe(10,  40);
    run_batch_size_probe(15,  30);
}

/* =========================================================================
 * Variant 8: eye_demo-shaped interleave — two event types, three chained
 *            button handlers, blob events firing between presses.
 *
 * Mirrors the ESP eye_demo workload exactly:
 *   - blob_seen: destructure_fn writes 3 Realm slots, returns 3 args;
 *     single at-handler `at (blob_seen?) host_fn(Realm.last_blob_x, ...)`
 *   - button_pressed: no destructure_fn (returns 0); THREE chained at-
 *     handlers (advance index / call host fn / log via host fn)
 *   - Workload: 5 blob events, then 1 button event, then 5 blob events,
 *     then 1 button event ... for 20 button events total (100 blob).
 *
 * Pass criterion: blob handler fires 100 times AND button handlers fire
 * 20 times each.  Failure mode (= wedge reproduced) is the most recent
 * eye_demo symptom: handlers stop spawning after press 3.
 * ========================================================================= */
static int g_host_blob_calls = 0;
static int g_host_btn_calls  = 0;

static int host_blob_seen(struct UVM *vm, UValue self,
                          UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    g_host_blob_calls++;
    if (out) *out = urbi_make_nil();
    return UEXEC_OK;
}

static int host_btn_apply(struct UVM *vm, UValue self,
                          UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    g_host_btn_calls++;
    if (out) *out = urbi_make_nil();
    return UEXEC_OK;
}

UTEST(wedge_eye_demo_interleave)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    g_host_blob_calls = 0;
    g_host_btn_calls  = 0;

    int rc;
    rc = urbi_realm_set_global(&vm, r, "last_blob_x", 11, utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);
    rc = urbi_realm_set_global(&vm, r, "last_blob_y", 11, utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);
    rc = urbi_realm_set_global(&vm, r, "zone",         4, utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);
    rc = urbi_realm_set_global(&vm, r, "next_zone",    9, utest_e2e_make_int(1));
    UASSERT_EQ(URBI_OK, rc);

    /* Register two host fns. */
    UASSERT_EQ(URBI_OK, urbi_register(&vm, r, "host_blob_seen", host_blob_seen));
    UASSERT_EQ(URBI_OK, urbi_register(&vm, r, "host_btn_apply", host_btn_apply));

    /* Register two events. */
    urbi_event_id_t ev_blob = urbi_event_register(&vm, r, "ev_blob",
                                                   slot_writing_destruct_fn, NULL);
    UASSERT(ev_blob != URBI_EVENT_ID_INVALID);
    urbi_event_id_t ev_btn  = urbi_event_register(&vm, r, "ev_btn",
                                                   noop_destruct_fn, NULL);
    UASSERT(ev_btn != URBI_EVENT_ID_INVALID);

    /* Install script: 1 blob handler + 3 chained button handlers, same shape
     * as eye_demo.u.  Realm-slot routing for blob; chained handlers for
     * button. */
    rc = utest_e2e_compile_and_run(&vm,
        "at (ev_blob?) host_blob_seen(Realm.x);"
        "at (ev_btn?)  Realm.zone = Realm.next_zone;"
        "at (ev_btn?)  host_btn_apply(Realm.zone);"
        "at (ev_btn?)  Realm.zone = Realm.zone",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Workload: 5 blob events + 1 button event, repeated. */
    const int rounds = 20;
    const int blobs_per_round = 5;
    for (int round = 0; round < rounds; round++) {
        for (int i = 0; i < blobs_per_round; i++) {
            urbi_event_payload_t p = {0};
            p.u32[0] = (uint32_t)(round * 1000 + i);
            (void)pump_one_event(&vm, ev_blob, &p, sizeof(p));
        }
        (void)pump_one_event(&vm, ev_btn, NULL, 0U);
    }

    if (g_host_blob_calls != rounds * blobs_per_round
            || g_host_btn_calls != rounds) {
        fprintf(stderr,
                "WEDGE REPRODUCED (eye_demo interleave): "
                "blob fires=%d (expected %d), btn fires=%d (expected %d)\n",
                g_host_blob_calls, rounds * blobs_per_round,
                g_host_btn_calls,  rounds);
    }
    UASSERT_EQ(rounds * blobs_per_round, g_host_blob_calls);
    UASSERT_EQ(rounds, g_host_btn_calls);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Variant 9: at-body containing `.get(N)` on a Realm-stored List.
 *
 * eye_demo observation (2026-05-15): on ESP, at-handler bodies whose body
 * contains a `.get(N)` somewhere (e.g. `Realm.list.get(N).method()` or
 * `Realm.list.get(N).field`) silently no-op — the body runs to OP_RET
 * (strands=0, no throw warning) but the call inside the body produces
 * no visible effect.  Bodies WITHOUT .get() work fine.
 *
 * Probe each shape independently.  If any of these fail on host with
 * counter < expected, the runtime is reproducing the ESP behaviour.
 * ========================================================================= */

UTEST(wedge_at_body_with_list_get_int)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    struct URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    int rc;
    rc = urbi_realm_set_global(&vm, r, "counter", 7, utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    urbi_event_id_t id = urbi_event_register(&vm, r, "ev9",
                                              noop_destruct_fn, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* Body: Realm.counter = Realm.next.get(0); where next is a List of ints.
     * Mirrors eye_demo's button handler 1 pattern. */
    rc = utest_e2e_compile_and_run(&vm,
        "Realm.next = List.new(7, 8, 9);"
        "at (ev9?) Realm.counter = Realm.next.get(0)",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    for (int i = 0; i < 5; i++) {
        (void)pump_one_event(&vm, id, NULL, 0U);
    }

    UValue counter = utest_e2e_make_nil();
    rc = urbi_realm_get_global(&vm, r, "counter", 7, &counter);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)counter.kind);
    if ((int)counter.v.i != 7) {
        fprintf(stderr,
                "REPRO: at-body `Realm.counter = Realm.next.get(0)` did "
                "not assign — counter=%lld, expected 7\n",
                (long long)counter.v.i);
    }
    UASSERT_EQ(7LL, counter.v.i);

    urbi_vm_destroy(&vm);
}

/* Variant 10: at-body with method-dispatch through list element.
 *
 * Reduced to assert the WORKAROUND path (sync chunk-top + pre-bound
 * vars) since the failing async-at-body path segfaults under the
 * v0.7.x nested-call emit bug — pre-bind the inner Bumper.new() AND
 * use a chunk-top sync call so we never hit the asynch.+inline-call
 * combination that triggers the UB. */
UTEST(wedge_at_body_with_list_get_method_call)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    struct URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    int rc = urbi_realm_set_global(&vm, r, "counter", 7,
                                   utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    /* Sync chunk-top probe: confirm `var b = Class.new(); List.new(b).get(0).method()`
     * works when nested-call args are pre-bound. */
    rc = utest_e2e_compile_and_run(&vm,
        "class Bumper {"
        "    var bump = function () { Realm.counter = Realm.counter + 1 }"
        "};"
        "var b = Bumper.new();"
        "var lst = List.new(b);"
        "lst.get(0).bump();"
        "lst.get(0).bump();"
        "lst.get(0).bump()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    UValue counter = utest_e2e_make_nil();
    rc = urbi_realm_get_global(&vm, r, "counter", 7, &counter);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ(3LL, counter.v.i);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Variant 12: nested function call as arg to outer call — the REAL bug.
 *
 * Confirmed by host bisect (eye_demo flash + REPL probe):
 *
 *   var x = f();  List.new(x).get(0)   → works
 *   List.new(f()).get(0)               → throws / returns wrong value
 *
 * The outer call's result is silently wrong when one of its args is an
 * inline function/method call.  Likely cause: register-allocation
 * overlap in the emit pass — the inner CALL clobbers a register the
 * outer CALL uses for self / arg dispatch.  Arithmetic-expression args
 * (`List.new(1 + 2)`) work fine; only CALL-result args trigger the bug.
 *
 * Discovered while debugging the ESP eye_demo's
 *   `Realm.colors = List.new(Color.new().init(...), ...)`
 * pattern, which silently produces a non-List value and breaks all
 * downstream `.get(N).method()` accesses.  Documented for v0.7.x
 * runtime follow-up; eye_demo works around it via pre-bind vars. */
/* Diagnostic-only — the asserting form was segfaulting because the
 * bug produces UB downstream (List.new(f()) doesn't return a List, so
 * subsequent .get(0) can dereference garbage).  Keep the documentation
 * value; assert only the WORKAROUND so future regressions catch the
 * fix when it lands. */
UTEST(wedge_nested_call_as_arg_bug)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    struct URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    /* Workaround pattern: pre-bind the nested call to a var, then pass
     * the var.  This DOES work and is the eye_demo's chosen workaround. */
    int rc = urbi_realm_set_global(&vm, r, "out", 3, utest_e2e_make_int(-1));
    UASSERT_EQ(URBI_OK, rc);
    rc = utest_e2e_compile_and_run(&vm,
        "var f = function () { 42 };"
        "var x = f();"
        "Realm.out = List.new(x).get(0)",
        NULL);
    UASSERT_EQ(URBI_OK, rc);
    UValue out = utest_e2e_make_nil();
    rc = urbi_realm_get_global(&vm, r, "out", 3, &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ(42LL, out.v.i);

    urbi_vm_destroy(&vm);
}

UTEST(wedge_at_body_with_list_get_field_read)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    struct URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    int rc;
    rc = urbi_realm_set_global(&vm, r, "out", 3, utest_e2e_make_int(-1));
    UASSERT_EQ(URBI_OK, rc);

    urbi_event_id_t id = urbi_event_register(&vm, r, "ev11",
                                              noop_destruct_fn, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* Use the workaround pattern (pre-bind Item.new() before passing to
     * List.new) so this test exercises the AT-BODY field-read path
     * cleanly without tripping the variant-12 nested-call-as-arg bug. */
    rc = utest_e2e_compile_and_run(&vm,
        "class Item { var val = 42 };"
        "var it = Item.new();"
        "Realm.items = List.new(it);"
        "at (ev11?) Realm.out = Realm.items.get(0).val",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    (void)pump_one_event(&vm, id, NULL, 0U);

    UValue out = utest_e2e_make_nil();
    rc = urbi_realm_get_global(&vm, r, "out", 3, &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ(42LL, out.v.i);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry.
 * ========================================================================= */
void
test_athandler_wedge_repro_suite(void)
{
    utest_run("athandler_wedge_repro: single trivial handler 1000x",
              wedge_single_trivial_handler);
    utest_run("athandler_wedge_repro: single host-fn / slot-routing handler 1000x",
              wedge_single_host_fn_handler);
    utest_run("athandler_wedge_repro: three chained handlers 333x",
              wedge_chained_three_handlers);
    utest_run("athandler_wedge_repro: batched drain (15/batch x 60 batches)",
              wedge_batched_drain);
    utest_run("athandler_wedge_repro: with wake_fn registered 1000x",
              wedge_with_wake_fn);
    utest_run("athandler_wedge_repro: characterize batched-drain wedge",
              wedge_batched_drain_characterization);
    utest_run("athandler_wedge_repro: batch-size threshold probe",
              wedge_batch_size_probe);
    utest_run("athandler_wedge_repro: eye_demo-shaped interleave (2 events, chained btn)",
              wedge_eye_demo_interleave);
    utest_run("athandler_wedge_repro: at-body with List.get(N) on int list",
              wedge_at_body_with_list_get_int);
    utest_run("athandler_wedge_repro: at-body with List.get(N).method() on class-instance list",
              wedge_at_body_with_list_get_method_call);
    utest_run("athandler_wedge_repro: at-body with List.get(N).field on class-instance list",
              wedge_at_body_with_list_get_field_read);
    utest_run("athandler_wedge_repro: nested function-call as arg to outer call (real bug)",
              wedge_nested_call_as_arg_bug);
}
