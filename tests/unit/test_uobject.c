/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for src/object/uobject.h — UObject/UProtos/USlot layouts.
 *
 * The header itself carries _Static_assert pins on UObject and USlot widths;
 * if those trip, this file won't compile.  These runtime tests give a second,
 * test-runner-visible signal that the layout is what the spec says it is. */

#include "utest.h"

#include "object/uobject.h"
#include "umodule.h"   /* UValue */
#include "uvm.h"
#include "urbi/object.h"   /* T8 public API */

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* === USlot width === */

UTEST(uobject_uslot_is_exactly_uvalue) {
    /* USlot collapses to UValue per pre-M4 USlot/UProps spec §3. */
    UASSERT_EQ((int)sizeof(USlot), (int)sizeof(UValue));
    UASSERT_EQ((int)sizeof(USlot), 16);
}

/* === UObject header width === */

UTEST(uobject_header_is_48_bytes) {
    /* Pinned by spec §3 (pre-M4 prototype-chain representation design). */
    UASSERT_EQ((int)sizeof(UObject), 48);
}

/* === UObject field offsets ===
 * Field order matters: layout must be exactly cell -> shape -> slots ->
 * protos -> object_id -> lookup_stamp -> flags -> reserved. */

UTEST(uobject_field_order_matches_spec) {
    /* cell at offset 0. */
    UASSERT_EQ((int)offsetof(UObject, cell), 0);
    /* shape at offset 8 (UCell + 6 B compiler-inserted padding). */
    UASSERT_EQ((int)offsetof(UObject, shape), 8);
    /* slots, protos at 16, 24. */
    UASSERT_EQ((int)offsetof(UObject, slots), 16);
    UASSERT_EQ((int)offsetof(UObject, protos), 24);
    /* object_id, lookup_stamp, flags, reserved at 32, 36, 40, 44. */
    UASSERT_EQ((int)offsetof(UObject, object_id), 32);
    UASSERT_EQ((int)offsetof(UObject, lookup_stamp), 36);
    UASSERT_EQ((int)offsetof(UObject, flags), 40);
    UASSERT_EQ((int)offsetof(UObject, reserved), 44);
}

/* === Slot-flag + atom-family bit patterns === */

UTEST(uobject_atom_mask_and_flag_bits_are_distinct) {
    /* Low nibble of UObject.flags reserved for atom family; bit 4/5
     * carry frozen + sandbox-readonly. */
    UASSERT_EQ(URBI_OBJ_ATOM_MASK,       0x0Fu);
    UASSERT_EQ(URBI_OBJ_FLAG_FROZEN,     0x10u);
    UASSERT_EQ(URBI_OBJ_FLAG_SANDBOX_RO, 0x20u);
    /* The four slot-property flags occupy bits 0..3 with no overlap. */
    UASSERT_EQ(URBI_SLOT_FLAG_OGET     |
               URBI_SLOT_FLAG_OSET     |
               URBI_SLOT_FLAG_CONSTANT |
               URBI_SLOT_FLAG_LOCAL,    0x0Fu);
}

UTEST(uobject_atom_family_values_pinned) {
    /* Load-bearing for T8 atom-singleton install order; do not renumber. */
    UASSERT_EQ(URBI_ATOM_OBJECT,  0);
    UASSERT_EQ(URBI_ATOM_INTEGER, 1);
    UASSERT_EQ(URBI_ATOM_FLOAT,   2);
    UASSERT_EQ(URBI_ATOM_STRING,  3);
    UASSERT_EQ(URBI_ATOM_LIST,    4);
    UASSERT_EQ(URBI_ATOM_DICT,    5);
    UASSERT_EQ(URBI_ATOM_TAG,     6);
    UASSERT_EQ(URBI_ATOM_EVENT,   7);
    UASSERT_EQ(URBI_ATOM_SYMBOL,  8);
}

/* === T8: public-mirror atom enum stays in sync with the internal one === */

