/* SPDX-License-Identifier: BSD-3-Clause */
/* uobject_slot.c — slot install / remove / property primitives.
 * Extracted from uobject.c during v0.5.4-decompose (OBJ-045 #4). */

#include <stddef.h>   /* offsetof */
#include <stdint.h>

#include "object/uobject.h"
#include "object/uobject_internal.h"
#include "object/ushape.h"
#include "vm/uvm.h"
#include "urbi/gc.h"            /* urbi_gc_alloc */
#include "gc/ugc_incremental.h" /* gc_shade_gray */
#include "gc/ugc.h"             /* UTYPE_SLOT_ARRAY / UTYPE_PROPS / UTYPE_PROPS_TABLE */
#include "changed/uchanged_node.h" /* urbi_emit_slot_change_if_subscribed */
#include "module/umodule.h"

/* uprops_alloc — allocate a fresh UProps cell with all flags clear and
 * oget/oset = UVAL_VOID.  Returns NULL on OOM. */
static UProps *
uprops_alloc(UVM *vm)
{
    UCell *c = urbi_gc_alloc(vm, sizeof(UProps), UTYPE_PROPS);
    if (c == NULL) {
        return NULL;
    }
    UProps *p = (UProps *)c;
    p->oget.kind     = UVAL_VOID;
    p->oget.v.i      = 0;
    p->oset.kind     = UVAL_VOID;
    p->oset.v.i      = 0;
    p->constant      = 0U;
    p->_spare        = 0U;
    return p;
}

/* === T26: install a local slot on a receiver ===
 *
 * Per pre-M2 §6.1 + pre-M4 topology-generation spec §4.2 row 2.
 *
 * Two cases:
 *   1. Slot already exists on this lineage (urbi_shape_find_slot returns
 *      an index >= 0): in-place value update.  No shape transition, no
 *      USlotArray reallocation, no topology_gen bump.  Note: at v1.0
 *      `obj->slots[idx]` is the dense receiver storage — assigning to it
 *      stores the new value directly; no separate slot-write barrier is
 *      needed because the OLD value (about to be overwritten) cannot point
 *      at a black-marked cell that the GC has already scanned (USlot is
 *      reachable via walk_uobject's per-slot UValue cb, which the GC re-
 *      drives in the next slice — same reasoning as the UProps oget/oset
 *      writes in T17).
 *
 *   2. Slot is new (find_slot returns -1): transition to the child shape
 *      via urbi_shape_transition_add_slot, allocate a fresh USlotArray
 *      sized for new_shape->count, copy the old slots, write the new
 *      value at new_shape->index, shade the old wrapper (forward Dijkstra
 *      — it is about to become unreachable), and publish the new shape +
 *      slots pointer.  No topology_gen bump per topology spec §4.2 row 2
 *      (IC shape-mismatch check covers this).
 *
 * Returns 0 on success, -1 on OOM. */
int
urbi_object_set_local_slot(UVM *vm, UObject *obj, USymbol *name, UValue value)
{
    if (vm == NULL || obj == NULL || name == NULL) {
        return -1;
    }

    /* Case 1: slot already in this lineage — in-place value update. */
    int32_t existing = urbi_shape_find_slot(obj->shape, name);
    if (existing >= 0) {
        obj->slots[existing] = value;
        urbi_emit_slot_change_if_subscribed(vm, obj, name, value);
        return 0;
    }

    /* Case 2: leaf-shape-add.  Materialise (or hit cached) child shape. */
    UShape *new_shape = urbi_shape_transition_add_slot(vm, obj->shape, name);
    if (new_shape == NULL) {
        return -1;
    }

    /* Allocate fresh USlotArray wrapper sized for the new shape's count.
     * new_shape->count is always >= 1 here (we just added a slot to a
     * shape whose count was N; the child has count N+1).  Layout follows
     * the UPropsTable / UProtoInstanceArr precedent — UCell first, then
     * n + _pad, then the entries[] FAM. */
    UCell *c = urbi_gc_alloc(vm,
                             sizeof(USlotArray)
                             + (size_t)new_shape->count * sizeof(USlot),
                             UTYPE_SLOT_ARRAY);
    if (c == NULL) {
        /* Shape transition already published `new_shape` into the parent's
         * transitions cache; that's harmless — it's just a cached child
         * shape that no UObject currently references.  No partial state to
         * undo on the receiver. */
        return -1;
    }
    USlotArray *fresh = (USlotArray *)c;
    fresh->n    = new_shape->count;
    fresh->_pad = 0U;

    /* Copy old slot values into the new wrapper.  obj->shape->count is
     * the old slot count (one less than new_shape->count). */
    for (uint32_t i = 0U; i < obj->shape->count; i++) {
        fresh->entries[i] = obj->slots[i];
    }
    /* Write the new slot value at the freshly added index. */
    fresh->entries[new_shape->index] = value;

    /* Forward Dijkstra barrier on the OLD wrapper cell — about to become
     * unreachable.  Recover the wrapper base from obj->slots via offsetof
     * (same trick walk_ushape uses for props_table -> UPropsTable).
     * obj->slots is NULL for a freshly allocated UObject (root shape, no
     * slots yet); only shade if there's an existing wrapper. */
    if (obj->slots != NULL) {
        UCell *old_wrapper = (UCell *)(void *)
            ((uint8_t *)obj->slots - offsetof(USlotArray, entries));
        gc_shade_gray(vm, old_wrapper);
    }

    /* Publish.  No unconditional topology_gen bump per topology spec §4.2
     * row 2 — the IC's per-site shape-mismatch check catches any cached
     * entry that referenced the old shape.  T27: BUT if obj is itself a
     * prototype of some other UObject, IC entries that walked through obj
     * looking for `name` may have cached a miss-then-fall-through past
     * obj; installing `name` on obj now changes that resolution result and
     * those entries need invalidation.  Bump in the prototype case only —
     * topology spec §4.1 row 4. */
    obj->slots = fresh->entries;
    obj->shape = new_shape;
    if (obj->flags & URBI_OBJ_FLAG_IS_PROTOTYPE) {
        vm->topology_gen++;
    }
    urbi_emit_slot_change_if_subscribed(vm, obj, name, value);
    return 0;
}

