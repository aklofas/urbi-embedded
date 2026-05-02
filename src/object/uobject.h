/* SPDX-License-Identifier: BSD-3-Clause */
/* uobject.h — UObject / UProtos / USlot internal layout.
 *
 * Public API in include/urbi/object.h (lands at later M4 task).  This header
 * is freestanding and may be included by any internal .c file that touches
 * object slots, prototypes, or hidden classes.
 *
 * Spec references:
 *   docs/superpowers/specs/2026-04-29-urbi-pre-m4-prototype-chain-representation-design.md §3, §4
 *   docs/superpowers/specs/2026-04-29-urbi-pre-m4-uslot-uprops-collapse-design.md §3
 *
 * Reconciliation note (T0 R-1): the spec's §3 shorthand `UGCHeader gc_hdr; // 8B`
 * collapses, in this codebase, to the existing 2-byte UCell embedded as the
 * first member with 6 bytes of compiler-inserted natural alignment padding
 * before the next pointer field.  Net layout still matches spec §3 (48 B);
 * the per-VM UAllCellsNode sidecar (src/gc/ugc_incremental.c) supplies the
 * "alloc-link" the spec mentions — NOT inline. */

#ifndef UOBJECT_H
#define UOBJECT_H

#include <stdint.h>

#include "umodule.h"   /* UValue (16 B) + USymbol forward-typedef */
#include "gc/ugc.h"    /* UCell (2 B) */

/* USymbol is forward-declared in umodule.h.  Real definition lives in
 * uintern.h once the intern layer migrates to UString GC cells (later M4
 * task).  Consumers of this header that need the full struct must include
 * uintern.h explicitly. */

/* === USlot ===
 *
 * USlot collapses to exactly one UValue (16 B) per the pre-M4 USlot/UProps
 * spec §3.  Inlined into UObject as `USlot slots[shape->count]`. */
typedef UValue USlot;
_Static_assert(sizeof(USlot) == sizeof(UValue),
               "USlot must equal UValue width");
_Static_assert(sizeof(USlot) == 16,
               "USlot must be 16 bytes per pre-M4 USlot/UProps spec §3");

/* === IC + UProps slot-property flag bits ===
 *
 * Per pre-M4 GETSLOT/SETSLOT spec §6.5.  These flags populate UIC.flags
 * (inline-cache attribute summary) and the per-slot 4-bit nibbles packed
 * into UShape.flags (v1.0 cap of 8 slots in the packed form; T15 spills
 * to a side allocation when a UShape's slot count exceeds 8). */
#define URBI_SLOT_FLAG_OGET      (1u << 0)   /* slot has a getter installed */
#define URBI_SLOT_FLAG_OSET      (1u << 1)   /* slot has a setter installed */
#define URBI_SLOT_FLAG_CONSTANT  (1u << 2)   /* slot value is constant */
#define URBI_SLOT_FLAG_LOCAL     (1u << 3)   /* slot is on the receiver, not a prototype */
/* bits 4-7 reserved for v1.x */

/* === UObject.flags layout ===
 *
 * uint32_t bitfield per pre-M4 prototype-chain spec §3.  Low 4 bits encode
 * the atom family (root Object, the eight built-in atoms, plus 9..15 spare);
 * bit 4 is frozen; bit 5 is sandbox-readonly (per Luau prior art); the high
 * bits are spare. */
typedef enum {
    URBI_ATOM_OBJECT  = 0,   /* root Object */
    URBI_ATOM_INTEGER = 1,
    URBI_ATOM_FLOAT   = 2,
    URBI_ATOM_STRING  = 3,
    URBI_ATOM_LIST    = 4,
    URBI_ATOM_DICT    = 5,
    URBI_ATOM_TAG     = 6,
    URBI_ATOM_EVENT   = 7,
    URBI_ATOM_SYMBOL  = 8
    /* 9..15 reserved */
} URBIAtomFamily;
#define URBI_OBJ_ATOM_MASK         0x0Fu
#define URBI_OBJ_FLAG_FROZEN       (1u << 4)
#define URBI_OBJ_FLAG_SANDBOX_RO   (1u << 5)   /* per Luau prior art */
/* bits 6..31 spare */

/* === forward decls (real definitions land at later M4 tasks) ===
 * UShape + UObject are also typedef'd in include/urbi/object.h (the public
 * mirror); guard against C99-pedantic typedef redeclaration when both
 * headers are pulled in by a single TU.  UProps is internal-only. */
#ifndef URBI_OBJECT_TYPEDEF_DEFINED
#define URBI_OBJECT_TYPEDEF_DEFINED
typedef struct UShape   UShape;
typedef struct UObject  UObject;
#endif
typedef struct UProps   UProps;

/* === UProtos ===
 *
 * Heap form for n >= 2 prototypes per spec §4.2.  n == 0 (no protos) and
 * n == 1 (single proto) use the tagged-pointer encoding stashed directly
 * in UObject.protos and never allocate a UProtos block.  items[0] is the
 * highest-priority prototype (MRO position 0). */
typedef struct UProtos {
    UCell             cell;          /* 2 B GC header (M3 sidecar pattern) */
    /* 6 B compiler-inserted padding before n */
    uint32_t          n;             /* prototype count; n >= 2 always */
    uint32_t          _pad;          /* explicit pad to 8 B align items[] */
    UObject          *items[];       /* flexible array of proto pointers */
} UProtos;

