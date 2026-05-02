/* SPDX-License-Identifier: BSD-3-Clause */
/* uobject.c — atom-family singletons + UObject allocator (M4 / T8).
 *
 * Spec references:
 *   docs/superpowers/specs/2026-04-29-urbi-pre-m4-prototype-chain-representation-design.md §3, §4.1, §8.1
 *
 * Per-VM lazy-allocated atom prototypes: root Object plus the eight built-in
 * atoms (Integer/Float/String/List/Dict/Tag/Event/Symbol).  Each is pinned
 * via urbi_pin so that GC cycles before the T36 root-provider lands cannot
 * reclaim them.
 *
 * The single-tag prototype encoding `(root << 1) | 1` used in
 * urbi_object_atom matches the canonical form decoded by UPROTOS_FOREACH
 * (src/object/uobject.h, T9). */

#include <stdint.h>

#include "object/uobject.h"
#include "object/ushape.h"
#include "uvm.h"
#include "urbi/gc.h"      /* urbi_gc_alloc + urbi_pin */
#include "gc/ugc_incremental.h"   /* gc_shade_gray (T10 mutation primitives) */
#include "urbi/object.h"
#include "urbi/urbi.h"    /* urbi_panic + URBI_OK / UErrCode */

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
static uint32_t
next_id(UVM *vm)
{
    if (vm->next_object_id == UINT32_MAX) {
        urbi_panic("URBI_FATAL_OBJECT_ID_EXHAUSTED");
    }
    return ++vm->next_object_id;
}

/* === pin_uobject ===
 *
 * urbi_pin takes a UValue (M3 row 10 §4.2) and only acts on heap-bearing
 * UValKinds (currently UVAL_CLOSURE only — UVAL_OBJECT is a later M4
 * addition).  At T8 the cleanest way to pin a UObject* is to wrap it as
 * a synthetic UVAL_CLOSURE so urbi_pin's uvalue_is_heap check passes;
 * UClosure embeds UCell as its first member at offset 0, so the cast is
 * well-defined and the pin reaches the real cell's gc_byte.  This mirrors
 * the helper used in tests/unit/test_ugc_handle.c. */
