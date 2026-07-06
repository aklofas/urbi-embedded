/* SPDX-License-Identifier: BSD-3-Clause */
/* ushape.h — UShape (hidden class) + UShapeMap (transition cache) + UProps.
 *
 * Design references: object-model §3/§7.1/§7.2; uslot/uprops
 * collapse §4.1/§4.2/§5.1/§5.2.
 *
 * Layout invariants pinned by tests/unit/test_ushape.c.  UShape header is
 * 56 bytes on 64-bit hosts (UCell + 6 B compiler-inserted pad + 7 payload
 * fields + 1 explicit `_pad` field totalling 48 B).
 *
 * UProps holds slot-property metadata (oget / oset / constant) and is
 * allocated lazily — the per-shape props_table side-table is NULL by
 * default per the USlot/UProps collapse spec §4.2; it materialises only
 * when a slot in this shape's lineage has a property installed.
 *
 * Transition primitives are stubs at this task; later tasks land the
 * transition-cache lookup + sibling-shape materialisation. */

#ifndef USHAPE_H
#define USHAPE_H

#include <stdint.h>

#include "object/uobject.h"   /* USlot, USymbol forward decl, UObject */
#include "gc/ugc.h"           /* UCell */

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;

/* === UShapeMap ===
 *
 * Transition cache: a small open-addressing table mapping the added
 * USymbol* (interned, pointer-identity) to the child UShape that adds
 * that slot.  One UShapeMap per parent UShape, allocated lazily on the
 * first transition out of that parent.
 *
 * Probe scheme: linear probing on the low bits of (key >> 4) — symbols
 * are interned, so pointer identity is the only valid comparison and
 * the shift strips alignment-zero bits.
 *
 * cap is a power of two; resize to 2x at >= 75% load.
 *
 * Walker (utypes_init.c walk_ushapemap) shades each non-NULL entry's
 * value (UShape*).  Keys are USymbol* — interned, never collected per
 * the intern-table contract — so the walker doesn't shade keys. */
typedef struct UShapeMap {
    UCell        cell;          /* 2 B GC header (type_tag = UTYPE_SHAPE_MAP) */
    /* 2 B compiler-inserted padding before cap */
    uint32_t     cap;           /* power of two */
    uint32_t     count;         /* live entries */
    uint32_t     _pad;          /* explicit pad to 8 B align entries[] */
    struct {
        USymbol *k;             /* interned-symbol pointer; NULL marks empty */
        UShape  *v;             /* child shape adding k */
    } entries[];                /* flexible array; length == cap */
} UShapeMap;

/* === UProps ===
 *
 * Per-slot property record.  Allocated only when a slot has at least one
 * property installed; vector of UProps* lives in UShape.props_table.
 *
 * Reactive watchers may extend with changed/accessed/removed; stdlib pins the
 * full catalog. */
struct UProps {
    UCell        cell;              /* 2 B GC header */
    /* 6 B compiler-inserted padding before oget */
    UValue       oget;              /* UVAL_VOID when unset */
    UValue       oset;              /* UVAL_VOID when unset */
    uint32_t     constant : 1;
    uint32_t     _spare   : 31;
};

/* === UPropsTable ===
 *
 * Wrapper GC cell holding a UShape's per-slot UProps* array.  Allocated
 * lazily by urbi_shape_transition_property when the first property
 * is installed on a shape's lineage.
 *
 * UShape.props_table points at the entries[] flexible array; the wrapper
 * cell's reachability is provided by walk_ushape, which recovers the cell
 * base from props_table via offsetof(UPropsTable, entries) and shades it.
 *
 * Field order is load-bearing (UCell first member; explicit pad to 8 B
 * before entries[] so each UProps* element is naturally aligned). */
typedef struct UPropsTable {
    UCell        cell;              /* type_tag = UTYPE_PROPS_TABLE */
    /* 2 B compiler-inserted padding before n */
    uint32_t     n;                 /* entry count (== owning UShape.count) */
    uint32_t     _pad;              /* explicit pad to 8 B align entries[] */
    UProps      *entries[];         /* flexible array; length == n */
} UPropsTable;

/* === UShape ===
 *
 * Hidden class per §3 + USlot/UProps collapse §4.1.
 *
 * Field order is load-bearing: layout pinned by test_ushape.c.
 *
 *   cell         2 B   GC header (type_tag = UTYPE_SHAPE)
 *   <pad>        6 B   compiler-inserted alignment to next pointer
 *   name         8 B   USymbol* — last-added slot name (NULL for root)
 *   index        4 B   slot offset in UObject.slots[] (undefined for root)
 *   count        4 B   slot count at this shape
 *   flags        4 B   OGET/OSET/CONSTANT/LOCAL bits per slot;
 *                      packed across slots (4 bits/slot in v1.0)
 *   _pad         4 B   explicit pad to 8-byte align next pointer
 *   parent       8 B   shape without this slot
 *   transitions  8 B   name -> child shape cache; NULL until first transition
 *   props_table  8 B   dense per-slot UProps* array (length == count when set);
 *                      NULL until any slot in this lineage has a property.
 *   ----------------
 *   total       56 B
 */
