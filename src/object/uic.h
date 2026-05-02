/* SPDX-License-Identifier: BSD-3-Clause */
/* uic.h — UIC inline-cache record + URBI_IC_ENTRIES_PER_SITE tunable.
 *
 * Spec references:
 *   docs/superpowers/specs/2026-04-29-urbi-pre-m4-getslot-setslot-encoding-design.md §4.1, §4.3
 *
 * One UIC is reserved per GETSLOT/SETSLOT bytecode site (allocated alongside
 * the function's IC table per pre-M4 §4.1).  Each entry caches a (recv_shape,
 * topology_gen) -> (slot, uprops, flags) mapping; lookups linear-scan the
 * first `n` entries (n <= URBI_IC_ENTRIES_PER_SITE) and miss falls through
 * to the megamorphic GET/SET fallback path (T40).  `replace_cursor` advances
 * round-robin on miss-with-full-cache to give a fair eviction order without
 * an LRU bookkeeping field.
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

_Static_assert(URBI_IC_ENTRIES_PER_SITE == 1
            || URBI_IC_ENTRIES_PER_SITE == 2
            || URBI_IC_ENTRIES_PER_SITE == 4,
    "URBI_IC_ENTRIES_PER_SITE must be 1, 2, or 4");

typedef struct UIC {
    USymbol  *name;                                       /* slot name (interned) */
    UShape   *recv_shapes[URBI_IC_ENTRIES_PER_SITE];      /* receiver shape key */
    uint64_t  topology_gen[URBI_IC_ENTRIES_PER_SITE];     /* per-VM topology stamp at fill */
    USlot    *slots[URBI_IC_ENTRIES_PER_SITE];            /* cached slot pointer */
    UProps   *uprops[URBI_IC_ENTRIES_PER_SITE];           /* cached UProps* (NULL when none) */
    uint8_t   flags[URBI_IC_ENTRIES_PER_SITE];            /* URBI_SLOT_FLAG_* summary */
    uint8_t   n;                                          /* live-entry count, 0..URBI_IC_ENTRIES_PER_SITE */
    uint8_t   replace_cursor;                             /* wrap-around eviction cursor */
} UIC;

/* Layout pin.  At default 4-entry / 64-bit-pointer build the natural layout
 * is 142 bytes of payload padded to 144 (max alignment 8 from uint64_t /
 * pointer fields).  The plan-task body's worked-example arithmetic came out
 * to 152 instead — that was off by one round-up step; empirical sizeof on
 * gcc / clang x86_64 + aarch64 confirms 144.  Cross-target builds (32-bit
 * pointers) shrink the pointer arrays and skip this assert. */
#if URBI_IC_ENTRIES_PER_SITE == 4 \
        && defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(struct UIC) == 144,
    "UIC must be 144 B at default 4-entry, 64-bit pointers");
#endif

#endif /* UIC_H */
