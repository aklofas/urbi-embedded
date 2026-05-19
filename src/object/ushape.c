/* SPDX-License-Identifier: BSD-3-Clause */
/* ushape.c — UShape root singleton + transition primitives.
 *
 * Spec references:
 *   docs/superpowers/specs/2026-04-24-urbi-pre-m2-object-model-design.md §3, §7.1, §7.2
 *   docs/superpowers/specs/2026-04-29-urbi-pre-m4-uslot-uprops-collapse-design.md §4.1, §4.2
 *
 * The root-shape singleton is owned by UVM (vm->root_shape) and lazily
 * allocated on the first urbi_shape_root call.  All root-shape fields are
 * zero except cell (filled by urbi_gc_alloc).
 *
 * T13 lands the transition cache (UShapeMap), the real
 * urbi_shape_transition_add_slot, and the real urbi_shape_find_slot.
 * T17 lands the sibling-shape primitive (urbi_shape_transition_property)
 * and the lazy UPropsTable wrapper allocation. */

#include <stddef.h>
#include <stdint.h>

#include "object/ushape.h"
#include "object/uobject.h"   /* URBI_SLOT_FLAG_* bits */
#include "vm/uvm.h"
#include "urbi/gc.h"          /* urbi_gc_alloc, UTYPE_SHAPE_MAP, UTYPE_PROPS_TABLE */
#include "gc/ugc.h"
#include "chunk/uchunk.h"

/* === Transition-cache helpers (file-private) === */

#define USHAPE_INITIAL_CAP   8U

/* map_alloc — allocate a fresh UShapeMap with `cap` empty entries.
 * cap MUST be a power of two.  Returns NULL on OOM. */
static UShapeMap *
map_alloc(struct UVM *vm, uint32_t cap)
{
    UCell *c = urbi_gc_alloc(vm,
                             sizeof(UShapeMap)
                             + (size_t)cap * sizeof(((UShapeMap *)0)->entries[0]),
                             UTYPE_SHAPE_MAP);
    if (c == NULL) {
        return NULL;
    }
    UShapeMap *m = (UShapeMap *)c;
    m->cap   = cap;
    m->count = 0U;
    m->_pad  = 0U;
    for (uint32_t i = 0U; i < cap; i++) {
        m->entries[i].k = NULL;
        m->entries[i].v = NULL;
    }
    return m;
}

/* Hash USymbol pointer to a table slot.  Symbols are interned → pointer
 * identity is the only valid comparison; the >>4 strips alignment-zero
 * bits so adjacent allocations don't all collide on bucket 0. */
static inline uint32_t
map_hash(const USymbol *k, uint32_t mask)
{
    return (uint32_t)(((uintptr_t)k >> 4) & mask);
}

/* Linear-probe lookup.  Returns the entry's UShape* on hit, NULL on miss.
 * cap is a power of two so mask = cap-1.  k MUST be non-NULL (NULL marks
 * empty slots). */
static UShape *
map_get(const UShapeMap *m, const USymbol *k)
{
    const uint32_t mask = m->cap - 1U;
    uint32_t i = map_hash(k, mask);
    for (uint32_t probes = 0U; probes < m->cap; probes++) {
        if (m->entries[i].k == NULL) {
            return NULL;            /* empty slot — terminates the probe */
        }
        if (m->entries[i].k == k) {
            return m->entries[i].v;
        }
        i = (i + 1U) & mask;
    }
    return NULL;
}

/* Linear-probe insert.  Caller has already ensured spare capacity (load
 * < 75% post-insert).  Inserting a duplicate key overwrites the existing
 * value but does NOT bump count — at v1.0 the only call site
 * (urbi_shape_transition_add_slot) checks for existing entry first, so
 * overwrite never actually happens.  k MUST be non-NULL. */
static void
map_put(UShapeMap *m, USymbol *k, UShape *v)
{
    const uint32_t mask = m->cap - 1U;
    uint32_t i = map_hash(k, mask);
    while (m->entries[i].k != NULL && m->entries[i].k != k) {
        i = (i + 1U) & mask;
    }
    if (m->entries[i].k == NULL) {
        m->count++;
    }
    m->entries[i].k = k;
    m->entries[i].v = v;
}

/* === Public API === */

UShape *urbi_shape_root(struct UVM *vm)
{
    if (vm->root_shape != NULL) {
        return vm->root_shape;
    }

    UCell *c = urbi_gc_alloc(vm, sizeof(UShape), UTYPE_SHAPE);
    if (c == NULL) {
        return NULL;
    }

    UShape *s = (UShape *)c;
    s->name        = NULL;
    s->index       = 0U;
    s->count       = 0U;
    s->flags       = 0U;
    s->_pad        = 0U;
    s->parent      = NULL;
    s->transitions = NULL;
    s->props_table = NULL;

    vm->root_shape = s;
    return s;
}