/* === T27: urbi_object_remove_slot ===
 *
 * Strategy: rebuild obj->shape via urbi_shape_transition_remove_slot,
 * allocate a fresh USlotArray sized for new_shape->count, copy the
 * surviving slot values across, shade the old wrapper, publish.  Bumps
 * topology_gen unconditionally per topology spec §4.1 row 1.
 *
 * Returns:
 *    0 — success or silent no-op (slot wasn't present)
 *   -1 — OOM (transition or USlotArray allocation failed) */
int
urbi_object_remove_slot(UVM *vm, UObject *obj, USymbol *name)
{
    if (vm == NULL || obj == NULL || name == NULL) {
        return -1;
    }

    /* No-op if the slot doesn't exist on this lineage. */
    int32_t old_idx = urbi_shape_find_slot(obj->shape, name);
    if (old_idx < 0) {
        return 0;   /* silent no-op per plan body */
    }

    /* Materialise the rebuilt shape (one slot dropped). */
    UShape *new_shape = urbi_shape_transition_remove_slot(vm, obj->shape, name);
    if (new_shape == NULL) {
        return -1;
    }

    /* Allocate fresh USlotArray sized for new_shape->count.  When count == 0
     * (last slot removed), publish slots = NULL and skip the wrapper alloc. */
    USlotArray *fresh = NULL;
    if (new_shape->count > 0U) {
        UCell *c = urbi_gc_alloc(vm,
                                 sizeof(USlotArray)
                                 + (size_t)new_shape->count * sizeof(USlot),
                                 UTYPE_SLOT_ARRAY);
        if (c == NULL) {
            return -1;
        }
        fresh = (USlotArray *)c;
        fresh->n    = new_shape->count;
        fresh->_pad = 0U;

        /* Walk the new shape's lineage in reverse (root-ward → leaf-ward) to
         * recover, for each surviving slot, the *old* index it occupied in
         * obj->slots.  Per pre-M2 §7.1 each shape carries its own (name, index)
         * — old_idx for surviving name N is just urbi_shape_find_slot(obj->
         * shape, N).  The new shape's own (name, index) gives us the
         * destination. */
        for (UShape *s = new_shape; s != NULL; s = s->parent) {
            if (s->name == NULL) break;     /* root */
            int32_t src = urbi_shape_find_slot(obj->shape, s->name);
            /* src must be >= 0 — every name in new_shape's lineage was, by
             * construction, in obj->shape's lineage minus the one removal. */
            fresh->entries[s->index] = obj->slots[src];
        }
    }

    /* Shade the OLD wrapper (forward Dijkstra barrier — about to drop). */
    if (obj->slots != NULL) {
        UCell *old_wrapper = (UCell *)(void *)
            ((uint8_t *)obj->slots - offsetof(USlotArray, entries));
        gc_shade_gray(vm, old_wrapper);
    }

    obj->slots = (fresh != NULL) ? fresh->entries : NULL;
    obj->shape = new_shape;
    vm->topology_gen++;
    return 0;
}

/* === T28: install / remove / mutate slot property primitives ===
 *
 * Each primitive resolves the slot index by urbi_shape_find_slot on the
 * receiver's local shape (NOT the prototype walk — properties belong to
 * shapes, and a slot's properties are pinned to the holding object).
 *
 * install + remove route through urbi_shape_transition_property (T17) for
 * the shape transition, then write the per-slot UProps* into
 * new_shape->props_table[idx].  set_property_value mutates the existing
 * UProps in-place. */