UTEST(uobject_public_atom_tag_values_match_internal) {
    /* The public URBIAtomFamilyTag (in include/urbi/object.h) uses _F
     * suffixes to dodge namespace collisions but must mirror the internal
     * URBIAtomFamily numerically — urbi_object_atom dispatches on these. */
    UASSERT_EQ((int)URBI_ATOM_OBJECT_F,  (int)URBI_ATOM_OBJECT);
    UASSERT_EQ((int)URBI_ATOM_INTEGER_F, (int)URBI_ATOM_INTEGER);
    UASSERT_EQ((int)URBI_ATOM_FLOAT_F,   (int)URBI_ATOM_FLOAT);
    UASSERT_EQ((int)URBI_ATOM_STRING_F,  (int)URBI_ATOM_STRING);
    UASSERT_EQ((int)URBI_ATOM_LIST_F,    (int)URBI_ATOM_LIST);
    UASSERT_EQ((int)URBI_ATOM_DICT_F,    (int)URBI_ATOM_DICT);
    UASSERT_EQ((int)URBI_ATOM_TAG_F,     (int)URBI_ATOM_TAG);
    UASSERT_EQ((int)URBI_ATOM_EVENT_F,   (int)URBI_ATOM_EVENT);
    UASSERT_EQ((int)URBI_ATOM_SYMBOL_F,  (int)URBI_ATOM_SYMBOL);
}

/* === T8: root Object singleton lifecycle === */

UTEST(uobject_root_object_singleton_has_atom_family_object) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* Pre-condition: vm->atom_object zero-initialised by uvm_init. */
    UASSERT(vm.atom_object == NULL);

    UObject *root = urbi_object_root(&vm);
    UASSERT(root != NULL);

    /* Atom-family bits encode URBI_ATOM_OBJECT (0). */
    UASSERT_EQ((int)(root->flags & URBI_OBJ_ATOM_MASK),
               (int)URBI_ATOM_OBJECT);

    /* First object allocated in this VM gets id 1 (next_object_id init=0,
     * next_id pre-increments).  Reserves 0 as a "no id" sentinel. */
    UASSERT_EQ((int)root->object_id, 1);

    /* Cell type tag set by urbi_gc_alloc to UTYPE_OBJECT. */
    UASSERT_EQ((int)root->cell.type_tag, (int)UTYPE_OBJECT);

    /* Root carries no slots; shape points at the per-VM root hidden class. */
    UASSERT(root->slots == NULL);
    UASSERT(root->shape != NULL);
    UASSERT(root->shape == vm.root_shape);

    /* Root has no prototypes — protos field is the empty form (0). */
    UASSERT_EQ((int)root->protos, 0);

    /* Idempotent — second call returns the same singleton. */
    UASSERT(urbi_object_root(&vm) == root);
    UASSERT(vm.atom_object == root);

    uvm_destroy(&vm);
}

/* === T8: non-root atom singleton (Integer) ===
 *
 * Exercises the generic urbi_object_atom path: lazy-creates the root first,
 * then the named atom, wires its protos to (root << 1) | 1, and pins it. */

UTEST(uobject_atom_integer_singleton_links_to_root) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *integer = urbi_object_atom(&vm, URBI_ATOM_INTEGER_F);
    UASSERT(integer != NULL);
    UASSERT_EQ((int)(integer->flags & URBI_OBJ_ATOM_MASK),
               (int)URBI_ATOM_INTEGER);

    /* urbi_object_atom auto-creates the root first; root gets id 1, integer
     * gets id 2. */
    UASSERT(vm.atom_object != NULL);
    UASSERT_EQ((int)vm.atom_object->object_id, 1);
    UASSERT_EQ((int)integer->object_id,        2);

    /* Single-tag prototype encoding per spec §4.1 (provisional T8 form;
     * T9 lands UPROTOS_FOREACH which decodes this in one place).
     * Low bit 1 marks single-tag; high bits hold the prototype pointer. */
    UASSERT((integer->protos & 1u) == 1u);
    UASSERT((UObject *)(integer->protos >> 1) == vm.atom_object);

    /* Idempotent — second call returns the same singleton. */
    UASSERT(urbi_object_atom(&vm, URBI_ATOM_INTEGER_F) == integer);
    UASSERT(vm.atom_integer == integer);

    uvm_destroy(&vm);
}

/* === T8: independent atoms across the full set === */

