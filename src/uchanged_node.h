/* SPDX-License-Identifier: BSD-3-Clause */
/* UChangedNode: per-slot slot-change subscriber cell.
 * Spec #4 §3.1.
 *
 * One UChangedNode is allocated (lazily, on first `obj.x.changed?` install)
 * per (object, slot-name) pair.  It lives on UObject.changed_events_head as
 * a singly-linked intrusive list.
 *
 * Layout on a 64-bit host (8-byte pointer alignment):
 *   cell   : type_tag(1) + gc_byte(1) + pad0(6) = 8 B
 *   name   : 8 B  (USymbol*)
 *   event  : 8 B  (UEvent*)
 *   next   : 8 B  (UChangedNode*)
 *   Total  : 32 B
 *
 * On 32-bit cross targets (Cortex-M7, rv32imc) pointer fields shrink to 4 B
 * and the total is 16 B.  The _Static_assert in this header gates the exact
 * 32-byte check to __SIZEOF_POINTER__ == 8 builds.
 *
 * type_tag = UTYPE_CHANGED_NODE; gc_byte = 0 at alloc (set by urbi_gc_alloc).
 * name points to an interned USymbol (intern table keeps it alive — not walked
 * by the GC walker).  event and next are GC-managed and walked. */

#ifndef UCHANGED_NODE_H
#define UCHANGED_NODE_H

#include <stdint.h>

#include "gc/ugc.h"    /* UCell, UTYPE_CHANGED_NODE */
#include "uevent.h"    /* UEvent forward-compatible include */

#ifdef __cplusplus
extern "C" {
#endif

/* === Forward declarations === */

struct UVM;
struct USymbol;

/* === UChangedNode struct (spec #4 §3.1) === */

typedef struct UChangedNode {
    /* --- common cell header (matches UCell at offsets 0..1) --- */
    uint8_t  type_tag;                   /* UTYPE_CHANGED_NODE */
    uint8_t  gc_byte;                    /* GC color bits; set by urbi_gc_alloc */
    uint8_t  pad0[6];                    /* explicit pad to 8 B before first ptr */

    /* --- slot name (interned; not GC-walked per intern-table contract) --- */
    struct USymbol      *name;

    /* --- per-slot changed event (GC-walked) --- */
    UEvent              *event;

    /* --- intrusive list link; NULL = end of chain --- */
    struct UChangedNode *next;
} UChangedNode;

/* Size assertion: 32 B on 64-bit host (8B header + 8B + 8B + 8B = 32B).
 * On 32-bit cross targets the total is 16 B (4B header + 4B + 4B + 4B).
 * Guarded on pointer width to avoid a hard failure on 32-bit cross targets. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(UChangedNode) == 32,
               "UChangedNode must be 32 bytes on 64-bit (spec #4 §3.1)");
#endif

#ifdef __cplusplus
}
#endif

#endif /* UCHANGED_NODE_H */
