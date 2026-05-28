/* SPDX-License-Identifier: BSD-3-Clause */
/* uobject_slot.c — slot install / remove / property primitives.
 * Extracted from uobject.c during v0.5.4-decompose (OBJ-045 #4). */

#include <stddef.h>   /* offsetof */
#include <stdint.h>

#include "object/uobject.h"
#include "object/uobject_internal.h"
#include "object/ushape.h"
#include "vm/uvm.h"
#include "urbi/types.h"         /* URBI_OK / URBI_ERR_* — OBJ-007 distinct codes */
#include "urbi/gc.h"            /* urbi_gc_alloc + urbi_gc_slot_store barrier */
#include "gc/ugc_incremental.h" /* gc_shade_gray + urbi_gc_slot_store */
#include "gc/ugc.h"             /* UTYPE_SLOT_ARRAY / UTYPE_PROPS / UTYPE_PROPS_TABLE */
#include "changed/uchanged_node.h" /* urbi_emit_slot_change_if_subscribed */
#include "chunk/uchunk.h"

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
 *      USlotArray reallocation, no topology_gen bump.  The new value is
 *      written through the combined barrier + store (urbi_gc_slot_store):
 *      if the receiver UObject is BLACK and the new value is a white
 *      heap cell, the barrier shades the new value GRAY to maintain the
 *      tri-color invariant.  The old value (about to be overwritten)
 *      remains reachable through whichever other paths held it; that's
 *      the GC's responsibility, not the slot-write site's.
 *
 *      OBJ-003: the previous comment asserted "OLD value cannot point at
 *      a black-marked cell" — backwards reasoning.  Forward Dijkstra
 *      protects the *new* value, not the old.
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

    /* Case 1: slot already in this lineage — in-place value update.
     * Route through the combined barrier + store (urbi_gc_slot_store): if the
     * receiver UObject cell is BLACK and the new value is a white
     * heap cell, the barrier shades the new value gray.  Without the
     * barrier, the new white child under a black parent would be
     * unreachable in the mark phase and could be swept while still
     * reachable via this slot.  The combined helper performs barrier +
     * store atomically from the caller's point of view (F12). */
    int32_t existing = urbi_shape_find_slot(obj->shape, name);
    if (existing >= 0) {
        urbi_gc_slot_store(vm, (UCell *)obj, (uint32_t)existing,
                           &obj->slots[existing], value);
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
        /* TIDY-006: single (char *) intermediate avoids casting-through-void
         * on uint8_t * → UCell *.  The container_of offsetof recovery is
         * alignment-safe by USlotArray's layout. */
        UCell *old_wrapper = (UCell *)
            ((char *)obj->slots - offsetof(USlotArray, entries));
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
    /* Install (first-time slot creation) is NOT a slot change per the legacy
     * semantic.  Only subsequent writes to an existing slot fire the watcher.
     * Do NOT call urbi_emit_slot_change_if_subscribed here.  The in-place
     * update branch (Case 1 above) is the correct emit site. */
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
urbi_object_remove_slot(UVM *vm, UObject *obj, const USymbol *name)
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
        /* TIDY-006: single (char *) intermediate avoids casting-through-void
         * on uint8_t * → UCell *.  The container_of offsetof recovery is
         * alignment-safe by USlotArray's layout. */
        UCell *old_wrapper = (UCell *)
            ((char *)obj->slots - offsetof(USlotArray, entries));
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

/* uvalue_eq — bitwise equality on a UValue's tag + payload.  Sufficient
 * for the v1.0 "is this the same install value?" idempotent-install
 * detection in urbi_object_install_property: pointer-identity for heap-
 * bearing kinds, integer/float bit-equality otherwise. */
static int
uvalue_eq(UValue a, UValue b)
{
    return a.kind == b.kind && a.v.i == b.v.i;
}

int
urbi_object_install_property(UVM *vm, UObject *obj, const USymbol *name,
                             uint8_t flag_bit, UValue value)
{
    /* OBJ-007: distinguish OOM from invalid-arg / slot-not-found.
     * Returns:
     *   URBI_OK                    on success or idempotent no-op
     *   URBI_ERR_INVALID_ARG       NULL args or unsupported flag_bit
     *   URBI_ERR_SLOT_NOT_FOUND    slot does not exist on obj's lineage
     *   URBI_ERR_OOM               shape transition / UProps allocation OOM */
    if (vm == NULL || obj == NULL || name == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    int32_t idx = urbi_shape_find_slot(obj->shape, name);
    if (idx < 0) {
        return URBI_ERR_SLOT_NOT_FOUND;   /* slot must exist before installing */
    }
    /* Reject unsupported flag_bit BEFORE any allocation. */
    if (flag_bit != URBI_SLOT_FLAG_OGET
        && flag_bit != URBI_SLOT_FLAG_OSET
        && flag_bit != URBI_SLOT_FLAG_CONSTANT) {
        return URBI_ERR_INVALID_ARG;
    }

    /* OBJ-041: detect a TRUE no-op idempotent install — flag bit already
     * set AND the existing UProps's relevant field already holds the
     * same value.  In that case the install is observably equivalent to
     * doing nothing: no shape transition, no UProps allocation, no
     * topology_gen bump.
     *
     * The shape's per-slot 4-bit nibble in `flags` is the cheap source
     * of truth for "is the bit already set?" — see ushape.c §4.1.  For
     * indices >= 8 we conservatively skip the no-op detection because
     * the v1.0 packed nibble doesn't represent those slots. */
    if (idx < 8) {
        const uint32_t shift      = (uint32_t)idx * 4U;
        const uint32_t old_nibble = (obj->shape->flags >> shift) & 0xFU;
        const int already_set = (old_nibble & ((uint32_t)flag_bit & 0xFU)) != 0U;
        if (already_set
            && obj->shape->props_table != NULL
            && obj->shape->props_table[idx] != NULL) {
            const UProps *cur = obj->shape->props_table[idx];
            int same_value = 0;
            if (flag_bit == URBI_SLOT_FLAG_OGET) {
                same_value = uvalue_eq(cur->oget, value);
            } else if (flag_bit == URBI_SLOT_FLAG_OSET) {
                same_value = uvalue_eq(cur->oset, value);
            } else {
                /* CONSTANT carries no payload; the bit-already-set check
                 * above is sufficient — re-installing is a true no-op. */
                same_value = 1;
            }
            if (same_value) {
                return 0;   /* idempotent no-op: no allocation, no bump */
            }
        }
    }

    /* Materialise sibling shape (or clone) FIRST.  OBJ-006: if this
     * step OOMs, no UProps cell is left dangling.
     *
     * Two cases — non-idempotent transition vs idempotent (flag already
     * set).  In the idempotent case `urbi_shape_transition_property`
     * returns parent unchanged; we fork a fresh sibling clone in line
     * with OBJ-005 so the publish step never mutates `obj->shape`. */
    UShape *new_shape = urbi_shape_transition_property(vm, obj->shape,
                                                       (uint32_t)idx,
                                                       flag_bit, 1);
    if (new_shape == NULL) {
        return URBI_ERR_OOM;
    }

    UPropsTable *clone_pt = NULL;
    UShape *clone = NULL;
    if (new_shape == obj->shape) {
        /* OBJ-005: idempotent transition — the existing shape is
         * potentially shared with other UObjects.  Allocate a fresh
         * sibling clone (same lineage / flags / identity-fresh / fresh
         * props_table) BEFORE the UProps so that an OOM at any point
         * leaks nothing GC-managed but a sibling shape (which the next
         * sweep reclaims). */
        UCell *sc = urbi_gc_alloc(vm, sizeof(UShape), UTYPE_SHAPE);
        if (sc == NULL) {
            return URBI_ERR_OOM;
        }
        clone = (UShape *)sc;
        clone->name        = obj->shape->name;
        clone->index       = obj->shape->index;
        clone->count       = obj->shape->count;
        clone->flags       = obj->shape->flags;
        clone->_pad        = 0U;
        clone->parent      = obj->shape->parent;
        clone->transitions = NULL;
        clone->props_table = NULL;

        UCell *pc = urbi_gc_alloc(vm,
                                  sizeof(UPropsTable)
                                  + (size_t)obj->shape->count
                                    * sizeof(UProps *),
                                  UTYPE_PROPS_TABLE);
        if (pc == NULL) {
            return URBI_ERR_OOM;
        }
        clone_pt = (UPropsTable *)pc;
        clone_pt->n    = obj->shape->count;
        clone_pt->_pad = 0U;
        for (uint32_t i = 0U; i < clone_pt->n; i++) {
            clone_pt->entries[i] = (obj->shape->props_table != NULL)
                                 ? obj->shape->props_table[i]
                                 : NULL;
        }
    }

    /* Allocate the fresh UProps now that the shape transition has
     * succeeded.  OBJ-006: this allocation order ensures that a
     * transition-failure path leaves no UProps cell dangling. */
    const UProps *existing = (obj->shape->props_table != NULL)
                       ? obj->shape->props_table[idx]
                       : NULL;
    UProps *fresh = uprops_alloc(vm);
    if (fresh == NULL) {
        return URBI_ERR_OOM;
    }
    if (existing != NULL) {
        fresh->oget     = existing->oget;
        fresh->oset     = existing->oset;
        fresh->constant = existing->constant;
    }
    if (flag_bit == URBI_SLOT_FLAG_OGET) {
        fresh->oget = value;
    } else if (flag_bit == URBI_SLOT_FLAG_OSET) {
        fresh->oset = value;
    } else {
        fresh->constant = 1U;
        (void)value;   /* CONSTANT carries no payload */
    }

    /* Publish: write the new UProps into the destination shape's
     * props_table[idx]. */
    if (clone != NULL) {
        clone_pt->entries[idx] = fresh;
        clone->props_table     = clone_pt->entries;
        obj->shape             = clone;
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
urbi_object_remove_property(UVM *vm, UObject *obj, const USymbol *name,
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
    const UProps *existing = (obj->shape->props_table != NULL)
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
urbi_object_set_property_value(UVM *vm, UObject *obj, const USymbol *name,
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
    if (flag_bit != URBI_SLOT_FLAG_OGET
        && flag_bit != URBI_SLOT_FLAG_OSET) {
        return -1;
    }

    /* OBJ-018: copy-on-write the UProps cell before mutation.  The
     * existing UProps* may be shared via UPropsTable seeding with a
     * sibling shape (see ushape.c::alloc_props_table); an in-place
     * write of `oget`/`oset` would propagate the mutation to every
     * shape that aliased this UProps pointer.  Allocating a fresh
     * UProps and rewriting the props_table[idx] entry isolates this
     * shape's view from any aliasing sibling. */
    const UProps *existing = obj->shape->props_table[idx];
    UProps *fresh = uprops_alloc(vm);
    if (fresh == NULL) {
        return -1;
    }
    fresh->oget     = existing->oget;
    fresh->oset     = existing->oset;
    fresh->constant = existing->constant;
    if (flag_bit == URBI_SLOT_FLAG_OGET) {
        fresh->oget = value;
    } else {
        fresh->oset = value;
    }
    obj->shape->props_table[idx] = fresh;

    /* Bumping topology_gen here is the load-bearing invariant per topology
     * spec §4.1 row 7: cached IC uprops[] entries point at the old UProps
     * pointer, which is now stale — IC dispatch must re-fetch via the
     * slow path to pick up the new UProps cell. */
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
urbi_object_resolve_slot(UVM *vm, UObject *recv, const USymbol *name,
                         UObject **out_holder, uint32_t *out_index)
{
    if (vm == NULL || recv == NULL || name == NULL
        || out_holder == NULL || out_index == NULL) {
        return -1;
    }

    /* Same wrap protocol as urbi_object_lookup: pre-bump if safe, otherwise
     * force a clear pass and reset to 1.  This pins lookup_stamp uniqueness
     * for the entire DFS below.
     *
     * CPPCHK-002: cppcheck flags this as always-false because it assumes a
     * 32-bit lookup_id; vm->lookup_id is uint64_t and the cast catches the
     * actual u32 wrap.  Suppressed via .cppcheck.suppressions. */
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
