/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi_object_get_or_create_change_event — lazy per-(object,slot) UEvent.
 * Spec #4 §6.3. */

#include "uchanged_node.h"        /* UChangedNode, UTYPE_CHANGED_NODE */
#include "event/uevent.h"               /* UEvent, urbi_event_create */
#include "object/uobject.h"       /* UObject */
#include "gc/ugc.h"               /* urbi_gc_alloc */
#include "gc/ugc_incremental.h"   /* UGC_HAS_SLOT_CHANGE_EVENT, UGC_COLOR_BLACK,
                                   * UGC_COLOR_MASK, gc_shade_gray, UNLIKELY */
#include "vm/uvm.h"                  /* UVM, host_log_fn */
#include "urbi/urbi.h"            /* URBI_ASSERT_NOT_ISR, URBI_LOG_WARN */

/* USymbol is just an interned const char* — the typedef lives in umodule.h
 * which is pulled in transitively via uobject.h. */

/* urbi_object_get_or_create_change_event: walk obj->changed_events_head by
 * USymbol pointer identity (interned → pointer-compare is sufficient).
 * On hit: return existing UEvent.
 * On miss: GC-alloc a UChangedNode + a new UEvent (spec #3 constructor),
 * prepend node to the chain, set UGC_HAS_SLOT_CHANGE_EVENT on the object,
 * manually gc_shade_gray the node when the parent object is BLACK (forward
 * Dijkstra via a field write — not a UCell-slot write).
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
            vm->host_log_fn(vm, URBI_LOG_WARN, "slot-change OOM (node alloc)");
        return NULL;
    }
    UChangedNode *node = (UChangedNode *)nc;

    /* Allocate the associated UEvent. */
    UEvent *event = urbi_event_create(vm);
    if (UNLIKELY(event == NULL)) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, URBI_LOG_WARN, "slot-change OOM (event alloc)");
        /* node is not yet linked; GC will reclaim it on the next sweep. */
        return NULL;
    }

    /* Wire the node. */
    node->name  = name;
    node->event = event;
    node->next  = obj->changed_events_head;
    obj->changed_events_head = node;

    /* Mark the object: at least one slot now has a change-event subscriber. */
    ((UCell *)obj)->gc_byte |= UGC_HAS_SLOT_CHANGE_EVENT;

    /* Dijkstra forward barrier: if the parent object is BLACK, the newly
     * prepended node (WHITE/GRAY born-white) must be shaded gray so the
     * tri-color invariant is preserved. */
    if (UNLIKELY((((UCell *)obj)->gc_byte & UGC_COLOR_MASK) == UGC_COLOR_BLACK))
        gc_shade_gray(vm, (UCell *)node);

    return event;
}
