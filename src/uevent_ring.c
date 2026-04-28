/* SPDX-License-Identifier: BSD-3-Clause */
/* ISR-safe SPSC event ring implementation.
   Freestanding-safe: no <string.h>, <stdlib.h>, or any hosted header.
   Acquire/release ordering via GCC/Clang __atomic_* builtins. */

#include "uevent_ring.h"
#include "uvm.h"
#include "urbi.h"  /* URBI_ERR_EVENT_* error codes */

/* Byte-copy helper: replaces memcpy.  No hosted headers in src/. */
static void
ring_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    size_t i;
    for (i = 0u; i < n; i++) {
        d[i] = s[i];
    }
}

void
uevent_ring_init(UEventRing *r)
{
    __atomic_store_n(&r->write_idx,      0u, __ATOMIC_RELAXED);
    __atomic_store_n(&r->read_idx,       0u, __ATOMIC_RELAXED);
    __atomic_store_n(&r->overflow_count, 0u, __ATOMIC_RELAXED);
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
        __atomic_fetch_add(&r->overflow_count, 1u, __ATOMIC_RELAXED);
        return URBI_ERR_EVENT_PAYLOAD_TOO_LARGE;
    }

    /* SPSC producer path: only one ISR writer at a time.
       write_idx is private to the producer — load with RELAXED.
       read_idx is written by the consumer — load with ACQUIRE. */
    w      = __atomic_load_n(&r->write_idx, __ATOMIC_RELAXED);
    rd     = __atomic_load_n(&r->read_idx,  __ATOMIC_ACQUIRE);
    next_w = (w + 1u) & (uint32_t)(URBI_EVENT_RING_DEPTH - 1u);

    if (next_w == rd) {
        /* Ring full. */
        __atomic_fetch_add(&r->overflow_count, 1u, __ATOMIC_RELAXED);
        return URBI_ERR_EVENT_RING_FULL;
    }

    e = &r->ring[w];
    e->event_id    = event_id;
    e->payload_len = (uint16_t)len;
    if (payload && len > 0u) {
        ring_memcpy(e->payload, payload, len);
    }

    /* Publish the new entry: RELEASE so the consumer sees the entry data. */
    __atomic_store_n(&r->write_idx, next_w, __ATOMIC_RELEASE);

    return URBI_OK;
}

void
uevent_ring_drain(struct UVM *vm)
{
    UEventRing *r;
    uint32_t w, rd, drained;

    if (!vm) return;
    r = vm->event_ring;
    if (!r) return;

    /* Consumer path: read_idx is private to the consumer.
       write_idx is written by the producer — load with ACQUIRE. */
    w   = __atomic_load_n(&r->write_idx, __ATOMIC_ACQUIRE);
    rd  = __atomic_load_n(&r->read_idx,  __ATOMIC_RELAXED);

    drained = 0u;
    while (rd != w && drained < (uint32_t)URBI_EVENT_RING_DEPTH) {
        /* Entry at rd is ready to consume.
           At M3 the watcher machinery (M5) is not yet live.
           We account for the event by incrementing event_queue_count so
           that urbi_step() stays in RUNNING (not QUIESCENT) until T34
           drains the logical queue.  When M5 watcher eval lands, this
           path is replaced by a real watcher-signal dispatch. */
        vm->event_queue_count++;

        rd = (rd + 1u) & (uint32_t)(URBI_EVENT_RING_DEPTH - 1u);
        drained++;
    }

    /* Publish consumed position: RELEASE so producer sees free slots. */
    __atomic_store_n(&r->read_idx, rd, __ATOMIC_RELEASE);
}