UTEST(uobject_atom_singletons_are_independent) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *flt = urbi_object_atom(&vm, URBI_ATOM_FLOAT_F);
    UObject *str = urbi_object_atom(&vm, URBI_ATOM_STRING_F);
    UObject *tag = urbi_object_atom(&vm, URBI_ATOM_TAG_F);

    UASSERT(flt != NULL);
    UASSERT(str != NULL);
    UASSERT(tag != NULL);

    /* Distinct cells. */
    UASSERT(flt != str);
    UASSERT(str != tag);
    UASSERT(tag != flt);

    /* Atom-family bits match. */
    UASSERT_EQ((int)(flt->flags & URBI_OBJ_ATOM_MASK), (int)URBI_ATOM_FLOAT);
    UASSERT_EQ((int)(str->flags & URBI_OBJ_ATOM_MASK), (int)URBI_ATOM_STRING);
    UASSERT_EQ((int)(tag->flags & URBI_OBJ_ATOM_MASK), (int)URBI_ATOM_TAG);

    /* All three share the same root via the single-tag protos encoding. */
    UObject *root = vm.atom_object;
    UASSERT(root != NULL);
    UASSERT((UObject *)(flt->protos >> 1) == root);
    UASSERT((UObject *)(str->protos >> 1) == root);
    UASSERT((UObject *)(tag->protos >> 1) == root);

    uvm_destroy(&vm);
}

/* === T8: URBI_ATOM_OBJECT_F routes through urbi_object_root === */

UTEST(uobject_atom_via_object_f_returns_root) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *via_atom = urbi_object_atom(&vm, URBI_ATOM_OBJECT_F);
    UObject *via_root = urbi_object_root(&vm);

    UASSERT(via_atom != NULL);
    UASSERT(via_atom == via_root);

    uvm_destroy(&vm);
}

/* === T8: invalid family tag returns NULL === */

UTEST(uobject_atom_invalid_family_returns_null) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* 9..15 reserved per uobject.h; >= 9 must not match the switch. */
    UObject *o = urbi_object_atom(&vm, (URBIAtomFamilyTag)9);
    UASSERT(o == NULL);

    uvm_destroy(&vm);
}

/* === T8: T11-stubbed mutators return URBI_ERR_INVALID_ARG === */

UTEST(uobject_proto_mutators_are_t11_stubs) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_root(&vm);
    UASSERT(o != NULL);

    UASSERT_EQ((int)urbi_object_add_proto   (&vm, o, o),       (int)URBI_ERR_INVALID_ARG);
    UASSERT_EQ((int)urbi_object_remove_proto(&vm, o, o),       (int)URBI_ERR_INVALID_ARG);
    UASSERT_EQ((int)urbi_object_set_protos  (&vm, o, NULL, 0u), (int)URBI_ERR_INVALID_ARG);

    uvm_destroy(&vm);
}

void test_uobject_suite(void) {
    utest_run("uobject: USlot == UValue (16 B)", uobject_uslot_is_exactly_uvalue);
    utest_run("uobject: header is 48 bytes", uobject_header_is_48_bytes);
    utest_run("uobject: field order matches spec §3", uobject_field_order_matches_spec);
    utest_run("uobject: atom mask + flag bits distinct", uobject_atom_mask_and_flag_bits_are_distinct);
    utest_run("uobject: atom family values pinned", uobject_atom_family_values_pinned);
    utest_run("uobject: public atom tag values match internal",
              uobject_public_atom_tag_values_match_internal);
    utest_run("uobject: root Object singleton has atom family Object",
              uobject_root_object_singleton_has_atom_family_object);
    utest_run("uobject: atom Integer singleton links to root",
              uobject_atom_integer_singleton_links_to_root);
    utest_run("uobject: atom singletons are independent",
              uobject_atom_singletons_are_independent);
    utest_run("uobject: atom via OBJECT_F returns root",
              uobject_atom_via_object_f_returns_root);
    utest_run("uobject: atom invalid family returns NULL",
              uobject_atom_invalid_family_returns_null);
    utest_run("uobject: proto mutators are T11 stubs",
              uobject_proto_mutators_are_t11_stubs);
}
