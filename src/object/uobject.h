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

#include "module/umodule.h"   /* UValue (16 B) + USymbol forward-typedef */
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
URBI_STATIC_ASSERT(sizeof(USlot) == sizeof(UValue),
               "USlot must equal UValue width");
URBI_STATIC_ASSERT(sizeof(USlot) == 16,
               "USlot must be 16 bytes per pre-M4 USlot/UProps spec §3");

/* === USlotArray ===
 *
 * Wrapper GC cell holding a UObject's grow-on-write slot storage.  Allocated
 * lazily by urbi_object_set_local_slot (T26) on the first slot transition
 * out of the empty root shape, then reallocated fresh on each subsequent
 * leaf-shape-add (M3 GC is non-relocating).
 *
 * UObject.slots points at the entries[] flexible array; the wrapper cell's
 * reachability is provided by walk_uobject, which recovers the cell base
 * from obj->slots via offsetof(USlotArray, entries) and shades it.
 *
 * Field order is load-bearing (UCell first member; explicit pad to 8 B
 * before entries[] so each USlot is naturally aligned).  Mirrors the
 * UPropsTable / UProtoInstanceArr precedent. */
typedef struct USlotArray {
    UCell        cell;              /* type_tag = UTYPE_SLOT_ARRAY */
    /* 2 B compiler-inserted padding before n */
    uint32_t     n;                 /* entry count (== owning UShape.count) */
    uint32_t     _pad;              /* explicit pad to 8 B align entries[] */
    USlot        entries[];         /* flexible array; length == n */
} USlotArray;

/* === IC + UProps slot-property flag bits ===
 *
 * Per pre-M4 GETSLOT/SETSLOT spec §6.5.  These flags populate UIC.flags
 * (inline-cache attribute summary) and the per-slot 4-bit nibbles packed
 * into UShape.flags (v1.0 cap of 8 slots in the packed form; T15 spills
 * to a side allocation when a UShape's slot count exceeds 8). */
#define URBI_SLOT_FLAG_OGET      (1U << 0)   /* slot has a getter installed */
#define URBI_SLOT_FLAG_OSET      (1U << 1)   /* slot has a setter installed */
#define URBI_SLOT_FLAG_CONSTANT  (1U << 2)   /* slot value is constant */
#define URBI_SLOT_FLAG_LOCAL     (1U << 3)   /* slot is on the receiver, not a prototype */
/* bits 4-7 reserved for v1.x */

/* === UObject.flags layout ===
 *
 * uint32_t bitfield per pre-M4 prototype-chain spec §3.  Low 4 bits encode
 * the atom family (root Object, the eight built-in atoms 1..8, plus the
 * three M6 Phase 4 additions 9..11 — Boolean / Nil / Void; 12..15 still
 * spare for v1.x); bit 4 is frozen; bit 5 is sandbox-readonly (per Luau
 * prior art); the high bits are spare.  URBIAtomFamily enum lives in
 * <urbi/object.h> as of v0.5.5; the internal duplicate (with no `_F`
 * suffix) was retired in favor of the public form. */
#include "urbi/object.h"
#define URBI_OBJ_ATOM_MASK         0x0FU
#define URBI_OBJ_FLAG_FROZEN       (1U << 4)
#define URBI_OBJ_FLAG_SANDBOX_RO   (1U << 5)   /* per Luau prior art */
#define URBI_OBJ_FLAG_IS_PROTOTYPE (1U << 6)   /* T27: set when this object is referenced as another's prototype.
                                                  Monotonic — never cleared.  Drives the conditional topology_gen
                                                  bump in urbi_object_set_local_slot per topology spec §4.1 row 4
                                                  (slot install on a prototype must invalidate IC entries that
                                                  cached lookups walking through this object). */
/* bits 7..31 spare */

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

/* === Forward declaration for UChangedNode (spec #4 §3.1) ===
 *
 * Full definition in src/uchanged_node.h.  Declared here as an incomplete
 * type so struct UObject can hold the head pointer without a circular
 * include dependency (uchanged_node.h includes uevent.h, which is not
 * needed by every consumer of uobject.h). */
struct UChangedNode;

/* === UObject ===
 *
 * 56 B header on 64-bit host after M5 spec #4 §3.1 adds changed_events_head
 * (was 48 B at M4).  Field order is load-bearing: pinned by
 * tests/unit/test_uobject.c offset checks.  All fields are populated by
 * urbi_object_alloc. */
