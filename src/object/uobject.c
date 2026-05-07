/* SPDX-License-Identifier: BSD-3-Clause */
/* uobject.c — atom-family singletons + UObject allocator (M4 / T8).
 *
 * Spec references:
 *   docs/superpowers/specs/2026-04-29-urbi-pre-m4-prototype-chain-representation-design.md §3, §4.1, §8.1
 *
 * Per-VM lazy-allocated atom prototypes: root Object plus the eight built-in
 * atoms (Integer/Float/String/List/Dict/Tag/Event/Symbol).  T36's root
 * provider (m4_object_roots_walker, registered via urbi_object_register_gc_roots
 * in uvm_init) keeps the singletons alive across GC cycles by shading each
 * non-NULL vm->atom_* field directly during MARK_ROOTS.
 *
 * The single-tag prototype encoding `(root << 1) | 1` used in
 * urbi_object_atom matches the canonical form decoded by UPROTOS_FOREACH
 * (src/object/uobject.h, T9). */

#include <stddef.h>       /* offsetof */
#include <stdint.h>

#include "object/uobject.h"
#include "object/uobject_internal.h"
#include "object/ushape.h"
#include "object/umodule_instance.h"  /* T36: walk module_instances_head */
#include "value/uintern.h"      /* T40: ustr_intern("fallback", ...) */
#include "vm/uvm.h"
#include "urbi/gc.h"      /* urbi_gc_alloc + urbi_gc_register_root_provider */
#include "gc/ugc_incremental.h"   /* gc_shade_gray (T10), urbi_gc_walk_all_cells (T12) */
#include "urbi/object.h"
#include "urbi/urbi.h"    /* urbi_panic + URBI_OK / UErrCode */
#include "changed/uchanged_node.h"  /* urbi_emit_slot_change_if_subscribed (T65) */

/* === next_id ===
 *
 * Per-VM monotonic UObject identity counter (spec §8.1).
 *
 * vm->next_object_id is initialised to 0 by uvm_init; pre-increment yields
 * 1 on the first call, 2 on the second, etc.  At UINT32_MAX the next bump
 * would overflow — fatal-abort per spec §8.1 rather than silently wrap.
 *
 * Note: deviates from spec §8.1's pseudocode (which shows post-increment +
 * init=0, yielding ids 0..N-1) in favour of the established uvm.h comment
 * + plan test expectation that the first object gets id == 1.  The spec
 * pseudocode and v1.0 ABI agree on the monotonic-counter semantics; only
 * the initial offset disagrees, and 1-based ids leave 0 as a "no id"
 * sentinel for future debug printing. */
uint32_t
next_id(UVM *vm)
{
    if (vm->next_object_id == UINT32_MAX) {
        urbi_panic("URBI_FATAL_OBJECT_ID_EXHAUSTED");
    }
    return ++vm->next_object_id;
}

/* === urbi_object_alloc ===
 *
 * Allocate a fresh UObject in the given atom family.  Wires shape to the
 * root hidden class (no slots), protos to the empty form (0), and stamps
 * a fresh object_id.  Caller is responsible for pinning if the object is
 * a long-lived singleton. */
UObject *
urbi_object_alloc(UVM *vm, URBIAtomFamily family)
{
    UCell *c = urbi_gc_alloc(vm, sizeof(UObject), UTYPE_OBJECT);
    if (c == NULL) {
        return NULL;
    }
    UObject *o = (UObject *)c;

    /* shape: lazy-allocate the per-VM root if it hasn't been touched.
     * urbi_shape_root may itself OOM; if so, the UObject we just allocated
     * stays half-initialised — but it's a fresh GC cell, so the next sweep
     * reclaims it.  Returning NULL signals OOM to the caller. */
    o->shape = urbi_shape_root(vm);
    if (o->shape == NULL) {
        return NULL;
    }
    o->slots               = NULL;  /* zero-slot at construction; T15 lands slot transitions */
    o->protos              = 0u;   /* empty form per spec §4.1 */
    o->object_id           = next_id(vm);
    o->lookup_stamp        = 0u;
    o->flags               = (uint32_t)((uint32_t)family & URBI_OBJ_ATOM_MASK);
    o->reserved            = 0u;
    o->changed_events_head = NULL; /* lazy-alloc at first `obj.x.changed?` install (R6) */
    return o;
}

