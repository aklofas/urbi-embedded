/* SPDX-License-Identifier: BSD-3-Clause */
/* UChangedNode: per-slot slot-change subscriber cell.
 * Spec #4 §3.1.
 *
 * One UChangedNode is allocated (lazily, on first `obj.x.changed?` install)
 * per (object, slot-name) pair.  It lives on UObject.changed_events_head as
 * a singly-linked intrusive list.
 *
 * Layout on a 64-bit host (8-byte pointer alignment):
 *   cell   : type_tag(1) + gc_byte(1) + pad0(6) = 8 B
 *   name   : 8 B  (USymbol*)
 *   event  : 8 B  (UEvent*)
 *   next   : 8 B  (UChangedNode*)
 *   Total  : 32 B
 *
 * On 32-bit cross targets (Cortex-M7, rv32imc) pointer fields shrink to 4 B
 * and the total is 16 B.  The URBI_STATIC_ASSERT in this header gates the exact
 * 32-byte check to __SIZEOF_POINTER__ == 8 builds.
 *
 * type_tag = UTYPE_CHANGED_NODE; gc_byte = 0 at alloc (set by urbi_gc_alloc).
 * name points to an interned USymbol (intern table keeps it alive — not walked
 * by the GC walker).  event and next are GC-managed and walked. */

#ifndef UCHANGED_NODE_H
#define UCHANGED_NODE_H

#include <stdint.h>

#include "gc/ugc.h"              /* UCell, UTYPE_CHANGED_NODE */
#include "gc/ugc_incremental.h"  /* UGC_HAS_SLOT_CHANGE_EVENT, LIKELY, UNLIKELY */
#include "event/uevent.h"              /* UEvent forward-compatible include */
#include "chunk/umodule.h"             /* UValue, USymbol */

