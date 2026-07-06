/* SPDX-License-Identifier: BSD-3-Clause */
/* uic.h — UIC inline-cache record + URBI_IC_ENTRIES_PER_SITE tunable.
 *
 * Design references: getslot/setslot encoding §4.1/§4.3.
 *
 * One UIC is reserved per GETSLOT/SETSLOT bytecode site (allocated alongside
 * the function's IC table per §4.1).  Each entry caches a (recv_shape,
 * recv_protos, topology_gen) -> (slot, uprops, flags) mapping; lookups
 * linear-scan the first `n` entries (n <= URBI_IC_ENTRIES_PER_SITE) and miss
 * falls through to the megamorphic GET/SET fallback path.
 * `replace_cursor` advances round-robin on miss-with-full-cache to give a
 * fair eviction order without an LRU bookkeeping field.
 *
 * URBI_IC_ENTRIES_PER_SITE is a compile-time tunable bound to {1, 2, 4}.
 * Default is 4 (host build); the footprint preset binds 2 (later task);
 * 1 is the monomorphic-only configuration. */

#ifndef UIC_H
#define UIC_H

#include <stdint.h>

#include "object/uobject.h"   /* USlot typedef + URBI_SLOT_FLAG_* bits + USymbol fwd */
#include "object/ushape.h"    /* UShape, UProps */

#ifndef URBI_IC_ENTRIES_PER_SITE
#define URBI_IC_ENTRIES_PER_SITE 4
#endif

URBI_STATIC_ASSERT(URBI_IC_ENTRIES_PER_SITE == 1
            || URBI_IC_ENTRIES_PER_SITE == 2
            || URBI_IC_ENTRIES_PER_SITE == 4,
    "URBI_IC_ENTRIES_PER_SITE must be 1, 2, or 4");

typedef struct UIC {
    USymbol  *name;                                       /* slot name (interned) */
    UShape   *recv_shapes[URBI_IC_ENTRIES_PER_SITE];      /* receiver shape key */
    uintptr_t recv_protos[URBI_IC_ENTRIES_PER_SITE];      /* receiver protos word at fill
                                                             (T8b polymorphic-site key).
                                                             Shape alone CANNOT discriminate
                                                             the receiver's class — fresh
                                                             instances of slot-less classes
                                                             all share the root shape (protos
                                                             are not part of the shape), so a
                                                             same-shape receiver of a DIFFERENT
                                                             class wrong-hit the cached
                                                             inherited slots[k]/uprops[k].
                                                             Compared as an opaque word, never
                                                             dereferenced.  Same-shape + same-
                                                             protos different-identity receivers
                                                             sharing one entry is CORRECT:
                                                             identical local layout + identical
                                                             proto list imply identical
                                                             resolution. */
    uint64_t  topology_gen[URBI_IC_ENTRIES_PER_SITE];     /* per-VM topology stamp at fill */
    USlot    *slots[URBI_IC_ENTRIES_PER_SITE];            /* cached slot pointer
                                                             (valid only when
                                                             FLAG_LOCAL is clear;
                                                             see slot_idx below) */
    UProps   *uprops[URBI_IC_ENTRIES_PER_SITE];           /* cached UProps* (NULL when none) */
    uint16_t  slot_idx[URBI_IC_ENTRIES_PER_SITE];         /* slot index in recv->slots[];
                                                             used ONLY when FLAG_LOCAL is
                                                             set — the cached `slots[k]`
                                                             pointer above is recv-specific
                                                             for local slots and would
                                                             return the wrong instance's
                                                             value on polymorphic same-
                                                             shape receivers (OBJ-IC-POLY).
                                                             Fast path re-resolves via
                                                             &recv->slots[slot_idx[k]]
                                                             when the LOCAL bit is set;
                                                             non-LOCAL slots live on a
                                                             stable proto so the absolute
                                                             pointer is still correct. */
    uint8_t   flags[URBI_IC_ENTRIES_PER_SITE];            /* URBI_SLOT_FLAG_* summary */
    uint8_t   n;                                          /* live-entry count, 0..URBI_IC_ENTRIES_PER_SITE */
    uint8_t   replace_cursor;                             /* wrap-around eviction cursor */
} UIC;

/* Layout pin.  At default 4-entry / 64-bit-pointer build the natural layout
 * grew to 152 bytes when OBJ-IC-POLY added uint16_t slot_idx[N] (8 bytes at
 * N=4, padded to 8-byte alignment), then to 184 when T8b added the
 * uintptr_t recv_protos[N] polymorphic-site key (32 bytes at N=4).
 * Empirical sizeof on gcc x86_64 confirms 184.  Cross-target builds
 * (32-bit pointers) shrink the pointer arrays and skip this assert. */
#if URBI_IC_ENTRIES_PER_SITE == 4 \
        && defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
URBI_STATIC_ASSERT(sizeof(struct UIC) == 184,
    "UIC must be 184 B at default 4-entry, 64-bit pointers");
#endif

/* === Slow-path helpers ===
 *
 * Per GETSLOT/SETSLOT spec §6.3.  Called from the OP_GETSLOT /
 * OP_SETSLOT dispatch arms when the inline-cache fast path misses
 * (no shape+topology match).  Each helper resolves the slot via
 * urbi_object_resolve_slot, fills exactly one IC entry at
 * ic->replace_cursor, and returns 0/−1.  After fill the caller inspects
 * ic->flags[(replace_cursor − 1) mod cap] to dispatch a getter/setter
 * via URBI_VM_DISPATCH_GETTER / URBI_VM_DISPATCH_SETTER as needed; the
 * helpers themselves never call into the VM dispatch loop. */

struct UVM;
/* UObject typedef is already in scope via "object/uobject.h" above. */

int urbi_slot_get_slow(struct UVM *vm, UObject *recv, UIC *ic,
                       UValue *out_value);
int urbi_slot_set_slow(struct UVM *vm, UObject *recv, UIC *ic,
                       UValue value);

#endif /* UIC_H */