/* === urbi_atom_family_name ===
 *
 * Stable static string per atom family; used by future error messages
 * (T11 valid_proto failure path and beyond). */
const char *
urbi_atom_family_name(URBIAtomFamily f)
{
    switch (f) {
        case URBI_ATOM_OBJECT:  return "Object";
        case URBI_ATOM_INTEGER: return "Integer";
        case URBI_ATOM_FLOAT:   return "Float";
        case URBI_ATOM_STRING:  return "String";
        case URBI_ATOM_LIST:    return "List";
        case URBI_ATOM_DICT:    return "Dict";
        case URBI_ATOM_TAG:     return "Tag";
        case URBI_ATOM_EVENT:   return "Event";
        case URBI_ATOM_SYMBOL:  return "Symbol";
        default:                return "?";
    }
}

/* === urbi_object_root ===
 *
 * Lazy-allocate the per-VM root Object on first call.  The root has no
 * prototypes (protos field stays at 0 / empty form per spec §4.1).
 * Returns NULL on OOM. */
UObject *
urbi_object_root(struct UVM *vm)
{
    if (vm->atom_object != NULL) {
        return vm->atom_object;
    }

    UObject *o = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
    if (o == NULL) {
        return NULL;
    }
    /* protos already 0 (empty form) from urbi_object_alloc. */
    vm->atom_object = o;

    /* T36: m4_object_roots_walker (registered via urbi_object_register_gc_roots
     * in uvm_init) keeps this singleton alive across collection cycles by
     * shading vm->atom_object directly during MARK_ROOTS.  No explicit pin
     * needed — replaces the synthetic UVAL_CLOSURE wrapper trick used pre-T36. */
    return o;
}

/* === urbi_object_atom ===
 *
 * Lazy-allocate the named atom singleton on first call.  Each non-root
 * atom's protos field carries the single-tag encoding `(root << 1) | 1`
 * pointing at the root Object (the canonical single form decoded by
 * UPROTOS_FOREACH per pre-M4 prototype-chain spec §4.1).
 *
 * Returns NULL on OOM or invalid family tag. */
UObject *
urbi_object_atom(struct UVM *vm, URBIAtomFamilyTag family)
{
    UObject **slot;
    URBIAtomFamily internal_family;

    switch (family) {
        case URBI_ATOM_OBJECT_F:
            slot = &vm->atom_object;
            internal_family = URBI_ATOM_OBJECT;
            break;
        case URBI_ATOM_INTEGER_F:
            slot = &vm->atom_integer;
            internal_family = URBI_ATOM_INTEGER;
            break;
        case URBI_ATOM_FLOAT_F:
            slot = &vm->atom_float;
            internal_family = URBI_ATOM_FLOAT;
            break;
        case URBI_ATOM_STRING_F:
            slot = &vm->atom_string;
            internal_family = URBI_ATOM_STRING;
            break;
        case URBI_ATOM_LIST_F:
            slot = &vm->atom_list;
            internal_family = URBI_ATOM_LIST;
            break;
        case URBI_ATOM_DICT_F:
            slot = &vm->atom_dict;
            internal_family = URBI_ATOM_DICT;
            break;
        case URBI_ATOM_TAG_F:
            slot = &vm->atom_tag;
            internal_family = URBI_ATOM_TAG;
            break;
        case URBI_ATOM_EVENT_F:
            slot = &vm->atom_event;
            internal_family = URBI_ATOM_EVENT;
            break;
        case URBI_ATOM_SYMBOL_F:
            slot = &vm->atom_symbol;
            internal_family = URBI_ATOM_SYMBOL;
            break;
        default:
            return NULL;
    }

    if (*slot != NULL) {
        return *slot;
    }

    /* For URBI_ATOM_OBJECT_F, route through urbi_object_root so the
     * allocate-and-pin path is identical to a direct urbi_object_root call.
     * urbi_object_root sets vm->atom_object and pins; we then return the
     * cached value via *slot on the next call. */
    if (family == URBI_ATOM_OBJECT_F) {
        return urbi_object_root(vm);
    }

    /* Ensure the root Object exists first — every non-root atom's protos
     * field references it.  An OOM here propagates as NULL. */
    UObject *root = urbi_object_root(vm);
    if (root == NULL) {
        return NULL;
    }

    UObject *o = urbi_object_alloc(vm, internal_family);
    if (o == NULL) {
        return NULL;
    }
    /* Route through the canonical T10 primitive: empty → single transition,
     * with the forward Dijkstra barrier on the inserted root and a
     * topology_gen bump.  o was just allocated with protos == 0 (empty form)
     * so the "shade existing" branch is a no-op. */
    urbi_object_set_protos_single(vm, o, root);
    *slot = o;

    /* T36: kept alive by m4_object_roots_walker (see urbi_object_root). */
    return o;
}

