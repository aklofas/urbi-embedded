/* SPDX-License-Identifier: BSD-3-Clause */
/* UEvent: first-class event cell for reactive dispatch.
 * Spec #3 §3.1.
 *
 * Allocated via urbi_gc_alloc (UTYPE_EVENT); integrated with the incremental
 * GC from birth.  Two intrusive subscriber lists:
 *   at_watchers_head — persistent AT_EVENT / AT_EVENT_SYNC watcher chain
 *                      (linked via UWatcher.next_in_event).
 *   waiters_head     — one-shot UStrand chain (linked via
 *                      UStrand.next_event_waiter); strands self-walk via
 *                      realm hierarchy — NOT walked by the UEvent GC walker.
 *
 * Layout on a 64-bit host (8-byte pointer alignment):
 *   cell header   : type_tag(1) + gc_byte(1) + flags(1) + pad0(5) = 8 B
 *   at_watchers_head : 8 B
 *   waiters_head     : 8 B
 *   name             : 16 B  (UValue — UVAL_NIL at alloc; populated at M6)
 *   Total            : 40 B + natural padding = ~48 B
 *
 * type_tag = UTYPE_EVENT; gc_byte = 0 at alloc (set by urbi_gc_alloc).
 * All pointer fields NULL at alloc; name.kind = UVAL_NIL. */

#ifndef UEVENT_H
#define UEVENT_H

#include <stdint.h>

#include "gc/ugc.h"    /* UCell, UTYPE_EVENT */
#include "module/umodule.h"   /* UValue, UVAL_NIL */

#ifdef __cplusplus
extern "C" {
#endif

/* === Forward declarations === */

struct UVM;
struct UWatcher;
struct UStrand;

/* === UEvent flag bits (stored in UEvent.flags) === */

#define UEVENT_FLAG_RESERVED  0x01u   /* placeholder; no semantic at M5 */

/* === UEvent struct (spec #3 §3.1) === */

typedef struct UEvent {
    /* --- common cell header (matches UCell at offsets 0..1) --- */
    uint8_t  type_tag;                   /* UTYPE_EVENT */
    uint8_t  gc_byte;                    /* GC color bits; set by urbi_gc_alloc */
    uint8_t  flags;                      /* UEVENT_FLAG_* bits; 0 at alloc */
    uint8_t  pad0[5];                    /* explicit pad to 8 B before first ptr */

    /* --- persistent at-watchers list (spec #3 §3.2) ---
     * Linked via UWatcher.next_in_event.  Walked by the GC walker. */
    struct UWatcher *at_watchers_head;

    /* --- one-shot waiters list (spec #3 §4.1) ---
     * Linked via UStrand.next_event_waiter.  Strands self-walk via the
     * realm hierarchy; this field is NOT walked by the GC walker. */
    struct UStrand  *waiters_head;

    /* --- name (M6 stdlib) ---
     * UVAL_NIL at alloc; populated when Event is assigned a name binding. */
    UValue   name;
} UEvent;

/* Size assertion: 40 B on 64-bit host (8B header + 8B + 8B + 16B = 40B).
 * The task description mentioned "~48 B" as a rounded estimate; the exact
 * layout works out to 40 B because pad0[5] fills the gap to 8 B alignment
 * for the first pointer with no trailing compiler pad.
 * Guarded on pointer width to avoid a hard failure on 32-bit cross targets. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(UEvent) == 40,
               "UEvent must be 40 bytes on 64-bit");
#endif

/* === UEvent lifecycle API === */

/* urbi_event_create: allocate and zero-initialize a UEvent via urbi_gc_alloc.
 *   Sets type_tag = UTYPE_EVENT; all pointer fields NULL; name = UVAL_NIL.
 *   Returns NULL on OOM.
 *   Not ISR-safe. */
UEvent *urbi_event_create(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* UEVENT_H */