struct UShape {
    UCell        cell;
    /* 6 B compiler-inserted padding */
    USymbol     *name;
    uint32_t     index;
    uint32_t     count;
    uint32_t     flags;
    uint32_t     _pad;
    UShape      *parent;
    UShapeMap   *transitions;
    UProps     **props_table;
};
/* The 56 / 48 byte invariants below assume 64-bit pointers (the supported
 * host ABI).  On 32-bit cross targets the pointer fields and pre-pointer
 * padding shrink, so the literal byte totals no longer hold.  Gate on
 * pointer width; runtime offset checks in tests/unit/test_ushape.c are
 * host-only and supply the second signal there. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
URBI_STATIC_ASSERT(sizeof(struct UShape) == 56,
               "UShape header must be 56 bytes per USlot/UProps spec §4.1");
URBI_STATIC_ASSERT(sizeof(struct UProps) == 48,
               "UProps must be 48 bytes per USlot/UProps spec §4.2");
#endif

/* === API === */

/* Lazy-allocates the per-VM root-shape singleton on first call.  Returns
 * the existing pointer thereafter.  Returns NULL on OOM. */
UShape *urbi_shape_root(struct UVM *vm);

/* Look up or allocate the child shape for adding `name` to `parent`.  Per
 * §7.1.  On first transition out of `parent`, allocates the
 * transitions cache (cap=8).  Subsequent calls hit the cache and return
 * the cached child unchanged (deterministic shape sharing).  Returns NULL
 * on OOM. */
UShape *urbi_shape_transition_add_slot(struct UVM *vm, UShape *parent,
                                       USymbol *name);

/* Sentinel returned by urbi_shape_find_slot when `name` is absent from
 * the lineage.  Replaces the magic-0 that several pre-Wave-6 callers
 * used to mean "not found" (OBJ-017 — slot index 0 is a valid slot, not
 * a miss).  -1 chosen so the result fits in int32_t and never collides
 * with any valid slot index. */
#define URBI_SHAPE_SLOT_INVALID  ((int32_t)-1)

/* Find the slot index for `name` in `s`'s lineage.
 * Returns >= 0 on hit (slot index in UObject.slots[]),
 * URBI_SHAPE_SLOT_INVALID on miss.
 *
 * Walks parent-ward over `s`'s lineage; first ancestor whose `name` field
 * matches by USymbol pointer identity is the slot, and its `index` is
 * returned. */
int32_t urbi_shape_find_slot(const UShape *s, const USymbol *name);

/* Materialise the sibling shape for installing or removing a property
 * flag bit on the slot at `slot_index` in `parent`.  Per §7.2 +
 * USlot/UProps collapse spec §5.1, §5.2.
 *
 * Behaviour:
 *  - If `parent->count == 0` or `slot_index >= parent->count`: returns NULL.
 *  - If the requested flag bit is already in the desired state: returns
 *    `parent` unchanged (idempotent no-op; no allocation).
 *  - Otherwise allocates a fresh sibling UShape sharing parent->parent
 *    (NOT parent itself) with the new flag-nibble at slot_index, and a
 *    fresh UPropsTable wrapper cell whose entries are seeded from
 *    parent->props_table (copy-on-write).  Caller writes the per-slot
 *    UProps* into sibling->props_table[slot_index] post-transition.
 *
 * Returns the (cached or fresh) sibling shape, or NULL on OOM. */
UShape *urbi_shape_transition_property(struct UVM *vm, UShape *parent,
                                       uint32_t slot_index,
                                       uint8_t flag_bit, int install);

/* Build a child shape that drops `name` from `parent`'s lineage.
 *
 * Strategy (private shape lineage fallback per §7.1): walk parent
 * parent-ward into a fixed-depth name buffer, drop the entry matching
 * `name`, then rebuild from the root via urbi_shape_transition_add_slot
 * over the surviving names in original order.  The resulting child shape
 * is private (not cached in any transitions table beyond the per-name
 * caches that add_slot already populates).
 *
 * Returns NULL if `name` is not in `parent`'s lineage, or on OOM.  Depth
 * is capped at 256 names; deeper lineages return NULL (matches the
 * 64-deep stack precedent in resolve_slot, doubled here because shape
 * lineages can be longer than prototype graph depth). */
UShape *urbi_shape_transition_remove_slot(struct UVM *vm, UShape *parent,
                                          const USymbol *name);

#ifdef __cplusplus
}
#endif

#endif /* USHAPE_H */