int
urbi_object_install_property(UVM *vm, UObject *obj, USymbol *name,
                             uint8_t flag_bit, UValue value)
{
    if (vm == NULL || obj == NULL || name == NULL) {
        return -1;
    }
    int32_t idx = urbi_shape_find_slot(obj->shape, name);
    if (idx < 0) {
        return -1;   /* slot must exist before installing a property on it */
    }

    /* Allocate a fresh UProps if the slot doesn't have one yet, or copy
     * the existing one (immutability — the existing UProps may be shared
     * with another shape; see USlot/UProps spec §5.1). */
    UProps *existing = (obj->shape->props_table != NULL)
                       ? obj->shape->props_table[idx]
                       : NULL;
    UProps *fresh = uprops_alloc(vm);
    if (fresh == NULL) {
        return -1;
    }
    if (existing != NULL) {
        fresh->oget     = existing->oget;
        fresh->oset     = existing->oset;
        fresh->constant = existing->constant;
    }
    /* Apply the install per flag_bit. */
    if (flag_bit == URBI_SLOT_FLAG_OGET) {
        fresh->oget = value;
    } else if (flag_bit == URBI_SLOT_FLAG_OSET) {
        fresh->oset = value;
    } else if (flag_bit == URBI_SLOT_FLAG_CONSTANT) {
        fresh->constant = 1U;
        (void)value;   /* CONSTANT carries no payload */
    } else {
        return -1;   /* unsupported flag bit */
    }

    /* Materialise sibling shape with the new flag bit.  install=1 even when
     * the bit is already set; transition_property is idempotent and will
     * return parent unchanged in that case. */
    UShape *new_shape = urbi_shape_transition_property(vm, obj->shape,
                                                       (uint32_t)idx,
                                                       flag_bit, 1);
    if (new_shape == NULL) {
        return -1;
    }
    /* If the sibling is the same shape (idempotent no-op), it has no
     * props_table of its own — the existing one (which may be NULL) stays
     * in place.  We still need to publish the freshly-allocated UProps,
     * because we just mutated its oget/oset/constant fields.  Allocate a
     * UPropsTable wrapper if the parent had none; otherwise mutate in place
     * (in-place mutation is the §5 cache-invalidation point — bumping
     * topology_gen below covers any IC entries). */
    if (new_shape == obj->shape) {
        /* Idempotent flag transition.  Need a props_table if there isn't
         * one; otherwise overwrite the per-slot UProps* pointer. */
        if (obj->shape->props_table == NULL) {
            /* Allocate a wrapper cell with all NULL entries, then write
             * fresh into the slot's index. */
            UCell *c = urbi_gc_alloc(vm,
                                     sizeof(UPropsTable)
                                     + (size_t)obj->shape->count
                                       * sizeof(UProps *),
                                     UTYPE_PROPS_TABLE);
            if (c == NULL) {
                return -1;
            }
            UPropsTable *pt = (UPropsTable *)c;
            pt->n    = obj->shape->count;
            pt->_pad = 0U;
            for (uint32_t i = 0U; i < pt->n; i++) {
                pt->entries[i] = NULL;
            }
            pt->entries[idx] = fresh;
            obj->shape->props_table = pt->entries;
        } else {
            obj->shape->props_table[idx] = fresh;
        }
    } else {
        /* Genuine sibling.  transition_property already allocated its
         * props_table; write the new UProps into the slot's index. */
        new_shape->props_table[idx] = fresh;
        obj->shape = new_shape;
    }

    vm->topology_gen++;
    return 0;
}