UShape *urbi_shape_transition_add_slot(struct UVM *vm, UShape *parent,
                                       USymbol *name)
{
    if (vm == NULL || parent == NULL || name == NULL) {
        return NULL;
    }

    /* 1. Lazy-allocate the transitions cache on first transition. */
    if (parent->transitions == NULL) {
        parent->transitions = map_alloc(vm, USHAPE_INITIAL_CAP);
        if (parent->transitions == NULL) {
            return NULL;
        }
    } else {
        /* 2. Check existing cache for this name. */
        UShape *cached = map_get(parent->transitions, name);
        if (cached != NULL) {
            return cached;
        }
    }

    /* 3. Resize cache to 2x if inserting would push load >= 75%.
     * Comparison ((count + 1) * 4) >= (cap * 3) avoids float math. */
    if (((parent->transitions->count + 1U) * 4U)
        >= (parent->transitions->cap * 3U)) {
        UShapeMap *bigger = map_alloc(vm, parent->transitions->cap * 2U);
        if (bigger == NULL) {
            return NULL;
        }
        /* Rehash existing entries into the bigger table. */
        for (uint32_t i = 0U; i < parent->transitions->cap; i++) {
            if (parent->transitions->entries[i].k != NULL) {
                map_put(bigger,
                        parent->transitions->entries[i].k,
                        parent->transitions->entries[i].v);
            }
        }
        parent->transitions = bigger;
    }

    /* 4. Allocate the child UShape. */
    UCell *cc = urbi_gc_alloc(vm, sizeof(UShape), UTYPE_SHAPE);
    if (cc == NULL) {
        return NULL;
    }
    UShape *child = (UShape *)cc;
    child->name        = name;
    child->index       = parent->count;
    child->count       = parent->count + 1U;
    child->flags       = parent->flags;
    child->_pad        = 0U;
    child->parent      = parent;
    child->transitions = NULL;
    child->props_table = NULL;

    /* 5. Insert into the cache. */
    map_put(parent->transitions, name, child);
    return child;
}

/* alloc_props_table — allocate a UPropsTable wrapper sized for `n` entries
 * and seed each entry from `seed` (if non-NULL) or NULL.  Returns NULL on
 * OOM.  n == 0 returns NULL (caller treats as "no table"). */
static UPropsTable *
alloc_props_table(struct UVM *vm, uint32_t n, UProps *const *seed)
{
    if (n == 0U) {
        return NULL;
    }
    UCell *c = urbi_gc_alloc(vm,
                             sizeof(UPropsTable)
                             + (size_t)n * sizeof(UProps *),
                             UTYPE_PROPS_TABLE);
    if (c == NULL) {
        return NULL;
    }
    UPropsTable *pt = (UPropsTable *)c;
    pt->n    = n;
    pt->_pad = 0U;
    if (seed != NULL) {
        for (uint32_t i = 0U; i < n; i++) {
            pt->entries[i] = seed[i];
        }
    } else {
        for (uint32_t i = 0U; i < n; i++) {
            pt->entries[i] = NULL;
        }
    }
    return pt;
}