static void
pin_uobject(UVM *vm, UObject *o)
{
    UValue v = {0};
    v.kind = UVAL_CLOSURE;
    v.v.p  = (void *)o;
    urbi_pin(vm, v);
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
    o->slots        = NULL;       /* zero-slot at construction; T15 lands slot transitions */
    o->protos       = 0u;         /* empty form per spec §4.1 */
    o->object_id    = next_id(vm);
    o->lookup_stamp = 0u;
    o->flags        = (uint32_t)((uint32_t)family & URBI_OBJ_ATOM_MASK);
    o->reserved     = 0u;
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

/* === Prototype-mutation primitives (T10 — per pre-M4 prototype-chain spec §5) ===
 *
 * Three barrier-aware primitives — every prototype-chain mutation routes
 * through one of these.  Each does the same three steps in order:
 *   1. Forward Dijkstra barrier on the EXISTING protos value (shade pre-
 *      overwrite — preserves the no-black-to-white tri-color invariant).
 *   2. Forward barrier on the inserted child(ren) (shade pre-write).
 *   3. Bump vm->topology_gen so cached IC entries observe the change
 *      (per pre-M2 §7.4 / pre-M4 topology-generation spec §3.1).
 *
 * gc_shade_gray (src/gc/ugc_incremental.c) is idempotent and self-guards
 * against non-white cells, so we can call it unconditionally without a
 * separate uvalue_is_heap_white check — matching the pattern used in
 * src/object/utypes_init.c walkers.  Under URBI_GC_NONE (v2 build) these
 * primitives still run; gc_shade_gray's color check is a no-op when color
 * tracking is disabled. */

/* shade_existing_protos — internal helper. Decodes obj->protos's three
 * storage forms (empty/single/heap per spec §4.1) and shades the underlying
 * cell(s) before the field is overwritten. */
static void
shade_existing_protos(UVM *vm, UObject *obj)
{
    uintptr_t raw = obj->protos;
    if (raw == 0u) {
        return;   /* empty form — nothing to shade */
    }
    if ((raw & 1u) != 0u) {
        /* single form: bit 0 set, address in high bits */
        gc_shade_gray(vm, (UCell *)(raw >> 1));
    } else {
        /* heap form: raw is a UProtos*. Shade the UProtos cell itself.
         * The UObject*s in items[] are reachable from the UProtos walker
         * (utypes_init.c walk_uprotos), so shading the UProtos is sufficient
         * to keep them alive across the overwrite — the GC will trace into
         * items[] when it next dequeues this gray cell. */
        gc_shade_gray(vm, (UCell *)raw);
    }
}

void
urbi_object_set_protos_empty(UVM *vm, UObject *obj)
{
    shade_existing_protos(vm, obj);
    obj->protos = 0u;
    vm->topology_gen++;
}

void
urbi_object_set_protos_single(UVM *vm, UObject *obj, UObject *p)
{
    shade_existing_protos(vm, obj);
    /* Forward barrier on the inserted child (per spec §5.3 — barrier is
     * per-write, not per-disposition). */
    gc_shade_gray(vm, (UCell *)p);
    obj->protos = ((uintptr_t)p << 1) | 1u;
    vm->topology_gen++;
}

void
urbi_object_set_protos_heap(UVM *vm, UObject *obj, UProtos *up)
{
    shade_existing_protos(vm, obj);
    /* Shade every item in the new UProtos plus the UProtos cell itself —
     * pre-write barriers on inserted children (per spec §5.3). */
    for (uint32_t i = 0; i < up->n; i++) {
        gc_shade_gray(vm, (UCell *)up->items[i]);
    }
    gc_shade_gray(vm, (UCell *)up);
    obj->protos = (uintptr_t)up;
    vm->topology_gen++;
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

    /* Pin so GC won't collect — host-handle table from M3 row 10 §4.2.
     * T36's root provider also covers this, but pinning at creation keeps
     * semantics defensible during partial wiring. */
    pin_uobject(vm, o);
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

    pin_uobject(vm, o);
    return o;
}

/* === T11: prototype-list mutators ===
 *
 * Implements the public ABI declared at T8 by composing the T10 mutation
 * primitives with a valid_proto atom-family check (per pre-M2 §5.1-§5.3
 * and pre-M4 prototype-chain spec §5.5).
 *
 * Error reporting: M3 baseline only defines URBI_ERR_INVALID_ARG; richer
 * codes (URBI_ERR_INHERIT_TYPE_MISMATCH, URBI_ERR_TOO_MANY_PROTOS) are
 * future-stdlib work.  All failure modes here return URBI_ERR_INVALID_ARG. */

/* Cap on the number of distinct prototypes a single setProtos call may
 * install (after dedup).  Stays in sync with the plan's stack-array sizing;
 * a larger cap can land in v1.x as part of stdlib error-code expansion. */
#define URBI_PROTOS_SETPROTOS_CAP  64u

/* valid_proto — atom-family compatibility check per pre-M4 prototype-chain
 * spec §5.5.  An atom can only inherit from its own family OR from the
 * root Object atom.  The root Object never blocks (either side may be
 * URBI_ATOM_OBJECT and the relationship is permitted). */
static int
valid_proto(const UObject *obj, const UObject *p)
{
    URBIAtomFamily ofam = (URBIAtomFamily)(obj->flags & URBI_OBJ_ATOM_MASK);
    URBIAtomFamily pfam = (URBIAtomFamily)(p->flags   & URBI_OBJ_ATOM_MASK);
    if (ofam == URBI_ATOM_OBJECT || pfam == URBI_ATOM_OBJECT) {
        return 1;
    }
    return ofam == pfam;
}

/* urbi_protos_alloc — allocate a fresh UProtos block for `n` items via the
 * GC (UTYPE_PROTOS).  Caller fills items[0..n).  Returns NULL on OOM. */
static UProtos *
urbi_protos_alloc(UVM *vm, uint32_t n)
{
    UCell *c = urbi_gc_alloc(vm,
                             sizeof(UProtos) + (size_t)n * sizeof(UObject *),
                             UTYPE_PROTOS);
    if (c == NULL) {
        return NULL;
    }
    UProtos *up = (UProtos *)c;
    up->n    = n;
    up->_pad = 0u;
    for (uint32_t i = 0; i < n; i++) {
        up->items[i] = NULL;
    }
    return up;
}

int
urbi_object_add_proto(struct UVM *vm, UObject *obj, UObject *proto)
{
    if (vm == NULL || obj == NULL || proto == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    if (!valid_proto(obj, proto)) {
        return URBI_ERR_INVALID_ARG;
    }

    /* Prepend at index 0 per pre-M2 §5.1: most-recently-added prototype
     * gets MRO priority. */
    uint32_t old_n = urbi_object_proto_count(obj);

    if (old_n == 0u) {
        urbi_object_set_protos_single(vm, obj, proto);
        return URBI_OK;
    }

    /* old_n >= 1 — build a fresh UProtos with [proto, ...existing]. */
    uint32_t new_n = old_n + 1u;
    if (new_n > URBI_PROTOS_SETPROTOS_CAP) {
        return URBI_ERR_INVALID_ARG;
    }
    UProtos *up = urbi_protos_alloc(vm, new_n);
    if (up == NULL) {
        return URBI_ERR_INVALID_ARG;   /* OOM — no URBI_ERR_OOM at v1.0 surface */
    }
    up->items[0] = proto;
    for (uint32_t i = 0; i < old_n; i++) {
        up->items[i + 1u] = urbi_object_proto_at(obj, i);
    }
    urbi_object_set_protos_heap(vm, obj, up);
    return URBI_OK;
}

int
urbi_object_remove_proto(struct UVM *vm, UObject *obj, UObject *proto)
{
    if (vm == NULL || obj == NULL || proto == NULL) {
        return URBI_ERR_INVALID_ARG;
    }

    uint32_t old_n = urbi_object_proto_count(obj);

    /* Find first occurrence; silent no-op if absent (legacy semantics per
     * pre-M2 §5.2). */
    uint32_t idx = old_n;   /* sentinel "not found" */
    for (uint32_t i = 0; i < old_n; i++) {
        if (urbi_object_proto_at(obj, i) == proto) {
            idx = i;
            break;
        }
    }
    if (idx == old_n) {
        return URBI_OK;   /* not present — silent no-op */
    }

    uint32_t new_n = old_n - 1u;
    if (new_n == 0u) {
        urbi_object_set_protos_empty(vm, obj);
        return URBI_OK;
    }
    if (new_n == 1u) {
        /* Pick the survivor (the one element whose index isn't `idx`). */
        UObject *survivor = urbi_object_proto_at(obj, (idx == 0u) ? 1u : 0u);
        urbi_object_set_protos_single(vm, obj, survivor);
        return URBI_OK;
    }

    /* new_n >= 2: build a fresh UProtos skipping idx. */
    UProtos *up = urbi_protos_alloc(vm, new_n);
    if (up == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    uint32_t out = 0u;
    for (uint32_t i = 0; i < old_n; i++) {
        if (i == idx) continue;
        up->items[out++] = urbi_object_proto_at(obj, i);
    }
    urbi_object_set_protos_heap(vm, obj, up);
    return URBI_OK;
}

int
urbi_object_set_protos(struct UVM *vm, UObject *obj, UObject **list, uint32_t n)
{
    if (vm == NULL || obj == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    if (n > 0u && list == NULL) {
        return URBI_ERR_INVALID_ARG;
    }

    /* Dedup first-occurrence-wins onto a stack array; cap at
     * URBI_PROTOS_SETPROTOS_CAP distinct survivors (per plan).  Skip NULL
     * entries up-front — they are invalid prototype slots. */
    UObject *deduped[URBI_PROTOS_SETPROTOS_CAP];
    uint32_t dn = 0u;
    for (uint32_t i = 0; i < n; i++) {
        UObject *cand = list[i];
        if (cand == NULL) {
            return URBI_ERR_INVALID_ARG;
        }
        /* O(dn) duplicate check — dn bounded by 64 so this is cheap. */
        int dup = 0;
        for (uint32_t j = 0; j < dn; j++) {
            if (deduped[j] == cand) { dup = 1; break; }
        }
        if (dup) continue;
        if (dn >= URBI_PROTOS_SETPROTOS_CAP) {
            return URBI_ERR_INVALID_ARG;   /* over cap */
        }
        deduped[dn++] = cand;
    }

    /* Validate every survivor BEFORE mutating any state — atomicity per the
     * plan's "no partial state" requirement. */
    for (uint32_t i = 0; i < dn; i++) {
        if (!valid_proto(obj, deduped[i])) {
            return URBI_ERR_INVALID_ARG;
        }
    }

    /* All checks passed; dispatch on dedup count. */
    if (dn == 0u) {
        urbi_object_set_protos_empty(vm, obj);
        return URBI_OK;
    }
    if (dn == 1u) {
        urbi_object_set_protos_single(vm, obj, deduped[0]);
        return URBI_OK;
    }
    UProtos *up = urbi_protos_alloc(vm, dn);
    if (up == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    for (uint32_t i = 0; i < dn; i++) {
        up->items[i] = deduped[i];
    }
    urbi_object_set_protos_heap(vm, obj, up);
    return URBI_OK;
}