int
urbi_object_remove_property(UVM *vm, UObject *obj, USymbol *name,
                            uint8_t flag_bit)
{
    if (vm == NULL || obj == NULL || name == NULL) {
        return -1;
    }
    int32_t idx = urbi_shape_find_slot(obj->shape, name);
    if (idx < 0) {
        return -1;
    }

    /* If the slot has no UProps, nothing to remove. */
    UProps *existing = (obj->shape->props_table != NULL)
                       ? obj->shape->props_table[idx]
                       : NULL;
    if (existing == NULL) {
        return 0;   /* silent no-op */
    }

    /* Materialise sibling shape clearing flag_bit.  install=0. */
    UShape *new_shape = urbi_shape_transition_property(vm, obj->shape,
                                                       (uint32_t)idx,
                                                       flag_bit, 0);
    if (new_shape == NULL) {
        return -1;
    }

    /* Build a fresh UProps with the requested bit cleared.  If all bits are
     * now clear AND constant==0, write NULL (drop the UProps entirely). */
    UProps *fresh = uprops_alloc(vm);
    if (fresh == NULL) {
        return -1;
    }
    fresh->oget     = existing->oget;
    fresh->oset     = existing->oset;
    fresh->constant = existing->constant;
    int all_clear = 0;
    if (flag_bit == URBI_SLOT_FLAG_OGET) {
        fresh->oget.kind = UVAL_VOID;
        fresh->oget.v.i  = 0;
    } else if (flag_bit == URBI_SLOT_FLAG_OSET) {
        fresh->oset.kind = UVAL_VOID;
        fresh->oset.v.i  = 0;
    } else if (flag_bit == URBI_SLOT_FLAG_CONSTANT) {
        fresh->constant = 0U;
    } else {
        return -1;
    }
    all_clear = (fresh->oget.kind == UVAL_VOID
                 && fresh->oset.kind == UVAL_VOID
                 && fresh->constant == 0U);

    UProps *publish = all_clear ? NULL : fresh;

    if (new_shape == obj->shape) {
        /* Idempotent — write directly into the existing props_table. */
        if (obj->shape->props_table != NULL) {
            obj->shape->props_table[idx] = publish;
        }
    } else {
        new_shape->props_table[idx] = publish;
        obj->shape = new_shape;
    }

    vm->topology_gen++;
    return 0;
}

int
urbi_object_set_property_value(UVM *vm, UObject *obj, USymbol *name,
                               uint8_t flag_bit, UValue value)
{
    if (vm == NULL || obj == NULL || name == NULL) {
        return -1;
    }
    int32_t idx = urbi_shape_find_slot(obj->shape, name);
    if (idx < 0) {
        return -1;
    }
    if (obj->shape->props_table == NULL
        || obj->shape->props_table[idx] == NULL) {
        return -1;   /* no UProps to mutate */
    }
    UProps *p = obj->shape->props_table[idx];
    if (flag_bit == URBI_SLOT_FLAG_OGET) {
        p->oget = value;
    } else if (flag_bit == URBI_SLOT_FLAG_OSET) {
        p->oset = value;
    } else {
        return -1;
    }
    /* Bumping topology_gen here is the load-bearing invariant per topology
     * spec §4.1 row 7: cached IC uprops[] entries point at this same UProps
     * pointer, so the pointer itself is still live — but the value behind it
     * just changed and IC dispatch must re-fetch via the slow path to pick
     * up the new getter/setter. */
    vm->topology_gen++;
    return 0;
}

/* === T25: urbi_object_resolve_slot ===
 *
 * Per pre-M4 GETSLOT/SETSLOT spec §6.3.  Same DFS shape as lookup_inner
 * (left-first, cycle-safe via lookup_stamp), but captures (holder, index)
 * rather than the slot value so the IC slow path can fill cache entries
 * with a direct USlot* into the holding object's storage.
 *
 * Iterative DFS over a fixed 64-deep stack to bound prototype-graph depth
 * (consistent with the legacy walker; deeper graphs are exotic and may be
 * promoted to a heap-allocated stack in v1.x).  Stack overflow returns -1
 * so the caller can surface a diagnostic. */
int
urbi_object_resolve_slot(UVM *vm, UObject *recv, USymbol *name,
                         UObject **out_holder, uint32_t *out_index)
{
    if (vm == NULL || recv == NULL || name == NULL
        || out_holder == NULL || out_index == NULL) {
        return -1;
    }

    /* Same wrap protocol as urbi_object_lookup: pre-bump if safe, otherwise
     * force a clear pass and reset to 1.  This pins lookup_stamp uniqueness
     * for the entire DFS below. */
    if ((uint32_t)(vm->lookup_id + 1ULL) == 0U) {
        urbi_object_lookup_id_force_wrap(vm);
    } else {
        vm->lookup_id++;
    }

    UObject *stack[URBI_RESOLVE_STACK_CAP];
    int sp = 0;
    stack[sp++] = recv;

    while (sp > 0) {
        UObject *cur = stack[--sp];
        if (cur->lookup_stamp == (uint32_t)vm->lookup_id) {
            continue;
        }
        cur->lookup_stamp = (uint32_t)vm->lookup_id;

        int32_t idx = urbi_shape_find_slot(cur->shape, name);
        if (idx >= 0) {
            *out_holder = cur;
            *out_index  = (uint32_t)idx;
            return 1;
        }

        /* Push protos in reverse so left-first DFS pops them in declaration
         * order (mirrors UPROTOS_FOREACH iteration order). */
        uint32_t n = urbi_object_proto_count(cur);
        for (uint32_t i = n; i > 0U; i--) {
            if (sp >= URBI_RESOLVE_STACK_CAP) {
                return -1;   /* depth overflow — caller raises diagnostic */
            }
            stack[sp++] = urbi_object_proto_at(cur, i - 1U);
        }
    }

    return 0;
}
