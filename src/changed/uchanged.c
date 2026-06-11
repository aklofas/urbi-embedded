/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi_object_get_or_create_change_event — lazy per-(object,slot) UEvent.
 * Spec #4 §6.3. */

#include "changed/uchanged_node.h"        /* UChangedNode, UTYPE_CHANGED_NODE */
#include "event/uevent.h"               /* UEvent, urbi_event_create */
#include "object/uobject.h"       /* UObject */
#include "gc/ugc.h"               /* urbi_gc_alloc */
#include "gc/ugc_incremental.h"   /* UGC_HAS_SLOT_CHANGE_EVENT, UGC_COLOR_BLACK,
                                   * UGC_COLOR_MASK, gc_shade_gray, UNLIKELY */
#include "vm/uvm.h"                  /* UVM, host_log_fn */
#include "urbi/urbi.h"            /* URBI_ASSERT_NOT_ISR, URBI_LOG_WARN */
#include "chunk/uchunk.h"
#include <stddef.h>

/* USymbol is just an interned const char* — the typedef lives in umodule.h
 * which is pulled in transitively via uobject.h. */

/* urbi_object_get_or_create_change_event: walk obj->changed_events_head by
 * USymbol pointer identity (interned → pointer-compare is sufficient).
 * On hit: return existing UEvent.
 * On miss: GC-alloc a UChangedNode + a new UEvent (spec #3 constructor),
 * prepend node to the chain, set UGC_HAS_SLOT_CHANGE_EVENT on the object,
 * manually gc_shade_gray the node when the parent object is BLACK (forward
 * Dijkstra via a field write — not a UCell-slot write).
 *
 * TAGCH-009 — only the node is explicitly shaded:
 *   The Dijkstra barrier shades the new UChangedNode if the parent is BLACK.
 *   The new UEvent does NOT need a parallel shade because it is reachable
 *   only through node->event — i.e. through the very node we just shaded.
 *   When the GC walks the gray node payload, it follows node->event and
 *   shades the event then.  Shading the event here would be redundant
 *   (and would mask future bugs that broke the node->event link).
 *
 * OOM on either alloc: return NULL (fail-soft); orphan node (if any) is
 * reclaimed by the next sweep.
 * Idempotent: second call for the same (obj, name) returns the same UEvent. */
UEvent *
urbi_object_get_or_create_change_event(UVM *vm, UObject *obj, USymbol *name)
{
    URBI_ASSERT_NOT_ISR(vm);

    /* Walk existing chain (pointer-identity compare — USymbol is interned). */
    UChangedNode *n;
    for (n = obj->changed_events_head; n != NULL; n = n->next) {
        if (n->name == name) return n->event;
    }

    /* Miss — allocate a new UChangedNode cell. */
    UCell *nc = urbi_gc_alloc(vm, sizeof(UChangedNode), UTYPE_CHANGED_NODE);
    if (UNLIKELY(nc == NULL)) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN, "slot-change OOM (node alloc)");
        return NULL;
    }
    UChangedNode *node = (UChangedNode *)nc;

    /* Allocate the associated UEvent.
     *
     * GC soundness (v0.13.2): pin the node across the event allocation —
     * the node is referenced only by this C local until the wire step
     * below, and a collection triggered by urbi_event_create
     * (URBI_GC_STRESS collects on every alloc) swept it, after which the
     * wire step corrupted recycled memory and published a dangling
     * changed_events_head entry.  The node is zero-filled (no children),
     * so the no-trace caveat of pins is moot here. */
    nc->gc_byte |= UGC_IS_PINNED;
    UEvent *event = urbi_event_create(vm);
    nc->gc_byte = (uint8_t)(nc->gc_byte & ~(uint8_t)UGC_IS_PINNED);
    if (UNLIKELY(event == NULL)) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN, "slot-change OOM (event alloc)");
        /* node is not yet linked; GC will reclaim it on the next sweep. */
        return NULL;
    }

    /* Wire the node. */
    node->name  = name;
    node->event = event;
    node->next  = obj->changed_events_head;
    obj->changed_events_head = node;

    /* Mark the object: at least one slot now has a change-event subscriber.
     *
     * TAGCH-010 — sticky-bit semantics:
     *   UGC_HAS_SLOT_CHANGE_EVENT is monotonically set; the v0.5.x runtime
     *   has NO clearing path.  Once any slot on `obj` ever had a watcher,
     *   the bit stays set for the lifetime of the object — even if every
     *   subscriber is later removed.  Effect: the slow path
     *   urbi_emit_slot_change_slow stays reachable on every slot write of
     *   that object; it tolerates an empty / unmatched chain by silent
     *   return (see uchanged_emit.c "no chain entry matches" branch).
     *   The cost is one chain walk per slot write on detached objects.
     *   Clearing on full chain detach is filed for v1.x — audit row GC-002. */
    ((UCell *)obj)->gc_byte |= UGC_HAS_SLOT_CHANGE_EVENT;

    /* Dijkstra forward barrier: if the parent object is BLACK, the newly
     * prepended node (WHITE/GRAY born-white) must be shaded gray so the
     * tri-color invariant is preserved. */
    if (UNLIKELY((((UCell *)obj)->gc_byte & UGC_COLOR_MASK) == UGC_COLOR_BLACK))
        gc_shade_gray(vm, (UCell *)node);

    return event;
}
