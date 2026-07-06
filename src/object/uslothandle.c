/* SPDX-License-Identifier: BSD-3-Clause */
/* uslothandle.c — USlotHandle creation + validate-or-refresh access.
 *
 * Per USlot/UProps collapse spec §7.  See uslothandle.h for the
 * struct layout + public API contract. */

#include <stdint.h>

#include "object/uslothandle.h"
#include "object/uobject.h"
#include "object/ushape.h"
#include "vm/uvm.h"
#include "urbi/gc.h"               /* urbi_gc_alloc */
#include "gc/ugc_incremental.h"    /* urbi_gc_slot_store */
#include "changed/uchanged_node.h"         /* urbi_emit_slot_change_if_subscribed */
#include "gc/ugc.h"
#include "chunk/uchunk.h"
#include <stddef.h>

/* === urbi_object_get_slot ===
 *
 * Resolve (obj, name) to a holder + slot index via urbi_object_resolve_slot
 * (the same DFS the IC slow path uses).  On hit, allocate a fresh
 * USlotHandle that captures owner + shape + index + topology-gen + name.
 *
 * Returns NULL on miss or OOM. */
USlotHandle *
urbi_object_get_slot(UVM *vm, UObject *obj, USymbol *name)
{
    if (vm == NULL || obj == NULL || name == NULL) {
        return NULL;
    }

    UObject *holder = NULL;
    uint32_t idx    = 0U;
    int rc = urbi_object_resolve_slot(vm, obj, name, &holder, &idx);
    if (rc <= 0) {
        return NULL;   /* miss or resolve-stack overflow */
    }

    UCell *c = urbi_gc_alloc(vm, sizeof(USlotHandle), UTYPE_SLOTHANDLE);
    if (c == NULL) {
        return NULL;
    }
    USlotHandle *h = (USlotHandle *)c;
    h->owner               = holder;
    h->shape_at_create     = holder->shape;
    h->slot_index          = idx;
    h->creation_topgen_low = (uint32_t)vm->topology_gen;
    h->name                = name;
    return h;
}

/* === validate_or_refresh — USlot/UProps spec §7.4 ===
 *
 * Fast path: shape unchanged, cached slot_index is still correct.
 * Slow path: shape transitioned since creation (slot may have moved or
 * been removed entirely).  Re-resolve by name on h->owner's current shape:
 *   - hit:  refresh slot_index + shape_at_create + creation_topgen_low; OK
 *   - miss: slot was removed; the handle is permanently invalid (read/write
 *           returns -1).  We don't NULL out fields — h->owner stays valid
 *           for inspection but no successful value transfer is possible. */
static int
validate_or_refresh(UVM *vm, USlotHandle *h)
{
    if (h->owner->shape == h->shape_at_create) {
        return 1;   /* fast path: cached state is current */
    }
    int32_t idx = urbi_shape_find_slot(h->owner->shape, h->name);
    if (idx < 0) {
        return 0;   /* slot was removed; permanently invalid */
    }
    h->slot_index          = (uint32_t)idx;
    h->shape_at_create     = h->owner->shape;
    h->creation_topgen_low = (uint32_t)vm->topology_gen;
    return 1;
}

int
urbi_slothandle_read_value(UVM *vm, USlotHandle *h, UValue *out)
{
    if (vm == NULL || h == NULL || out == NULL) {
        return -1;
    }
    if (!validate_or_refresh(vm, h)) {
        return -1;
    }
    *out = h->owner->slots[h->slot_index];
    return 0;
}

int
urbi_slothandle_write_value(UVM *vm, USlotHandle *h, UValue v)
{
    if (vm == NULL || h == NULL) {
        return -1;
    }
    if (!validate_or_refresh(vm, h)) {
        return -1;
    }
    /* Forward Dijkstra barrier on the slot store: if the owner cell is
     * black and v is a white heap value, shade v gray.  Mirrors the
     * pattern used by urbi_object_set_local_slot's in-place-update branch
     * (which inherits the barrier from the GC's per-slot mark callback —
     * USlotHandle writes are direct so we wire the barrier explicitly).
     * Use the combined helper to keep barrier + store atomic (F12). */
    urbi_gc_slot_store(vm, (UCell *)h->owner, h->slot_index,
                       &h->owner->slots[h->slot_index], v);
    urbi_emit_slot_change_if_subscribed(vm, h->owner, h->name, v);
    return 0;
}
