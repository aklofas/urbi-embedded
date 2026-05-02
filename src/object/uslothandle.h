/* SPDX-License-Identifier: BSD-3-Clause */
/* uslothandle.h — USlotHandle wrapper + getSlot/refresh-on-access (M4 / T37).
 *
 * Per pre-M4 USlot/UProps collapse spec §7.  USlotHandle is a heap GC cell
 * (UCELL_TYPE_SLOTHANDLE = 12) that holds a stable reference to a specific
 * slot on a specific UObject.  urbi_object_get_slot resolves a (recv, name)
 * pair to the actual holder (slot may be local or on an inherited prototype)
 * and allocates a fresh handle pointing at the resolved owner+index.
 *
 * Validate-on-access (validate_or_refresh): on every read/write, compare
 * h->shape_at_create to h->owner->shape.  If they match, the cached
 * slot_index is still correct.  If they differ (shape transition since
 * creation), re-resolve by name; on hit refresh the cache, on miss the
 * handle becomes permanently invalid and read/write returns -1.
 *
 * Strong reference: walk_uslothandle (utypes_init.c) shades h->owner so
 * the owner survives as long as the handle does (per spec §7.6).  shape +
 * name are reachable transitively through the owner. */

#ifndef USLOTHANDLE_H
#define USLOTHANDLE_H

#include <stdint.h>

#include "object/uobject.h"   /* UObject, UCell, UValue, USymbol fwd */
#include "object/ushape.h"    /* UShape */

struct UVM;

/* === USlotHandle ===
 *
 * Layout (40 B on 64-bit hosts):
 *   cell                   2 B   UCell (type_tag = UCELL_TYPE_SLOTHANDLE = 12)
 *   pad                    6 B   compiler-inserted natural-alignment pad
 *   owner                  8 B   resolved holder (may be recv or a proto)
 *   shape_at_create        8 B   owner->shape snapshot at creation/refresh
 *   slot_index             4 B   index into owner->slots[]
 *   creation_topgen_low    4 B   low-32 of vm->topology_gen at creation/refresh
 *                                (bookkeeping only; not load-bearing for
 *                                validate_or_refresh — the shape pointer is
 *                                the actual freshness signal)
 *   name                   8 B   USymbol* preserved for shape-transition refresh
 *
 * The 40-B invariant assumes 64-bit pointers; 32-bit cross targets shrink
 * the pointer fields and the literal byte total no longer holds.  Gate
 * the assert on pointer width (mirrors UObject's pattern). */
typedef struct USlotHandle {
    UCell      cell;
    /* 6 B compiler-inserted padding before owner */
    UObject   *owner;
    UShape    *shape_at_create;
    uint32_t   slot_index;
    uint32_t   creation_topgen_low;
    USymbol   *name;
} USlotHandle;
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(USlotHandle) == 40,
               "USlotHandle layout per pre-M4 USlot/UProps spec §7");
#endif

/* === Public API ===
 *
 * urbi_object_get_slot:
 *   Allocate a USlotHandle pointing at the owner of `name` on `obj` or one
 *   of its prototypes (resolved via urbi_object_resolve_slot, T25).  Returns
 *   NULL if the name doesn't resolve or on OOM.
 *
 * urbi_slothandle_read_value / urbi_slothandle_write_value:
 *   Validate-or-refresh on entry.  If the slot was removed since creation
 *   (re-resolve by name fails), returns -1; the handle is permanently
 *   invalid in that case.  Returns 0 on success.  write_value applies the
 *   GC slot-write barrier (urbi_gc_slot_write) before storing. */
USlotHandle *urbi_object_get_slot       (struct UVM *vm, UObject *obj, USymbol *name);
int          urbi_slothandle_read_value (struct UVM *vm, USlotHandle *h, UValue *out);
int          urbi_slothandle_write_value(struct UVM *vm, USlotHandle *h, UValue v);

#endif /* USLOTHANDLE_H */
