/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for src/object/ushape.h — UShape hidden class + UProps + root
 * shape singleton.
 *
 * The header carries a _Static_assert pinning sizeof(UShape) == 56; if it
 * trips this file won't compile.  These runtime tests give a second,
 * test-runner-visible signal and additionally verify the field-offset
 * layout and the root-shape singleton lifecycle. */

#include "utest.h"

#include "object/ushape.h"
#include "object/uobject.h"   /* USlot / UObject layout neighbours */
#include "value/uintern.h"          /* ustr_intern */
#include "vm/uvm.h"
#include "urbi/gc.h"          /* UTYPE_SHAPE */

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* === UShape header width === */

UTEST(ushape_header_is_56_bytes) {
    /* Pinned by spec — UCell + 6 B pad + 7 fields totalling 48 B. */
    UASSERT_EQ((int)sizeof(UShape), 56);
}

/* === UShape field offsets ===
 * cell -> name -> index -> count -> flags -> _pad -> parent -> transitions
 * -> props_table.  Test pins the layout against silent compiler drift. */

UTEST(ushape_field_order_matches_spec) {
    UASSERT_EQ((int)offsetof(UShape, cell), 0);
    /* name at offset 8 (UCell + 6 B compiler-inserted padding). */
    UASSERT_EQ((int)offsetof(UShape, name), 8);
    UASSERT_EQ((int)offsetof(UShape, index), 16);
    UASSERT_EQ((int)offsetof(UShape, count), 20);
    UASSERT_EQ((int)offsetof(UShape, flags), 24);
    UASSERT_EQ((int)offsetof(UShape, _pad), 28);
    UASSERT_EQ((int)offsetof(UShape, parent), 32);
    UASSERT_EQ((int)offsetof(UShape, transitions), 40);
    UASSERT_EQ((int)offsetof(UShape, props_table), 48);
}

/* === UProps layout ===
 * cell -> oget -> oset -> bitfield word.  Pinned alongside the gated
 * sizeof assert in ushape.h. */

UTEST(ushape_uprops_layout_matches_spec) {
    /* Width: 48 B on the supported 64-bit host ABI. */
    UASSERT_EQ((int)sizeof(UProps), 48);
    /* cell at offset 0; 6 B compiler-inserted pad before oget. */
    UASSERT_EQ((int)offsetof(UProps, cell), 0);
    UASSERT_EQ((int)offsetof(UProps, oget), 8);
    UASSERT_EQ((int)offsetof(UProps, oset), 24);
}

/* === Root-shape singleton lifecycle === */

UTEST(ushape_root_starts_null_then_lazy_allocates) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* Pre-condition: vm->root_shape zero-initialised by uvm_init. */
    UASSERT(vm.root_shape == NULL);

    UShape *root = urbi_shape_root(&vm);
    UASSERT(root != NULL);
    UASSERT(vm.root_shape == root);

    /* Root shape carries no slots, no parent, no caches, no props. */
    UASSERT_EQ((int)root->count, 0);
    UASSERT(root->parent == NULL);
    UASSERT(root->transitions == NULL);
    UASSERT(root->props_table == NULL);
    UASSERT(root->name == NULL);

    /* Type tag is UTYPE_SHAPE (set by urbi_gc_alloc). */
    UASSERT_EQ((int)root->cell.type_tag, (int)UTYPE_SHAPE);

    uvm_destroy(&vm);
}

UTEST(ushape_root_is_idempotent_singleton) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UShape *r1 = urbi_shape_root(&vm);
    UShape *r2 = urbi_shape_root(&vm);
    UShape *r3 = urbi_shape_root(&vm);

    UASSERT(r1 != NULL);
    UASSERT(r1 == r2);
    UASSERT(r2 == r3);

    uvm_destroy(&vm);
}

/* === transition_add_slot input guards ===
 * NULL name guarded; transition_property still a stub returning NULL. */

UTEST(ushape_transition_input_guards) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UShape *root = urbi_shape_root(&vm);
    UASSERT(root != NULL);

    /* NULL name short-circuits — root carries name == NULL and would
     * otherwise create a confusing shape. */
    UASSERT(urbi_shape_transition_add_slot(&vm, root, NULL) == NULL);

    /* sibling-property primitive (T17): root has count == 0, so any
     * slot_index is out of range and the call returns NULL. */
    UASSERT(urbi_shape_transition_property(&vm, root, 0u, 0u, 1) == NULL);

    uvm_destroy(&vm);
}

/* === T13: transition cache returns same shape for repeated add ===
 *
 * Two add_slot(root, foo) calls return the same child shape (cache hit on
 * the second call); child wires parent/name/index/count correctly; the
 * lineage walk finds foo at index 0 from the child but not from root. */

UTEST(ushape_transition_cache_returns_same_shape_for_repeated_add) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UShape *root = urbi_shape_root(&vm);
    /* ustr_intern returns const char* — cast to USymbol* for the opaque
     * pointer interface (see ushape.h header comment). */
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    UASSERT(foo != NULL);

    UShape *s1 = urbi_shape_transition_add_slot(&vm, root, foo);
    UShape *s2 = urbi_shape_transition_add_slot(&vm, root, foo);
    UASSERT(s1 != NULL);
    UASSERT(s1 == s2);                 /* cache hit on second call */
    UASSERT(s1->parent == root);
    UASSERT_EQ((int)s1->count, 1);
    UASSERT(s1->name == foo);
    UASSERT_EQ((int)s1->index, 0);

    UASSERT_EQ((int)urbi_shape_find_slot(s1, foo), 0);
    UASSERT_EQ((int)urbi_shape_find_slot(root, foo), -1);

    uvm_destroy(&vm);
}