/* === UObject ===
 *
 * 48 B header per pre-M4 prototype-chain spec §3.  Field order is
 * load-bearing: pinned by tests/unit/test_uobject.c offset checks.  All
 * fields are populated by urbi_object_alloc (lands at later M4 task). */
struct UObject {
    UCell             cell;          /* 2 B — GC color + type tag (UCELL_TYPE_OBJECT later) */
    /* 6 B compiler-inserted padding before shape* */
    UShape           *shape;         /* 8 B — hidden class */
    USlot            *slots;         /* 8 B — local slot storage, length == shape->count */
    uintptr_t         protos;        /* 8 B — tagged single-or-heap proto encoding (§4.1) */
    uint32_t          object_id;     /* 4 B — stable identity (§7) */
    uint32_t          lookup_stamp;  /* 4 B — visited-set marker for prototype walk (§6); u32 truncation of UVM.lookup_id */
    uint32_t          flags;         /* 4 B — atom family + frozen + readonly + spare */
    uint32_t          reserved;      /* 4 B — zero at v1.0; named v1.x candidates (§8.2) */
};
/* The 48-byte invariant assumes 64-bit pointers (the supported host ABI).
 * On 32-bit cross targets (e.g. Cortex-M7, rv32), the pointer fields shrink
 * and natural alignment changes, so the literal byte total no longer holds.
 * Gate the assert on pointer width; runtime offset checks in
 * tests/unit/test_uobject.c are host-only and supply the second signal there. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(struct UObject) == 48,
               "UObject header must be 48 bytes per pre-M4 prototype-chain spec §3");
#endif

/* === Internal allocator (T8) ===
 *
 * Allocate a fresh UObject in the named atom family.  Wires shape to the
 * per-VM root hidden class, protos to the empty form (0), object_id to
 * the next per-VM monotonic id, and flags to (family & ATOM_MASK).
 * Returns NULL on OOM. */
struct UVM;
UObject *urbi_object_alloc(struct UVM *vm, URBIAtomFamily family);

/* === Atom-family debug name (T8) ===
 *
 * Stable static string per atom family.  Used by error messages (T11
 * valid_proto failure path and beyond). */
const char *urbi_atom_family_name(URBIAtomFamily f);

/* === UPROTOS_FOREACH (T9 — per pre-M4 prototype-chain spec §6.1) ===
 *
 * UObject.protos is a uintptr_t with three storage forms (spec §4.1):
 *   - empty:  obj->protos == 0
 *   - single: obj->protos == ((p << 1) | 1u)   — bit 0 set, address in high bits
 *   - heap:   obj->protos == (uintptr_t)up     — bit 0 clear, raw UProtos*
 *
 * UPROTOS_FOREACH dispatches across all three forms and captures
 * obj->protos ONCE at iteration start, so iteration-during-mutation
 * sees a stable snapshot (legacy semantics per spec §6.3).
 *
 * Usage:
 *   UObject *p;
 *   UPROTOS_FOREACH(obj, p) { ... visit p ... }
 *
 * Identifier-naming note: the for-loop scope already isolates __upf_ctx_local,
 * so a fixed name is sufficient — no __LINE__ token-paste gymnastics required. */
struct __upf_ctx {
    uintptr_t  raw;       /* copy of obj->protos at iteration start */
    UProtos   *up;        /* non-NULL only in heap case */
    uint32_t   i;         /* 0..up->n in heap; 0/1 in single; unused in empty */
};

static inline struct __upf_ctx __upf_init(const UObject *obj) {
    struct __upf_ctx c;
    c.raw = obj->protos;
    c.up  = NULL;
    c.i   = 0u;
    if (c.raw != 0u && (c.raw & 1u) == 0u) {
        c.up = (UProtos *)c.raw;
    }
    return c;
}

static inline int __upf_next(struct __upf_ctx *c, UObject **out) {
    if (c->up != NULL) {
        if (c->i >= c->up->n) return 0;
        *out = c->up->items[c->i++];
        return 1;
    }
    if ((c->raw & 1u) != 0u) {
        if (c->i != 0u) return 0;
        *out = (UObject *)(c->raw >> 1);
        c->i = 1u;
        return 1;
    }
    return 0;
}

#define UPROTOS_FOREACH(obj, p_var)                                     \
    for (struct __upf_ctx __upf_ctx_local = __upf_init((obj));          \
         __upf_next(&__upf_ctx_local, &(p_var));                        \
        )

/* Convenience inlines — count + indexed access across all three forms. */
static inline uint32_t urbi_object_proto_count(const UObject *obj) {
    if (obj->protos == 0u) return 0u;
    if ((obj->protos & 1u) != 0u) return 1u;
    return ((const UProtos *)obj->protos)->n;
}

static inline UObject *urbi_object_proto_at(const UObject *obj, uint32_t i) {
    if (obj->protos == 0u) return NULL;
    if ((obj->protos & 1u) != 0u) {
        return (i == 0u) ? (UObject *)(obj->protos >> 1) : NULL;
    }
    {
        UProtos *up = (UProtos *)obj->protos;
        return (i < up->n) ? up->items[i] : NULL;
    }
}

#endif /* UOBJECT_H */