struct UObject {
    UCell             cell;                  /* 2 B — GC color + type tag (UCELL_TYPE_OBJECT later) */
    /* 6 B compiler-inserted padding before shape* */
    UShape           *shape;                 /* 8 B — hidden class */
    USlot            *slots;                 /* 8 B — local slot storage, length == shape->count */
    uintptr_t         protos;                /* 8 B — tagged single-or-heap proto encoding (§4.1) */
    uint32_t          object_id;             /* 4 B — stable identity (§7) */
    uint32_t          lookup_stamp;          /* 4 B — visited-set marker for prototype walk (§6); u32 truncation of UVM.lookup_id */
    uint32_t          flags;                 /* 4 B — atom family + frozen + readonly + spare */
    uint32_t          reserved;              /* 4 B — zero at v1.0; named v1.x candidates (§8.2) */
    struct UChangedNode *changed_events_head; /* 8 B — slot-change subscriber chain (spec #4 §3.1); NULL at alloc */
};
/* The 56-byte invariant assumes 64-bit pointers (the supported host ABI).
 * On 32-bit cross targets (e.g. Cortex-M7, rv32), the pointer fields shrink
 * and natural alignment changes, so the literal byte total no longer holds.
 * Gate the assert on pointer width; runtime offset checks in
 * tests/unit/test_uobject.c are host-only and supply the second signal there. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
URBI_STATIC_ASSERT(sizeof(struct UObject) == 56,
               "UObject header must be 56 bytes per M5 spec #4 §3.1");
#endif

/* === Internal allocator (T8) ===
 *
 * Allocate a fresh UObject in the named atom family.  Wires shape to the
 * per-VM root hidden class, protos to the empty form (0), object_id to
 * the next per-VM monotonic id, and flags to (family & ATOM_MASK).
 * Returns NULL on OOM. */
struct UVM;
UObject *urbi_object_alloc(struct UVM *vm, URBIAtomFamily family);

/* === T39: clone a UObject (atom-aware) ===
 *
 * Per pre-M2 §4.4 + atom-clone.chk semantics.  Allocates a fresh UObject
 * in the same atom family as `parent`, then threads `parent` into the
 * clone's protos as the single-tag form (clone inherits all of parent's
 * lineage via prototype lookup).  parent.flags's IS_PROTOTYPE bit is set
 * by urbi_object_set_protos_single; subsequent slot installs on parent
 * therefore bump topology_gen per topology spec §4.1 row 4.
 *
 * Returns NULL if parent is NULL or on OOM.  Does NOT install a `new`
 * method on the result — Class.new() / Object.new() stdlib wiring lands
 * with the M5 stdlib bring-up and depends on a working runtime call site. */
UObject *urbi_object_clone(struct UVM *vm, UObject *parent);

/* === Atom-family debug name (T8) ===
 *
 * Stable static string per atom family.  Used by error messages (T11
 * valid_proto failure path and beyond). */
const char *urbi_atom_family_name(URBIAtomFamily f);

/* === urbi_atom_proto_for_value — Phase 2 atom-method dispatch helper ===
 *
 * Route a UValue to its atom proto.  Used by the OP_GETSLOT / OP_SETSLOT
 * slow path so that slot lookup starting from a non-UVAL_OBJECT receiver
 * walks through the realm-global atom proto chain.
 *
 * For UVAL_OBJECT, returns the receiver pointer (no atom routing).
 * For UVAL_INT / UVAL_FLOAT / UVAL_STR / UVAL_EVENT, returns the
 * realm-global atom proto for that family (URBI_ATOM_INTEGER / _FLOAT /
 * _STRING / _EVENT).
 * For UVAL_BOOL, UVAL_NIL, UVAL_VOID, and the closure/strand/host-fn
 * kinds, returns the root Object proto (legacy semantics: lookup
 * terminates at root Object's slot table; Phase 4 tightens Boolean
 * routing once URBI_ATOM_BOOLEAN lands).
 *
 * Tag values are wrapped as UVAL_OBJECT today (no UVAL_TAG exists in the
 * public UValKind union), so they fall through the UVAL_OBJECT arm.
 *
 * Lazy-allocates the per-VM atom singleton on first call (idempotent).
 * Returns NULL only if urbi_object_atom returns NULL (atom-singleton
 * allocation OOM). */
struct UObject *urbi_atom_proto_for_value(struct UVM *vm, UValue v);