UShape *urbi_shape_transition_property(struct UVM *vm, UShape *parent,
                                       uint32_t slot_index,
                                       uint8_t flag_bit, int install)
{
    /* Sibling-shape materialisation per pre-M2 §7.2 + pre-M4 USlot/UProps
     * spec §5.1, §5.2.  Allocates a fresh sibling that shares parent's
     * lineage but carries the new flag bit at slot_index's nibble and a
     * copy-on-write props_table (caller writes the per-slot UProps* into
     * sibling->props_table[slot_index] post-transition). */
    if (vm == NULL || parent == NULL) {
        return NULL;
    }
    if (parent->count == 0U || slot_index >= parent->count) {
        return NULL;            /* no slot to attach a property to */
    }

    /* Compute new flag nibble at slot_index.  Per pre-M4 USlot/UProps
     * spec §4.1, UShape.flags packs 4 bits/slot across slots (v1.0 cap of
     * 8 slots in the packed form — spill side-table deferred to T-later). */
    const uint32_t shift = slot_index * 4U;
    const uint32_t old_nibble = (parent->flags >> shift) & 0xFU;
    const uint32_t fb = (uint32_t)flag_bit & 0xFU;
    const uint32_t new_nibble = install ? (old_nibble | fb)
                                        : (old_nibble & ~fb);
    if (old_nibble == new_nibble) {
        return parent;          /* idempotent no-op */
    }
    const uint32_t new_flags = (parent->flags & ~(0xFU << shift))
                             | (new_nibble << shift);

    /* Allocate sibling shape (shallow copy with overrides).  Sibling shares
     * parent->parent (NOT parent itself) — it represents the same set of
     * slots, just with different per-slot property bits. */
    UCell *sc = urbi_gc_alloc(vm, sizeof(UShape), UTYPE_SHAPE);
    if (sc == NULL) {
        return NULL;
    }
    UShape *sibling = (UShape *)sc;
    sibling->name        = parent->name;
    sibling->index       = parent->index;
    sibling->count       = parent->count;
    sibling->flags       = new_flags;
    sibling->_pad        = 0U;
    sibling->parent      = parent->parent;
    sibling->transitions = NULL;   /* sibling builds its own future cache */
    sibling->props_table = NULL;

    /* Allocate sibling's props_table, seeding entries from parent's table
     * when present (copy-on-write).  Caller writes the per-slot UProps*
     * for slot_index post-transition. */
    UPropsTable *pt = alloc_props_table(vm, parent->count,
                                        parent->props_table);
    if (pt == NULL) {
        /* OOM on the props_table cell.  sibling is GC-managed; sweep
         * reaps it.  No partial state to undo. */
        return NULL;
    }
    sibling->props_table = pt->entries;

    return sibling;
}

int32_t urbi_shape_find_slot(const UShape *s, const USymbol *name)
{
    if (name == NULL) {
        return URBI_SHAPE_SLOT_INVALID;   /* NULL name is never a slot */
    }
    for (const UShape *cur = s; cur != NULL; cur = cur->parent) {
        if (cur->name == name) {
            return (int32_t)cur->index;
        }
    }
    return URBI_SHAPE_SLOT_INVALID;
}

/* T27: rebuild a shape with `name` dropped from the lineage.
 *
 * Per pre-M2 §7.1's "private shape lineage fallback" allowance.  Walks
 * `parent` parent-ward into a fixed-depth name buffer, drops the entry
 * matching `name`, then rebuilds from the root via add_slot over the
 * surviving names (preserves declaration order of the survivors).
 *
 * Cap at URBI_SHAPE_REMOVE_DEPTH_CAP survivors (256) — deeper survivor
 * lineages return NULL so callers can surface a diagnostic.  Same
 * depth-bound discipline as urbi_object_resolve_slot's resolve stack.
 *
 * OBJ-019: the lineage walk encounters at most (CAP + 1) shapes — CAP
 * surviving names plus the one being dropped.  The total buffer is
 * sized for the survivors only, but the loop iterates over the full
 * lineage; the depth check guards the survivor count, not the iteration
 * count. */
#define URBI_SHAPE_REMOVE_DEPTH_CAP  256U

UShape *urbi_shape_transition_remove_slot(struct UVM *vm, UShape *parent,
                                          const USymbol *name)
{
    if (vm == NULL || parent == NULL || name == NULL) {
        return NULL;
    }
    if (parent->count == 0U) {
        return NULL;            /* nothing to drop */
    }

    /* Walk parent-ward, recording the (name) at each shape hop.  Order:
     * names[0] is the leaf (last-added), names[depth-1] is the first-added.
     * Per OBJ-019: the depth check bounds the SURVIVOR count, allowing
     * the dropped name to be encountered after CAP survivors have been
     * recorded — so a lineage of (CAP + 1) names where the dropped name
     * is anywhere in the chain still succeeds. */
    USymbol *names[URBI_SHAPE_REMOVE_DEPTH_CAP];
    uint32_t depth = 0U;
    int found = 0;
    for (UShape *cur = parent; cur != NULL && cur->name != NULL;
         cur = cur->parent) {
        if (cur->name == name) {
            found = 1;
            /* Don't record the dropped name; depth not bumped. */
        } else {
            if (depth >= URBI_SHAPE_REMOVE_DEPTH_CAP) {
                return NULL;    /* survivor cap exceeded */
            }
            names[depth++] = cur->name;
        }
    }
    if (!found) {
        return NULL;            /* `name` not in lineage */
    }

    /* Rebuild from the root in original add-order: names[depth-1] was added
     * first, names[0] was added last (excluding the dropped one). */
    UShape *root = urbi_shape_root(vm);
    if (root == NULL) {
        return NULL;
    }
    UShape *cur = root;
    for (uint32_t i = depth; i > 0U; i--) {
        cur = urbi_shape_transition_add_slot(vm, cur, names[i - 1U]);
        if (cur == NULL) {
            return NULL;
        }
    }
    return cur;
}