/* === T39: urbi_object_clone ===
 *
 * Per pre-M2 §4.4 + atom-clone.chk.  Atom-aware clone:
 *   - Allocates a fresh UObject in parent's atom family (low-4 of flags).
 *   - Threads `parent` into the clone's protos as the single-tag form, so
 *     `clone.foo` resolves via the prototype walk to parent.foo (or any
 *     of parent's own prototypes).
 *   - Marks parent as IS_PROTOTYPE (via urbi_object_set_protos_single's
 *     monotonic flag set), so future slot installs on parent bump
 *     topology_gen and invalidate IC entries that walked through it.
 *
 * Returns NULL on NULL parent or OOM. */
UObject *
urbi_object_clone(UVM *vm, UObject *parent)
{
    if (vm == NULL || parent == NULL) {
        return NULL;
    }
    URBIAtomFamily fam =
        (URBIAtomFamily)(parent->flags & URBI_OBJ_ATOM_MASK);
    UObject *clone = urbi_object_alloc(vm, fam);
    if (clone == NULL) {
        return NULL;
    }
    /* set_protos_single fires the forward Dijkstra barrier on parent,
     * sets URBI_OBJ_FLAG_IS_PROTOTYPE on parent, and bumps topology_gen. */
    urbi_object_set_protos_single(vm, clone, parent);
    return clone;
}

/* === T12: cycle-safe DFS lookup ===
 *
 * Per pre-M4 prototype-chain spec §6 + GETSLOT/SETSLOT spec §6.5.
 *
 * The visited-set is encoded by stamping UObject.lookup_stamp with the low
 * 32 bits of vm->lookup_id; a re-visit (stamp == lookup_id) short-circuits.
 * Each top-level urbi_object_lookup call pre-bumps lookup_id, guaranteeing
 * the new id is fresh against every UObject's previous stamp.
 *
 * urbi_shape_find_slot is a stub at T12 (always returns -1); T13 lands the
 * real lineage walk.  Until then lookup_inner unconditionally falls into
 * the proto-walk path on every visit — which is exactly what the cycle and
 * rollover tests need to exercise. */

