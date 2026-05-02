/* SPDX-License-Identifier: BSD-3-Clause */
/* ushape.h — UShape (hidden class) + UShapeMap (transition cache) + UProps.
 *
 * Spec references:
 *   docs/superpowers/specs/2026-04-24-urbi-pre-m2-object-model-design.md §3, §7.1, §7.2
 *   docs/superpowers/specs/2026-04-29-urbi-pre-m4-uslot-uprops-collapse-design.md §4.1, §4.2, §5.1, §5.2
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
 * M5 reactive may extend with changed/accessed/removed; M6 stdlib pins the
 * full catalog. */
struct UProps {
    UCell        cell;              /* 2 B GC header */
    /* 6 B compiler-inserted padding before oget */
    UValue       oget;              /* UVAL_VOID when unset */
    UValue       oset;              /* UVAL_VOID when unset */
    uint32_t     constant : 1;
    uint32_t     _spare   : 31;
};

/* === UShape ===
 *
 * Hidden class per pre-M2 §3 + pre-M4 USlot/UProps collapse §4.1.
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
_Static_assert(sizeof(struct UShape) == 56,
               "UShape header must be 56 bytes per pre-M4 USlot/UProps spec §4.1");
_Static_assert(sizeof(struct UProps) == 48,
               "UProps must be 48 bytes per pre-M4 USlot/UProps spec §4.2");
#endif

/* === API === */

/* Lazy-allocates the per-VM root-shape singleton on first call.  Returns
 * the existing pointer thereafter.  Returns NULL on OOM. */
UShape *urbi_shape_root(struct UVM *vm);

/* Look up or allocate the child shape for adding `name` to `parent`.  Per
 * pre-M2 §7.1.  On first transition out of `parent`, allocates the
 * transitions cache (cap=8).  Subsequent calls hit the cache and return
 * the cached child unchanged (deterministic shape sharing).  Returns NULL
 * on OOM. */
UShape *urbi_shape_transition_add_slot(struct UVM *vm, UShape *parent,
                                       USymbol *name);

/* Find the slot index for `name` in `s`'s lineage.
 * Returns >= 0 on hit (slot index in UObject.slots[]), -1 on miss.
 *
 * Walks parent-ward over `s`'s lineage; first ancestor whose `name` field
 * matches by USymbol pointer identity is the slot, and its `index` is
 * returned. */
int32_t urbi_shape_find_slot(const UShape *s, const USymbol *name);

/* Look up the sibling shape for installing/removing a property on the slot
 * at `slot_index` in `parent`.  Per pre-M2 §7.2 + pre-M4 USlot/UProps
 * collapse spec §5.1, §5.2.
 * Stub at this task; sibling-shape materialisation lands later. */
UShape *urbi_shape_transition_property(struct UVM *vm, UShape *parent,
                                       uint32_t slot_index,
                                       uint8_t flag_bit, int install);

#ifdef __cplusplus
}
#endif

#endif /* USHAPE_H */
