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
 * urbi_object_atom is provisional — T9 lands the canonical UPROTOS_FOREACH
 * iteration path and decodes this form in one place. */

#include <stdint.h>

#include "object/uobject.h"
#include "object/ushape.h"
#include "uvm.h"
#include "urbi/gc.h"      /* urbi_gc_alloc + urbi_pin */
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
 * pointing at the root Object (provisional — T9's UPROTOS_FOREACH lands
 * the canonical decode path).
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
    /* Single-tag protos encoding (provisional; T9 owns the canonical form):
     * low bit 1 marks single-tag, high bits hold the prototype pointer. */
    o->protos = ((uintptr_t)root << 1) | 1u;
    *slot = o;

    pin_uobject(vm, o);
    return o;
}

/* === T11 stubs ===
 *
 * Public ABI is locked here at T8 so host embedders can compile against the
 * v1.0 surface; T11 lands the real prototype-list mutators with cycle
 * detection and storage-form transitions per spec §4. */

int
urbi_object_add_proto(struct UVM *vm, UObject *obj, UObject *proto)
{
    (void)vm; (void)obj; (void)proto;
    return URBI_ERR_INVALID_ARG;   /* T11 implements */
}

int
urbi_object_remove_proto(struct UVM *vm, UObject *obj, UObject *proto)
{
    (void)vm; (void)obj; (void)proto;
    return URBI_ERR_INVALID_ARG;   /* T11 implements */
}

int
urbi_object_set_protos(struct UVM *vm, UObject *obj, UObject **list, uint32_t n)
{
    (void)vm; (void)obj; (void)list; (void)n;
    return URBI_ERR_INVALID_ARG;   /* T11 implements */
}