#ifdef __cplusplus
extern "C" {
#endif

/* === Forward declarations === */

struct UVM;
struct USymbol;

/* === UChangedNode struct (spec #4 §3.1) === */

typedef struct UChangedNode {
    /* --- common cell header (matches UCell at offsets 0..1) --- */
    uint8_t  type_tag;                   /* UTYPE_CHANGED_NODE */
    uint8_t  gc_byte;                    /* GC color bits; set by urbi_gc_alloc */
    uint8_t  pad0[6];                    /* explicit pad to 8 B before first ptr */

    /* --- slot name (interned; not GC-walked per intern-table contract) --- */
    struct USymbol      *name;

    /* --- per-slot changed event (GC-walked) --- */
    UEvent              *event;

    /* --- intrusive list link; NULL = end of chain --- */
    struct UChangedNode *next;
} UChangedNode;

/* Size assertion: 32 B on 64-bit host (8B header + 8B + 8B + 8B = 32B).
 * On 32-bit cross targets the total is 16 B (4B header + 4B + 4B + 4B).
 * Guarded on pointer width to avoid a hard failure on 32-bit cross targets. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
URBI_STATIC_ASSERT(sizeof(UChangedNode) == 32,
               "UChangedNode must be 32 bytes on 64-bit (spec #4 §3.1)");
#endif

/* === urbi_object_get_or_create_change_event (spec #4 §6.3) ===
 *
 * Walk obj->changed_events_head by USymbol pointer identity.
 * On miss: GC-alloc a UChangedNode + UEvent, prepend, set UGC_HAS_SLOT_CHANGE_EVENT,
 * and manually gc_shade_gray when the parent is BLACK.
 * OOM on either alloc returns NULL (fail-soft). Idempotent. */
struct UObject;
struct UVM;
struct UEvent;

struct UEvent *urbi_object_get_or_create_change_event(struct UVM    *vm,
                                                       struct UObject *obj,
                                                       struct USymbol *name);

/* === urbi_defer_slot_change (spec #4 §5.3) ===
 *
 * Write (parent, key, new_value) to the tail of the per-VM deferred SPSC
 * ring.  Called from urbi_emit_slot_change_slow when re-entrancy is
 * detected.  Ring-full silently drops with a one-shot URBI_LOG_WARN. */
void urbi_defer_slot_change(struct UVM    *vm,
                            struct UObject *parent,
                            struct USymbol *key,
                            UValue          new_value);

/* === urbi_emit_slot_change_slow (spec #4 §5.1) ===
 *
 * Slow path: called when UGC_HAS_SLOT_CHANGE_EVENT is set on parent.
 * Walks changed_events_head by USymbol identity, dispatches via
 * c_event_emit_sync.  Re-entrancy from scratch context routes to the
 * deferred-emit ring (T66).
 *
 * EMITR-013 contract: silent return on unmatched key is the normal case.
 * UGC_HAS_SLOT_CHANGE_EVENT is a per-OBJECT bit ("at least one slot on
 * this object has a change-watcher"), not per-slot.  Every slot-change
 * emit on a subscribed object reaches this function, but only one slot's
 * UChangedNode entry needs to match the supplied `key`.  When the chain
 * walk falls through with no name == key match, the affected slot simply
 * has no subscribers — silently return (no observer to notify).
 *
 * The earlier "programming error / bit 7 must only be set when at least
 * one UChangedNode exists" framing was misleading: bit 7 is correct as
 * long as ANY slot has a UChangedNode, which is what the chain walk
 * verifies on a per-key basis.  Callers MUST NOT treat the silent return
 * as an error path; it is the expected outcome for the "different slot"
 * case.  Callers that have already validated the key against an IC
 * table (and therefore know the chain SHOULD have an entry) can add a
 * URBI_INTERNAL_ASSERT in the caller to catch genuine misuse. */
void urbi_emit_slot_change_slow(struct UVM    *vm,
                                struct UObject *parent,
                                struct USymbol *key,
                                UValue          new_value);

/* === urbi_emit_slot_change_if_subscribed (spec #4 §5.1) — inline fast path ===
 *
 * Tests bit 7 of parent->cell.gc_byte with a single branch.  On the common
 * case (no subscriber, bit clear) this expands to ~2 instructions and does
 * not call into the slow path.  On the rare case (subscriber installed) the
 * slow path walks the UChangedNode chain by USymbol pointer identity and
 * dispatches via c_event_emit_sync.
 *
 * Call site pattern (all slot-write callsites):
 *   store(obj, idx, v);
 *   urbi_emit_slot_change_if_subscribed(vm, obj, key_sym, v);
 *
 * TAGCH-010 — sticky bit:
 *   The bit-7 read here is monotonically rising with respect to the lifetime
 *   of `parent` — UGC_HAS_SLOT_CHANGE_EVENT is set on first chain prepend in
 *   urbi_object_get_or_create_change_event and never cleared.  Therefore a
 *   "true" outcome on this branch means "obj has had at least one
 *   subscriber at some point", not "obj currently has a live subscriber".
 *   The slow path tolerates the latter case via silent return on chain
 *   miss.  Clearing-on-detach is a v1.x design item — audit row GC-002.
 */
static inline void
urbi_emit_slot_change_if_subscribed(struct UVM    *vm,
                                    struct UObject *parent,
                                    struct USymbol *key,
                                    UValue          new_value)
{
    if (LIKELY(!(((UCell *)parent)->gc_byte & UGC_HAS_SLOT_CHANGE_EVENT))) return;
    urbi_emit_slot_change_slow(vm, parent, key, new_value);
}

/* === urbi_drain_deferred_slot_changes (spec #4 §5.3) ===
 *
 * Drain the per-VM deferred slot-change ring (filled by re-entrant writes
 * during a sync slot-change body).  Called at every safepoint BEFORE
 * watcher_eval_dirty, per spec §5.4 ordering.
 * No-op when the ring is empty (head == tail) or NULL (OOM at init). */
void urbi_drain_deferred_slot_changes(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* UCHANGED_NODE_H */