/* === Prototype-mutation primitives (T10 — per pre-M4 prototype-chain spec §5) ===
 *
 * Every prototype-chain mutation routes through one of these three primitives.
 * Each primitive (1) fires the forward Dijkstra barrier on the existing protos
 * value before overwriting it, (2) shades the inserted child(ren) (write-pre
 * barrier on the new value), and (3) bumps vm->topology_gen (per pre-M2 §7.4 /
 * pre-M4 topology-generation spec §3.1).
 *
 * Caller invariants:
 *   - vm and obj must be non-NULL.
 *   - For _single: p must be non-NULL.
 *   - For _heap:  up must be non-NULL with up->n >= 2 (single-proto chains
 *                 use _single; empty chains use _empty).  The UProtos block
 *                 must already be allocated via urbi_gc_alloc (UTYPE_PROTOS)
 *                 with up->items[] populated; this primitive only shades and
 *                 publishes the pointer.
 *
 * Cycle detection / dedup / valid_proto checks are the caller's responsibility
 * (T11 wires those at the higher-level urbi_object_add_proto / set_protos
 * surfaces).  These primitives are the storage-form transition layer only. */
void urbi_object_set_protos_empty (struct UVM *vm, UObject *obj);
void urbi_object_set_protos_single(struct UVM *vm, UObject *obj, UObject *p);
void urbi_object_set_protos_heap  (struct UVM *vm, UObject *obj, UProtos *up);

/* === UPROTOS_FOREACH (T9 — per pre-M4 prototype-chain spec §6.1) ===
 *
 * UObject.protos is a uintptr_t with three storage forms (spec §4.1):
 *   - empty:  obj->protos == 0
 *   - single: obj->protos == ((uintptr_t)p | 1U)   — bit 0 set, raw pointer in remaining bits (alignment makes bit 0 free)
 *   - heap:   obj->protos == (uintptr_t)up         — bit 0 clear, raw UProtos*
 *
 * UPROTOS_FOREACH dispatches across all three forms and captures
 * obj->protos ONCE at iteration start, so iteration-during-mutation
 * sees a stable snapshot (legacy semantics per spec §6.3).
 *
 * Usage:
 *   UObject *p;
 *   UPROTOS_FOREACH(obj, p) { ... visit p ... }
 *
 * Identifier-naming note: the for-loop scope already isolates upf_ctx_local,
 * so a fixed name is sufficient — no __LINE__ token-paste gymnastics required. */
struct upf_ctx {
    uintptr_t  raw;       /* copy of obj->protos at iteration start */
    UProtos   *up;        /* non-NULL only in heap case */
    uint32_t   i;         /* 0..up->n in heap; 0/1 in single; unused in empty */
};

static inline struct upf_ctx upf_init(const UObject *obj) {
    struct upf_ctx c;
    c.raw = obj->protos;
    c.up  = NULL;
    c.i   = 0U;
    if (c.raw != 0U && (c.raw & 1U) == 0U) {
        /* Heap form: raw is a UProtos* stored as uintptr_t; bit 0 clear.
         * Per pre-M4 prototype-chain spec §7.2 the high-bit encoding is a
         * load-bearing design pin, so the int-to-pointer round-trip is
         * intentional. */
        c.up = (UProtos *)c.raw;  /* NOLINT(performance-no-int-to-ptr) — UProtos pointer-encoding (TIDY-003 design pin) */
    }
    return c;
}

static inline int upf_next(struct upf_ctx *c, UObject **out) {
    if (c->up != NULL) {
        if (c->i >= c->up->n) return 0;
        *out = c->up->items[c->i++];
        return 1;
    }
    if ((c->raw & 1U) != 0U) {
        if (c->i != 0U) return 0;
        /* Single form: bit 0 set, raw UObject* in remaining bits
         * (alignment makes bit 0 free); see urbi_object_proto_at. */
        *out = (UObject *)(c->raw & ~(uintptr_t)1U);  /* NOLINT(performance-no-int-to-ptr) — UProtos single-form pointer-encoding */
        c->i = 1U;
        return 1;
    }
    return 0;
}

#define UPROTOS_FOREACH(obj, p_var)                                     \
    for (struct upf_ctx upf_ctx_local = upf_init((obj));                \
         upf_next(&upf_ctx_local, &(p_var));                            \
        )