/* === T13: distinct construction orders yield distinct shapes ===
 *
 * Adding {a, b} vs {b, a} produces two different shapes (no shape
 * coalescing at v1.0); both have count == 2 but distinct lineages. */

UTEST(ushape_distinct_shapes_for_distinct_construction_orders) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UShape *root = urbi_shape_root(&vm);
    USymbol *a = (USymbol *)ustr_intern(&vm, "a", 1);
    USymbol *b = (USymbol *)ustr_intern(&vm, "b", 1);
    UASSERT(a != NULL);
    UASSERT(b != NULL);

    UShape *ab = urbi_shape_transition_add_slot(&vm,
        urbi_shape_transition_add_slot(&vm, root, a), b);
    UShape *ba = urbi_shape_transition_add_slot(&vm,
        urbi_shape_transition_add_slot(&vm, root, b), a);
    UASSERT(ab != NULL);
    UASSERT(ba != NULL);
    UASSERT(ab != ba);
    UASSERT_EQ((int)ab->count, 2);
    UASSERT_EQ((int)ba->count, 2);

    /* Both shapes find a + b in their lineages (different indices,
     * reflecting addition order). */
    UASSERT_EQ((int)urbi_shape_find_slot(ab, a), 0);
    UASSERT_EQ((int)urbi_shape_find_slot(ab, b), 1);
    UASSERT_EQ((int)urbi_shape_find_slot(ba, b), 0);
    UASSERT_EQ((int)urbi_shape_find_slot(ba, a), 1);

    uvm_destroy(&vm);
}

/* === T17: transition_property installs flag bit + lazy props_table ===
 *
 * Installing a property bit on a slot in foo_shape produces a sibling
 * shape that:
 *   - Is distinct from foo_shape (fresh allocation).
 *   - Shares foo_shape's parent (sibling lineage, not child).
 *   - Has the requested flag bit set in the slot's flag-nibble.
 *   - Has a non-NULL props_table (lazy allocation).
 *   - Lineage walk still finds foo at index 0.
 * Re-installing the same bit is idempotent and returns the same sibling. */

UTEST(ushape_transition_property_installs_flag_and_lazy_props_table) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UShape *root = urbi_shape_root(&vm);
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    UASSERT(foo != NULL);

    UShape *foo_shape = urbi_shape_transition_add_slot(&vm, root, foo);
    UASSERT(foo_shape != NULL);
    UASSERT_EQ((int)foo_shape->count, 1);
    UASSERT(foo_shape->props_table == NULL);
    UASSERT_EQ((int)foo_shape->flags, 0);

    /* Install OGET on slot 0. */
    UShape *with_oget = urbi_shape_transition_property(&vm, foo_shape,
        /*slot_index=*/0u, URBI_SLOT_FLAG_OGET, /*install=*/1);
    UASSERT(with_oget != NULL);
    UASSERT(with_oget != foo_shape);                 /* sibling, not parent */
    UASSERT(with_oget->parent == foo_shape->parent); /* shares root parent */
    UASSERT_EQ((int)with_oget->count, (int)foo_shape->count);
    UASSERT(with_oget->name == foo);
    UASSERT_EQ((int)with_oget->index, (int)foo_shape->index);

    /* OGET nibble at slot 0 is set; foo_shape's stays clear. */
    UASSERT_EQ((int)(with_oget->flags & URBI_SLOT_FLAG_OGET),
               (int)URBI_SLOT_FLAG_OGET);
    UASSERT_EQ((int)(foo_shape->flags & URBI_SLOT_FLAG_OGET), 0);

    /* props_table lazy-allocated and seeded to NULL (parent had no table). */
    UASSERT(with_oget->props_table != NULL);
    UASSERT(with_oget->props_table[0] == NULL);

    /* Lineage walk on the sibling still finds foo at index 0. */
    UASSERT_EQ((int)urbi_shape_find_slot(with_oget, foo), 0);

    /* Idempotent: re-installing OGET returns the input shape unchanged. */
    UShape *again = urbi_shape_transition_property(&vm, with_oget,
        0u, URBI_SLOT_FLAG_OGET, 1);
    UASSERT(again == with_oget);

    /* Removing OGET (not yet installed on foo_shape) is also a no-op. */
    UShape *no_change = urbi_shape_transition_property(&vm, foo_shape,
        0u, URBI_SLOT_FLAG_OGET, /*install=*/0);
    UASSERT(no_change == foo_shape);

    /* Out-of-range slot_index returns NULL. */
    UASSERT(urbi_shape_transition_property(&vm, foo_shape,
        /*slot_index=*/1u, URBI_SLOT_FLAG_OGET, 1) == NULL);

    uvm_destroy(&vm);
}

void test_ushape_suite(void) {
    utest_run("ushape: header is 56 bytes",
              ushape_header_is_56_bytes);
    utest_run("ushape: field order matches spec",
              ushape_field_order_matches_spec);
    utest_run("ushape: uprops layout matches spec",
              ushape_uprops_layout_matches_spec);
    utest_run("ushape: root starts null then lazy-allocates",
              ushape_root_starts_null_then_lazy_allocates);
    utest_run("ushape: root is idempotent singleton",
              ushape_root_is_idempotent_singleton);
    utest_run("ushape: transition input guards",
              ushape_transition_input_guards);
    utest_run("ushape: transition cache returns same shape for repeated add",
              ushape_transition_cache_returns_same_shape_for_repeated_add);
    utest_run("ushape: distinct shapes for distinct construction orders",
              ushape_distinct_shapes_for_distinct_construction_orders);
    utest_run("ushape: transition_property installs flag and lazy props_table",
              ushape_transition_property_installs_flag_and_lazy_props_table);
}
