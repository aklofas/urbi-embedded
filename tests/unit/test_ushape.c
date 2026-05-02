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
#include "uvm.h"
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

/* === Transition primitives are stubs at this task === */

UTEST(ushape_transition_stubs_return_null) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UShape *root = urbi_shape_root(&vm);
    UASSERT(root != NULL);

    UASSERT(urbi_shape_transition_add_slot(&vm, root, NULL) == NULL);
    UASSERT(urbi_shape_transition_property(&vm, root, 0u, 0u, 1) == NULL);

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
    utest_run("ushape: transition primitives are stubs",
              ushape_transition_stubs_return_null);
}