/* === T12: cycle-safe DFS lookup primitive ===
 *
 * Per pre-M4 prototype-chain spec §6 + GETSLOT/SETSLOT spec §6.5.
 *
 * urbi_object_lookup performs a left-first depth-first walk of obj's
 * prototype graph for the slot named `name`.  On hit, writes the slot
 * value to *out and returns 0.  On miss, returns -1 (no fallback retry
 * at T12; T40 lands the GET_FALLBACK retry path).
 *
 * Cycle safety: each top-level call bumps vm->lookup_id, then stamps every
 * visited UObject's lookup_stamp (u32 truncation) so re-visits short-circuit.
 * Cycles in the proto graph are tolerated — iteration terminates after one
 * visit per object.  Without the stamp, a cycle a→b→a would loop forever.
 *
 * Rollover: the low 32 bits of vm->lookup_id wrap eventually.  When the
 * next bump would produce 0, urbi_object_lookup_id_force_wrap is called to
 * clear every UObject's lookup_stamp and reset lookup_id to 1.  T36 may
 * fold the clear pass into the GC mark phase to avoid the separate
 * iteration; the API contract here is unchanged.
 *
 * Returns 0 on hit, -1 on miss.  Negative-rc reservation matches the
 * other M4 ABI surfaces (see urbi_object_add_proto).
 *
 * struct UVM is forward-declared above for urbi_object_alloc; it is
 * already in scope here. */
int  urbi_object_lookup(struct UVM *vm, UObject *obj, USymbol *name, UValue *out);
void urbi_object_lookup_id_force_wrap(struct UVM *vm);

/* === T25: resolve-slot helper (shared between IC slow path and USlotHandle) ===
 *
 * Per pre-M4 GETSLOT/SETSLOT spec §6.3.
 *
 * urbi_object_resolve_slot walks recv's prototype graph for `name` and on
 * hit reports the holding UObject and the slot index in *holder->slots.
 * Same DFS + lookup_stamp cycle-safety contract as urbi_object_lookup, but
 * captures the (holder, index) pair for the IC slow path to fill cache
 * entries that point directly at the storage cell.
 *
 * Returns:
 *   1 — found (*out_holder + *out_index valid)
 *   0 — miss (nothing written)
 *  -1 — error (e.g. resolve-stack depth bound)
 *
 * Bumps vm->lookup_id on entry; honours the wrap protocol that
 * urbi_object_lookup uses (force_wrap when the next id would be 0). */
int urbi_object_resolve_slot(struct UVM *vm, UObject *recv, const USymbol *name,
                             UObject **out_holder, uint32_t *out_index);

/* === T26: install a local slot on a receiver ===
 *
 * Per pre-M2 §6.1 + pre-M4 topology-generation spec §4.2 row 2.
 *
 * If `name` already exists locally on `obj` (i.e. urbi_shape_find_slot hits
 * in obj->shape's lineage), perform an in-place value update; no shape
 * transition, no topology_gen bump.
 *
 * Otherwise transition obj->shape to the child shape via
 * urbi_shape_transition_add_slot, allocate a fresh USlotArray wrapper cell,
 * copy over the existing slot values, write the new value at the freshly
 * added index, shade the OLD wrapper cell (forward Dijkstra barrier — it is
 * about to become unreachable), and publish the new shape + slots pointer.
 *
 * Returns 0 on success, -1 on OOM (either the shape transition or the
 * USlotArray allocation failed).
 *
 * No topology_gen bump for the leaf-shape-add case: per topology spec
 * §4.2 row 2, the IC's per-site shape-mismatch check is sufficient to
 * invalidate stale entries on the receiver.  T27 lands the conditional
 * bump for the "obj is itself a prototype" case via the IS_PROTOTYPE flag. */
int urbi_object_set_local_slot(struct UVM *vm, UObject *obj,
                               USymbol *name, UValue value);

/* === T27: remove a local slot from a receiver ===
 *
 * Per topology-generation spec §4.1 row 1.  If the slot doesn't exist on
 * obj's lineage, returns 0 (silent no-op, no allocation).  Otherwise rebuilds
 * obj->shape via urbi_shape_transition_remove_slot, allocates a fresh
 * USlotArray sized for new_shape->count, copies non-removed slot values,
 * shades the old wrapper, publishes the new shape + slots pointer, and
 * unconditionally bumps vm->topology_gen.
 *
 * Bumps unconditionally (whether obj is a prototype or not) because the
 * slot-removal changes the lineage's resolution semantics: any IC entry
 * that cached a slot pointer past this object is now potentially stale
 * (the slot may have moved to a different index in the new shape).
 *
 * Returns 0 on success or no-op, -1 on OOM. */
