/* SPDX-License-Identifier: BSD-3-Clause */
/* ISR-safe single-producer / single-consumer event ring.
   Lock-free via GCC/Clang __atomic_* builtins (acquire/release ordering).
   Freestanding-safe: no hosted headers.

   Power-of-2 depth required for bitmask modulo.
   URBI_EVENT_RING_DEPTH default is 256; M4 footprint builds override to 32. */

#ifndef UEVENT_RING_H
#define UEVENT_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef URBI_EVENT_RING_DEPTH
#  define URBI_EVENT_RING_DEPTH  256   /* must be a power of 2 */
#endif

#ifndef URBI_EVENT_PAYLOAD_MAX
#  define URBI_EVENT_PAYLOAD_MAX 16
#endif

/* Compile-time guard: ring depth must be a power of 2 for bitmask indexing. */
typedef char uevent_ring_depth_must_be_power_of_two[
    ((URBI_EVENT_RING_DEPTH & (URBI_EVENT_RING_DEPTH - 1)) == 0) ? 1 : -1
];

typedef struct UEventRingEntry {
    uint32_t event_id;
    uint16_t payload_len;
    uint8_t  payload[URBI_EVENT_PAYLOAD_MAX];
} UEventRingEntry;

/* Indices stored as plain volatile uint32_t; acquire/release ordering is
   provided via __atomic_load_n / __atomic_store_n / __atomic_fetch_add
   builtins.  This keeps the struct freestanding-compatible under -std=c99
   while still giving the full ISR-safe memory model guarantees. */
typedef struct UEventRing {
    UEventRingEntry ring[URBI_EVENT_RING_DEPTH];
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
    volatile uint32_t overflow_count;
} UEventRing;

/* Opaque forward declaration (definition in uvm.h). */
struct UVM;

/* Initialise all ring fields.  Must be called before any inject/drain. */
void uevent_ring_init(UEventRing *r);

/* Returns true if at least one entry is pending (consumer-side check). */
bool uevent_ring_has_pending(const UEventRing *r);

/* Drain all pending entries into vm's scheduler at urbi_step() entry.
   Bounded to URBI_EVENT_RING_DEPTH drains per call so it cannot starve
   the budget loop.  No-op when vm->event_ring is NULL. */
void uevent_ring_drain(struct UVM *vm);

/* === ISR-safe producer ===
   May be called from interrupt context (single producer only).
   Returns URBI_OK on success.
   Returns URBI_ERR_EVENT_PAYLOAD_TOO_LARGE if len > URBI_EVENT_PAYLOAD_MAX.
   Returns URBI_ERR_EVENT_RING_FULL if the ring has no free slots.
   In both error cases overflow_count is incremented. */
int urbi_inject_event(struct UVM *vm, uint32_t event_id,
                      const void *payload, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* UEVENT_RING_H */
