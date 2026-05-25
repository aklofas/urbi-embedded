/* SPDX-License-Identifier: BSD-3-Clause */
/* ISR-safe SPSC event ring implementation.
   Freestanding-safe: no <string.h>, <stdlib.h>, or any hosted header.
   Acquire/release ordering via GCC/Clang __atomic_* builtins. */

#include "event/uevent_ring.h"
#include "event/uevent_registry.h"  /* UEventRegistry, uevent_registry_lookup_by_id */
#include "event/uevent_emit.h"      /* c_event_emit_async */
#include "watcher/uwatcher_host.h"  /* uhost_watcher_table_walk_event (Gap J) */
#include "vm/uvm.h"
#include "urbi/urbi.h"  /* URBI_ERR_EVENT_* error codes, urbi_make_nil */
#include <stdint.h>

/* Byte-copy helper: replaces memcpy.  No hosted headers in src/. */
static void
ring_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t i;
    for (i = 0U; i < n; i++) {
        d[i] = s[i];
    }
}

void
uevent_ring_init(UEventRing *r)
{
    __atomic_store_n(&r->write_idx,      0U, __ATOMIC_RELAXED);
    __atomic_store_n(&r->read_idx,       0U, __ATOMIC_RELAXED);
    __atomic_store_n(&r->overflow_count, 0U, __ATOMIC_RELAXED);
}

bool
uevent_ring_has_pending(const UEventRing *r)
{
    if (!r) return false;
    uint32_t w  = __atomic_load_n(&r->write_idx, __ATOMIC_ACQUIRE);
    uint32_t rd = __atomic_load_n(&r->read_idx,  __ATOMIC_RELAXED);
    return w != rd;
}

int
urbi_inject_event(struct UVM *vm, uint32_t event_id,
                  const void *payload, size_t len)
{
    UEventRing *r;
    uint32_t w, rd, next_w;
    UEventRingEntry *e;

    if (!vm) return URBI_ERR_INVALID_ARG;

    r = vm->event_ring;
    if (!r) return URBI_ERR_INVALID_ARG;

    if (len > (size_t)URBI_EVENT_PAYLOAD_MAX) {
        __atomic_fetch_add(&r->overflow_count, 1U, __ATOMIC_RELAXED);
        return URBI_ERR_EVENT_PAYLOAD_TOO_LARGE;
    }

    /* SPSC producer path: only one ISR writer at a time.
       write_idx is private to the producer — load with RELAXED.
       read_idx is written by the consumer — load with ACQUIRE. */
    w      = __atomic_load_n(&r->write_idx, __ATOMIC_RELAXED);
    rd     = __atomic_load_n(&r->read_idx,  __ATOMIC_ACQUIRE);
    next_w = (w + 1U) & (uint32_t)(URBI_EVENT_RING_DEPTH - 1U);

    if (next_w == rd) {
        /* Ring full. */
        __atomic_fetch_add(&r->overflow_count, 1U, __ATOMIC_RELAXED);
        return URBI_ERR_EVENT_RING_FULL;
    }

    e = &r->ring[w];
    e->event_id    = event_id;
    e->payload_len = (uint16_t)len;
    if (payload && len > 0U) {
        ring_memcpy(e->payload, payload, len);
    }

    /* Publish the new entry: RELEASE so the consumer sees the entry data. */
    __atomic_store_n(&r->write_idx, next_w, __ATOMIC_RELEASE);

    /* Gap S (v0.7.1): notify the embedder that the ring has a new entry.
     * wake_fn may be called from ISR context; it MUST be O(1), non-blocking,
     * and non-allocating (e.g., xTaskNotifyGiveFromISR on FreeRTOS).
     * Load wake_fn with ACQUIRE to pair with the RELEASE store in
     * urbi_set_wake_fn — defensive for future URBI_SCHED_PREEMPTIVE work. */
    {
        void (*wfn)(void *) = (void (*)(void *))
            __atomic_load_n(&vm->wake_fn, __ATOMIC_ACQUIRE);
        if (wfn) {
            void *wud = vm->wake_ud;
            wfn(wud);
        }
    }

    return URBI_OK;
}