int urbi_object_remove_slot(struct UVM *vm, UObject *obj, const USymbol *name);

/* === T28: install / remove / mutate a slot property ===
 *
 * Per topology-generation spec §4.1 rows 5/6/7.  Each routes through
 * urbi_shape_transition_property (T17) for the shape transition, then
 * writes the per-slot UProps* into new_shape->props_table[idx], and
 * bumps vm->topology_gen.
 *
 * - install: allocate a fresh UProps cell, write the supplied UValue at the
 *   field selected by flag_bit (URBI_SLOT_FLAG_OGET or URBI_SLOT_FLAG_OSET)
 *   and CONSTANT bit if flag_bit == URBI_SLOT_FLAG_CONSTANT, transition
 *   shape, publish.  Returns -1 if the slot doesn't exist or on OOM.
 * - remove: clear the flag bit on the per-slot UProps; if all flags now
 *   clear, write NULL into props_table[idx].  Sibling shape transition
 *   with install=0.  Returns 0 if the slot exists; silent no-op (still 0)
 *   if the bit isn't set.  Returns -1 if the slot doesn't exist or on OOM.
 * - set_property_value: in-place write into the existing UProps's oget /
 *   oset field (no shape transition).  Bumps topology_gen because cached
 *   IC uprops[] pointer is stale (next dispatch must re-fetch). */
int urbi_object_install_property   (struct UVM *vm, UObject *obj,
                                    const USymbol *name, uint8_t flag_bit,
                                    UValue value);
int urbi_object_remove_property    (struct UVM *vm, UObject *obj,
                                    const USymbol *name, uint8_t flag_bit);
int urbi_object_set_property_value (struct UVM *vm, UObject *obj,
                                    const USymbol *name, uint8_t flag_bit,
                                    UValue value);

/* === T36: GC root provider for atom singletons + module instances ===
 *
 * Per pre-M3 GC roots spec §5.3 + pre-M4 amendments.  Registered via
 * urbi_gc_register_root_provider in urbi_vm_init after urbi_object_builtin_types_init.
 * Walks: vm->atom_object .. vm->atom_symbol (the nine atom-family singletons),
 * vm->root_shape, and every UModuleInstance reachable from
 * vm->module_instances_head.  Each non-NULL cell is shaded gray directly via
 * gc_shade_gray (the cells are direct UCell pointers, not UValue slots — the
 * mark_root_callback only handles UVAL_CLOSURE / UVAL_OBJECT slots).
 *
 * Once registered, the manual urbi_pin calls on atom singletons (T8) become
 * load-bearing only for cycles BEFORE this provider runs (i.e. mid-init
 * allocations); after first MARK_ROOTS the root walker keeps them alive. */
void urbi_object_register_gc_roots(struct UVM *vm);

/* Convenience inlines — count + indexed access across all three forms.
 *
 * obj->protos encoding (pre-M4 prototype-chain spec §7.2, updated v0.8.2):
 *   - empty:  obj->protos == 0
 *   - single: bit 0 = 1, remaining bits = raw UObject* (alignment guarantees
 *             bit 0 of the pointer is 0 so the tag does not collide)
 *   - heap:   bit 0 = 0, raw bits = UProtos* (alignment guarantees bit 0 = 0)
 *
 * The load-bearing design pin is TIDY-003: per-line NOLINT documents each
 * int-to-pointer cast. */
static inline uint32_t urbi_object_proto_count(const UObject *obj) {
    if (obj->protos == 0U) return 0U;
    if ((obj->protos & 1U) != 0U) return 1U;
    return ((const UProtos *)obj->protos)->n;  /* NOLINT(performance-no-int-to-ptr) — UProtos heap-form pointer-encoding */
}

static inline UObject *urbi_object_proto_at(const UObject *obj, uint32_t i) {
    if (obj->protos == 0U) return NULL;
    if ((obj->protos & 1U) != 0U) {
        return (i == 0U) ? (UObject *)(obj->protos & ~(uintptr_t)1U) : NULL;  /* NOLINT(performance-no-int-to-ptr) — UProtos single-form pointer-encoding */
    }
    {
        UProtos *up = (UProtos *)obj->protos;  /* NOLINT(performance-no-int-to-ptr) — UProtos heap-form pointer-encoding */
        return (i < up->n) ? up->items[i] : NULL;
    }
}

#endif /* UOBJECT_H */