int
lookup_inner(UVM *vm, UObject *obj, USymbol *name, UValue *out)
{
    /* Cycle / re-visit guard.  Truncating lookup_id to u32 is intentional —
     * UObject.lookup_stamp is 4 bytes per spec §3 to keep the header at
     * 48 B.  Rollover is handled by urbi_object_lookup pre-checking the
     * top-level bump. */
    if (obj->lookup_stamp == (uint32_t)vm->lookup_id) {
        return 0;   /* already visited; not found via this branch */
    }
    obj->lookup_stamp = (uint32_t)vm->lookup_id;

    /* Local-slot fast path.  T12: urbi_shape_find_slot is a stub returning
     * -1, so this branch is never taken yet — obj->slots may be NULL.
     * T13 lands the real find; T26 lands slot-array growth that makes
     * obj->slots non-NULL once the first slot transitions in. */
    UShape *s = obj->shape;
    int32_t idx = urbi_shape_find_slot(s, name);
    if (idx >= 0) {
        *out = obj->slots[idx];
        return 1;
    }

    /* Proto-walk: left-first DFS via UPROTOS_FOREACH (visits items[0]
     * first per spec §6.1).  Recursion depth is bounded by the proto
     * graph depth; cycles are short-circuited by the lookup_stamp guard
     * at function entry. */
    UObject *p;
    UPROTOS_FOREACH(obj, p) {
        int rc = lookup_inner(vm, p, name, out);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

int
urbi_object_lookup(UVM *vm, UObject *obj, USymbol *name, UValue *out)
{
    /* Rollover check: if the next u32 truncation of (lookup_id + 1) would
     * be 0 (the "no stamp" sentinel), force a clear-pass and reset to 1
     * BEFORE doing the increment.  Otherwise pre-bump so the new id is
     * fresh against every UObject's previous stamp. */
    if ((uint32_t)(vm->lookup_id + 1ull) == 0u) {
        urbi_object_lookup_id_force_wrap(vm);
        /* force_wrap leaves lookup_id == 1, which is fresh after the
         * just-cleared stamps. */
    } else {
        vm->lookup_id++;
    }

    int rc = lookup_inner(vm, obj, name, out);
    if (rc == 1) {
        return 0;   /* hit */
    }

    /* T40: GET_FALLBACK retry per pre-M2 §4.3.  On full-tree miss, retry
     * once with name = "fallback".  If fallback hits, return its value;
     * the caller is responsible for invoking it as a method.  v1.0 does
     * NOT implement the legacy `call.message` reflection layer (per
     * third-party-corpus-compatibility-audit B disposition); fallback
     * receives the bare value and must dispatch via plain function args.
     *
     * Cycle-safety: if `name` itself is "fallback", skip the retry —
     * otherwise lookup of "fallback" missing on an object without one
     * would recurse forever.  Bump lookup_id again so the second pass
     * uses a fresh stamp against UObjects that were marked during the
     * first pass; rollover-guard is identical to the entry path. */
    USymbol *fb = (USymbol *)ustr_intern(vm, "fallback", 8);
    if (name == fb) {
        return -1;
    }
    if ((uint32_t)(vm->lookup_id + 1ull) == 0u) {
        urbi_object_lookup_id_force_wrap(vm);
    } else {
        vm->lookup_id++;
    }
    rc = lookup_inner(vm, obj, fb, out);
    return (rc == 1) ? 0 : -1;
}

/* clear_lookup_stamp_cb — urbi_gc_walk_all_cells callback that resets
 * UObject.lookup_stamp to 0 on every UObject cell.  Skips non-object cells. */
void
clear_lookup_stamp_cb(UVM *vm, UCell *cell, void *ctx)
{
    (void)vm; (void)ctx;
    if (cell->type_tag == UTYPE_OBJECT) {
        ((UObject *)cell)->lookup_stamp = 0u;
    }
}

void
urbi_object_lookup_id_force_wrap(UVM *vm)
{
    /* Walk every GC cell, zero lookup_stamp on UObject cells.  T36 may
     * fold this into the mark phase to avoid a separate iteration; for
     * now an immediate dedicated pass is correct (per spec §7.2). */
    urbi_gc_walk_all_cells(vm, clear_lookup_stamp_cb, NULL);
    vm->lookup_id = 1ull;
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
    fresh->_pad = 0u;

    /* Copy old slot values into the new wrapper.  obj->shape->count is
     * the old slot count (one less than new_shape->count). */
    for (uint32_t i = 0u; i < obj->shape->count; i++) {
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
    if (new_shape->count > 0u) {
        UCell *c = urbi_gc_alloc(vm,
                                 sizeof(USlotArray)
                                 + (size_t)new_shape->count * sizeof(USlot),
                                 UTYPE_SLOT_ARRAY);
        if (c == NULL) {
            return -1;
        }
        fresh = (USlotArray *)c;
        fresh->n    = new_shape->count;
        fresh->_pad = 0u;

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
    p->constant      = 0u;
    p->_spare        = 0u;
    return p;
}

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
        fresh->constant = 1u;
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
            pt->_pad = 0u;
            for (uint32_t i = 0u; i < pt->n; i++) {
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
        fresh->constant = 0u;
    } else {
        return -1;
    }
    all_clear = (fresh->oget.kind == UVAL_VOID
                 && fresh->oset.kind == UVAL_VOID
                 && fresh->constant == 0u);

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
#define URBI_RESOLVE_STACK_CAP 64

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
    if ((uint32_t)(vm->lookup_id + 1ull) == 0u) {
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
        for (uint32_t i = n; i > 0u; i--) {
            if (sp >= URBI_RESOLVE_STACK_CAP) {
                return -1;   /* depth overflow — caller raises diagnostic */
            }
            stack[sp++] = urbi_object_proto_at(cur, i - 1u);
        }
    }

    return 0;
}

/* === T36: GC root provider for atom singletons + UModuleInstance list ===
 *
 * Per pre-M3 GC roots spec §5.3 + pre-M4 amendments.  Three new root sources:
 *   1. Atom-family singletons (vm->atom_object .. vm->atom_symbol).
 *   2. The root shape (vm->root_shape).
 *   3. UModuleInstance chain reachable from vm->module_instances_head.
 *
 * Each is a direct UCell pointer (not a UValue), so we shade via
 * gc_shade_gray rather than calling cb (mark_root_callback acts on UValue
 * slots — UVAL_CLOSURE / UVAL_OBJECT — which doesn't fit the singleton
 * pointers held in UVM fields).  The cb / ctx parameters are unused; their
 * presence keeps the UGcRootProviderFn signature uniform across providers.
 *
 * Children are reached via the registered walkers in src/object/utypes_init.c:
 *   - walk_uobject shades shape, slots, and the proto chain
 *   - walk_ushape shades parent + transitions + props_table contents
 *   - walk_umoduleinstance shades the proto_instances UProtoInstanceArr
 * Once a UModuleInstance is alive, its UProtoInstance entries (containing UIC
 * caches) keep the receiver shapes / slot pointers / uprops cached entries
 * reachable through walk_uprotoinstance (T22+ wiring lands on cache fill). */
static void
m4_object_roots_walker(UVM *vm, UGcRootCallback cb, void *ctx)
{
    (void)cb; (void)ctx;   /* direct gc_shade_gray; cb only handles UValue slots */

    /* Atom-family singletons. */
    if (vm->atom_object  != NULL) gc_shade_gray(vm, (UCell *)vm->atom_object);
    if (vm->atom_integer != NULL) gc_shade_gray(vm, (UCell *)vm->atom_integer);
    if (vm->atom_float   != NULL) gc_shade_gray(vm, (UCell *)vm->atom_float);
    if (vm->atom_string  != NULL) gc_shade_gray(vm, (UCell *)vm->atom_string);
    if (vm->atom_list    != NULL) gc_shade_gray(vm, (UCell *)vm->atom_list);
    if (vm->atom_dict    != NULL) gc_shade_gray(vm, (UCell *)vm->atom_dict);
    if (vm->atom_tag     != NULL) gc_shade_gray(vm, (UCell *)vm->atom_tag);
    if (vm->atom_event   != NULL) gc_shade_gray(vm, (UCell *)vm->atom_event);
    if (vm->atom_symbol  != NULL) gc_shade_gray(vm, (UCell *)vm->atom_symbol);

    /* M5 T53/T54 native proto objects. */
    if (vm->event_proto != NULL) gc_shade_gray(vm, (UCell *)vm->event_proto);
    if (vm->tag_proto   != NULL) gc_shade_gray(vm, (UCell *)vm->tag_proto);

    /* Root shape. */
    if (vm->root_shape != NULL) gc_shade_gray(vm, (UCell *)vm->root_shape);

    /* UModuleInstance chain (each cell's IC tables + proto_instances are
     * traced by walk_umoduleinstance / walk_uprotoinstance). */
    for (UModuleInstance *mi = vm->module_instances_head;
         mi != NULL;
         mi = mi->next_in_vm) {
        gc_shade_gray(vm, (UCell *)mi);
    }
}

void
urbi_object_register_gc_roots(struct UVM *vm)
{
    urbi_gc_register_root_provider(vm, m4_object_roots_walker);
}