void
uevent_ring_drain(struct UVM *vm)
{
    UEventRing *r;
    uint32_t w, rd, drained;

    if (!vm) return;
    /* Gap R: atomic section guard.  When atomic_active is set, the
     * embedder has bracketed a group of ISR events (urbi_atomic_begin /
     * urbi_atomic_end); hold all ring entries until the section closes. */
    if (vm->atomic_active) return;
    r = vm->event_ring;
    if (!r) return;

    /* Consumer path: read_idx is private to the consumer.
       write_idx is written by the producer — load with ACQUIRE. */
    w   = __atomic_load_n(&r->write_idx, __ATOMIC_ACQUIRE);
    rd  = __atomic_load_n(&r->read_idx,  __ATOMIC_RELAXED);

    drained = 0U;
    /* EVENT-008: SPSC ring max-usable depth is DEPTH-1 (one slot reserved
     * to distinguish full from empty).  Bound the drain at DEPTH-1, not
     * DEPTH, so the iteration count cannot exceed the actual capacity. */
    while (rd != w && drained < (uint32_t)(URBI_EVENT_RING_DEPTH - 1U)) {
        UEventRingEntry *e = &r->ring[rd];

        /* Gap B drain auto-routing (T61): check the event registry first.
         * If event_id is registered and not tombstoned, call the destructure
         * fn (if any) to convert raw payload bytes into UValues, then
         * dispatch through the UEvent via c_event_emit_async.
         *
         * Destructure fn contract:
         *   argc = destruct_fn(vm, payload_bytes, payload_len, args, 16, ud)
         *   argc >= 0 — success; args[0..argc-1] filled.
         *   argc < 0  — error; log + drop this event (do not emit).
         *
         * Multi-arg threading: today we emit a single UValue payload —
         * args[0] when argc > 0, NIL when argc == 0.  Full args[0..argc-1]
         * threading into watcher body R[0..n] requires the host-watcher
         * infrastructure landed in Sub-Bundle 3 (T64+).  Until then,
         * single-arg events work end-to-end; multi-arg events deliver only
         * the first argument to script-side `at(name ?(x))` bodies.
         *
         * Reentrancy: a watcher body that calls urbi_inject_event enqueues
         * into the ring but does NOT drain in this pass — it appears in the
         * next drain call.  This matches the pre-existing behavior for the
         * legacy drain handler (no change to reentrancy contract).
         *
         * Registered path does NOT fall through to the legacy handler.
         * Unregistered or tombstoned ids fall through unchanged (backward
         * compat with Wave-1 urbi_register_event_drain users).
         *
         * EVENT-007: legacy drain handler loaded with __ATOMIC_ACQUIRE for
         * the same reason documented in the original comment — defensive
         * pairing with the __ATOMIC_RELEASE store in urbi_register_event_drain
         * for future URBI_SCHED_PREEMPTIVE work. */
        if (e->event_id <= (uint32_t)0xFFFEU) {
            /* event_id is in the valid urbi_event_id_t range — check registry. */
            UEventRegistryEntry *re = uevent_registry_lookup_by_id(
                    &vm->event_registry, (urbi_event_id_t)e->event_id);
            if (re != NULL) {
                /* Registered path: destructure + emit. */
                UValue   args[16];
                int      argc = 0;
                UValue   payload;

                if (re->destruct_fn != NULL) {
                    /* Stack-allocated args buffer: max 16 UValues.
                     * Embedders with more than 16 args need a separate
                     * design (v1.x growable buffer backlog). */
                    argc = re->destruct_fn(vm,
                            (const urbi_event_payload_t *)e->payload,
                            (size_t)e->payload_len,
                            args, 16, re->destruct_ud);
                    if (argc < 0) {
                        /* Destructure error: log (if host_log_fn set) and drop. */
                        if (vm->host_log_fn) {
                            vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                                "event drain: destruct_fn error; dropping event");
                        }
                        rd = (rd + 1U) & (uint32_t)(URBI_EVENT_RING_DEPTH - 1U);
                        drained++;
                        continue;
                    }
                }

                /* Emit script-side watcher (UEvent dispatch, single-payload).
                 * Dispatch ordering: script-side UEvent first (so `at(name?)`
                 * watcher bodies are queued), then host-side callbacks below. */
                if (argc > 0) {
                    payload = args[0];
                } else {
                    payload.kind = (uint8_t)UVAL_NIL;
                    payload.v.i  = 0;
                }
                c_event_emit_async(vm, re->event, payload);

                /* Gap J (v0.7.1): dispatch host-side watchers.
                 * Pass ALL destructured args (argc, args[0..argc-1]) so host
                 * watchers receive the full multi-arg payload.  This closes the
                 * Sub-Bundle 2 multi-arg deferral for the host-watcher path.
                 * Note: script-side `at(name?)` bodies still see only args[0]
                 * via c_event_emit_async above — full multi-arg script threading
                 * is a separate v1.x item. */
                uhost_watcher_table_walk_event(&vm->host_watcher_table, vm,
                        (urbi_event_id_t)e->event_id,
                        (argc > 0) ? args : NULL, argc);

                rd = (rd + 1U) & (uint32_t)(URBI_EVENT_RING_DEPTH - 1U);
                drained++;
                continue;   /* registered path: do NOT fire legacy handler */
            }
        }

        /* Unregistered or out-of-range event_id: fall through to the
         * Wave-1 legacy drain handler (backward compat). */
        urbi_event_drain_handler h = (urbi_event_drain_handler)
            __atomic_load_n(&vm->event_drain_handler, __ATOMIC_ACQUIRE);
        if (h) {
            UValue nil_payload;
            nil_payload.kind = (uint8_t)UVAL_NIL;
            nil_payload.v.i  = 0;
            h(vm, vm->event_drain_ud, e->event_id, nil_payload);
        }

        rd = (rd + 1U) & (uint32_t)(URBI_EVENT_RING_DEPTH - 1U);
        drained++;
    }

    /* Publish consumed position: RELEASE so producer sees free slots. */
    __atomic_store_n(&r->read_idx, rd, __ATOMIC_RELEASE);
}
