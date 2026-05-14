/* SPDX-License-Identifier: BSD-3-Clause */
/* ISR-safe SPSC event ring implementation.
   Freestanding-safe: no <string.h>, <stdlib.h>, or any hosted header.
   Acquire/release ordering via GCC/Clang __atomic_* builtins. */

#include "event/uevent_ring.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"  /* URBI_ERR_EVENT_* error codes */
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

        /* T57: if a drain handler is registered, call it with the entry's
         * event_id and a NIL payload (the raw-bytes ring does not carry
         * UValues; host handler implements event_id → UEvent* mapping).
         * Without a drain handler, entries are silently discarded.
         *
         * T26 / EVENT-017: removed the M3-compat fallback branch that
         * bumped event_queue_count when no handler was registered.  Any
         * production embedding that uses urbi_inject_event registers a
         * drain handler at boot; the fallback was dead in practice and
         * conflated the ISR-ring drain count with the M5+ UEvent queue
         * count (separate liveness counter).
         *
         * EVENT-007: load with __ATOMIC_ACQUIRE for consistency with the
         * rest of this ISR-aware ring file.  The drain runs on the
         * consumer thread and the handler is registered from the same
         * thread under the v1.0 cooperative model, so no race exists
         * today.  The atomic load is defensive: it pairs with the
         * __ATOMIC_RELEASE store in urbi_register_event_drain so
         * URBI_SCHED_PREEMPTIVE multi-threading work (deferred to v1.x
         * per design-risks) inherits a correct contract instead of
         * relying on freshly-discovered races. */
        urbi_event_drain_handler h = (urbi_event_drain_handler)
            __atomic_load_n(&vm->event_drain_handler, __ATOMIC_ACQUIRE);
        if (h) {
            UValue nil_payload;
            nil_payload.kind = (uint8_t)UVAL_NIL;
            nil_payload.v.i  = 0;
            h(vm, e->event_id, nil_payload);
        }

        rd = (rd + 1U) & (uint32_t)(URBI_EVENT_RING_DEPTH - 1U);
        drained++;
    }

    /* Publish consumed position: RELEASE so producer sees free slots. */
    __atomic_store_n(&r->read_idx, rd, __ATOMIC_RELEASE);
}
