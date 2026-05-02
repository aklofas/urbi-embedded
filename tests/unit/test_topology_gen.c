/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for the M4 topology_gen wiring contract.
 *
 * Spec references:
 *   docs/superpowers/specs/2026-04-29-urbi-pre-m4-topology-generation-design.md §4.1, §4.2
 *
 * §4.1 enumerates the 12 surfaces that MUST bump vm->topology_gen so cached
 * IC entries observe the change.  §4.2 enumerates surfaces that MUST NOT
 * bump (the IC's per-site shape-mismatch check is sufficient).  These tests
 * pin both directions:
 *
 *   §4.1 row 1 — slot remove on any object              (T27)
 *   §4.1 row 2 — slot install on prototype              (T27, IS_PROTOTYPE)
 *   §4.1 row 3 — set on prototype shadowing existing    (T27, IS_PROTOTYPE)
 *   §4.1 row 4 — slot install (any shadow case) on proto (T27)
 *   §4.1 row 5 — install oget                            (T28)
 *   §4.1 row 6 — remove oget                             (T28)
 *   §4.1 row 7 — in-place oget mutation                  (T28)
 *   §4.2 row 1 — local slot value write                  (T29 audit)
 *   §4.2 row 2 — leaf-shape-add on a non-prototype       (T29 audit)
 *
 * Tests for §4.1 rows 8-12 (prototype-list mutations) live in test_uobject.c
 * already (T10's set_protos_* family).  Cross-VM IC isolation moves into
 * test_uic.c at T30.
 *
 * Note on surface count: §4.1's "12 surfaces" includes the seven covered
 * here plus the five prototype-list mutations (set_protos_empty/single/heap
 * × add/remove/set permutations).  This file pins the seven novel surfaces
 * landed at T27/T28; pre-existing T10 coverage handles the other five. */

#include "utest.h"

#include "object/uic.h"
#include "object/umoduleinstance.h"
#include "object/uobject.h"
#include "object/ushape.h"
#include "uintern.h"
#include "umodule.h"
#include "uvm.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* === T27 tests: bump surfaces for slot remove + slot install on prototype === */

UTEST(topology_gen_row_1_remove_slot_bumps) {
    /* §4.1 row 1: removing any slot from any object bumps topology_gen
     * (any IC entry that resolved past this object may now find the slot
     * elsewhere or not at all). */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);

    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    UValue v7 = {.kind = UVAL_INT, .v = {.i = 7}};
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v7), 0);

    /* Snapshot AFTER the install (which is a non-prototype leaf-shape-add
     * and per §4.2 row 2 must NOT bump).  Removing then must increment. */
    uint64_t pre = vm.topology_gen;
    UASSERT_EQ(urbi_object_remove_slot(&vm, o, foo), 0);
    UASSERT(vm.topology_gen == pre + 1u);

    /* Slot is gone. */
    UASSERT_EQ((int)urbi_shape_find_slot(o->shape, foo), -1);
    UASSERT_EQ((int)o->shape->count, 0);

    uvm_destroy(&vm);
}

UTEST(topology_gen_row_3_setslot_on_proto_shadowing_bumps) {
    /* §4.1 row 3: writing a slot on a child that shadows a parent's slot
     * is a leaf-shape-add on the child (the child's shape gains a new local
     * slot).  But the child here is not itself a prototype.  This test
     * specifically pins the load-bearing case: write on a prototype that
     * shadows its own existing slot.  Since the slot already exists on the
     * prototype directly, this is the in-place value update path (§4.2 row 1)
     * — must NOT bump.  Renaming the test target: "set on proto already
     * carrying the slot is in-place; no bump."  The shadowing-via-leaf-add
     * case is already covered by row_4 below. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *parent = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *child  = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *foo    = (USymbol *)ustr_intern(&vm, "foo", 3);

    /* Make `parent` a prototype of `child`. */
    urbi_object_set_protos_single(&vm, child, parent);
    UASSERT(parent->flags & URBI_OBJ_FLAG_IS_PROTOTYPE);

    /* Install `foo` on parent — this is leaf-shape-add ON A PROTOTYPE, so
     * §4.1 row 4 fires.  Must bump. */
    uint64_t pre1 = vm.topology_gen;
    UValue v0 = {.kind = UVAL_INT, .v = {.i = 0}};
    UASSERT_EQ(urbi_object_set_local_slot(&vm, parent, foo, v0), 0);
    UASSERT(vm.topology_gen == pre1 + 1u);

    /* Now overwrite parent.foo in-place.  §4.2 row 1 — must NOT bump. */
    uint64_t pre2 = vm.topology_gen;
    UValue v9 = {.kind = UVAL_INT, .v = {.i = 9}};
    UASSERT_EQ(urbi_object_set_local_slot(&vm, parent, foo, v9), 0);
    UASSERT(vm.topology_gen == pre2);

    uvm_destroy(&vm);
}

UTEST(topology_gen_row_4_install_slot_on_prototype_bumps) {
    /* §4.1 row 4: leaf-shape-add (new slot) on a prototype must bump.
     * Pin via: parent.foo = X, where parent has been wired as a prototype. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *parent = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *child  = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(!(parent->flags & URBI_OBJ_FLAG_IS_PROTOTYPE));

    /* Wire parent as child's prototype.  set_protos_single bumps once
     * (§4.1 row 8 — prototype-list mutation). */
    urbi_object_set_protos_single(&vm, child, parent);
    UASSERT(parent->flags & URBI_OBJ_FLAG_IS_PROTOTYPE);

    /* Now install `foo` on parent.  Leaf-shape-add path; obj is a
     * prototype, so the conditional bump fires.  Net delta = 1. */
    uint64_t pre = vm.topology_gen;
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    UValue v1 = {.kind = UVAL_INT, .v = {.i = 1}};
    UASSERT_EQ(urbi_object_set_local_slot(&vm, parent, foo, v1), 0);
    UASSERT(vm.topology_gen == pre + 1u);

    /* Sanity: the slot is on the prototype, not the child. */
    UASSERT_EQ((int)urbi_shape_find_slot(parent->shape, foo), 0);
    UASSERT_EQ((int)urbi_shape_find_slot(child->shape, foo), -1);

    uvm_destroy(&vm);
}

/* === T28 tests: property install / remove / in-place mutation surfaces === */

UTEST(topology_gen_row_5_install_oget_bumps) {
    /* §4.1 row 5: installing an oget (or oset / constant) on a slot bumps
     * topology_gen because the IC's cached uprops[] pointer + flags become
     * stale (the slot now dispatches via getter/setter instead of direct
     * read/write). */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    UValue v0 = {.kind = UVAL_INT, .v = {.i = 0}};
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v0), 0);

    /* Synthetic getter — any UValue carrying an object pointer satisfies
     * the install signature.  IS-the-getter semantics aren't checked at
     * install time; T22 dispatch wires the actual call. */
    UObject *root = urbi_object_root(&vm);
    UASSERT(root != NULL);
    UValue getter = {.kind = UVAL_CLOSURE, .v = {.p = (void *)root}};

    uint64_t pre = vm.topology_gen;
    UASSERT_EQ(urbi_object_install_property(&vm, o, foo,
                                            URBI_SLOT_FLAG_OGET, getter), 0);
    UASSERT(vm.topology_gen == pre + 1u);

    /* The shape's flag nibble for slot 0 now has OGET set. */
    UASSERT(o->shape->flags & URBI_SLOT_FLAG_OGET);
    UASSERT(o->shape->props_table != NULL);
    UASSERT(o->shape->props_table[0] != NULL);

    uvm_destroy(&vm);
}

UTEST(topology_gen_row_6_remove_oget_bumps) {
    /* §4.1 row 6: removing an oget bumps for the same reason — the IC's
     * cached flags need to drop the OGET bit. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    UValue v0 = {.kind = UVAL_INT, .v = {.i = 0}};
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v0), 0);

    UObject *root = urbi_object_root(&vm);
    UValue getter = {.kind = UVAL_CLOSURE, .v = {.p = (void *)root}};
    UASSERT_EQ(urbi_object_install_property(&vm, o, foo,
                                            URBI_SLOT_FLAG_OGET, getter), 0);
    UASSERT(o->shape->flags & URBI_SLOT_FLAG_OGET);

    /* Removing must bump. */
    uint64_t pre = vm.topology_gen;
    UASSERT_EQ(urbi_object_remove_property(&vm, o, foo,
                                           URBI_SLOT_FLAG_OGET), 0);
    UASSERT(vm.topology_gen == pre + 1u);

    /* OGET bit now clear; props_table entry dropped to NULL (only flag
     * was OGET so all_clear path fires). */
    UASSERT_EQ((int)(o->shape->flags & URBI_SLOT_FLAG_OGET), 0);

    uvm_destroy(&vm);
}

UTEST(topology_gen_row_7_in_place_oget_mutation_bumps) {
    /* §4.1 row 7: rewriting an existing UProps's oget value in place bumps
     * because cached IC uprops[] pointer is the same but the value behind
     * it has changed.  Subsequent dispatches must re-fetch. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    UValue v0 = {.kind = UVAL_INT, .v = {.i = 0}};
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v0), 0);

    UObject *root = urbi_object_root(&vm);
    UValue getter1 = {.kind = UVAL_CLOSURE, .v = {.p = (void *)root}};
    UASSERT_EQ(urbi_object_install_property(&vm, o, foo,
                                            URBI_SLOT_FLAG_OGET, getter1), 0);

    /* In-place mutation: pass a different UValue.  Same UProps cell stays
     * in props_table[0]; only oget's payload changes. */
    UProps *up_before = o->shape->props_table[0];
    UASSERT(up_before != NULL);

    uint64_t pre = vm.topology_gen;
    UObject *o2 = urbi_object_alloc(&vm, URBI_ATOM_INTEGER);
    UASSERT(o2 != NULL);
    UValue getter2 = {.kind = UVAL_CLOSURE, .v = {.p = (void *)o2}};
    UASSERT_EQ(urbi_object_set_property_value(&vm, o, foo,
                                              URBI_SLOT_FLAG_OGET, getter2), 0);
    UASSERT(vm.topology_gen == pre + 1u);

    /* Same UProps pointer, different oget payload. */
    UASSERT(o->shape->props_table[0] == up_before);
    UASSERT(up_before->oget.v.p == (void *)o2);

    uvm_destroy(&vm);
}

/* === T29 audit tests: surfaces that MUST NOT bump === */

UTEST(topology_gen_row_4_2_1_local_slot_value_write_does_not_bump) {
    /* §4.2 row 1: in-place value write on an existing local slot does NOT
     * bump.  IC entries cached against this shape stay valid because the
     * slot's storage location and shape are unchanged. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    UValue v1 = {.kind = UVAL_INT, .v = {.i = 1}};
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v1), 0);

    /* Snapshot AFTER the install; second set is the in-place case. */
    uint64_t pre = vm.topology_gen;
    UValue v2 = {.kind = UVAL_INT, .v = {.i = 2}};
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v2), 0);
    UASSERT(vm.topology_gen == pre);   /* no bump */

    uvm_destroy(&vm);
}

UTEST(topology_gen_row_4_2_2_leaf_shape_add_does_not_bump_when_not_prototype) {
    /* §4.2 row 2: leaf-shape-add on an object that is NOT a prototype must
     * NOT bump.  IC's per-site shape-mismatch check catches the new shape
     * naturally on the next access. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT_EQ((int)(o->flags & URBI_OBJ_FLAG_IS_PROTOTYPE), 0);

    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    USymbol *bar = (USymbol *)ustr_intern(&vm, "bar", 3);

    uint64_t pre = vm.topology_gen;
    UValue v1 = {.kind = UVAL_INT, .v = {.i = 1}};
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v1), 0);
    UValue v2 = {.kind = UVAL_INT, .v = {.i = 2}};
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, bar, v2), 0);
    UASSERT(vm.topology_gen == pre);   /* both slot adds: no bump */

    uvm_destroy(&vm);
}

/* === T29 audit tests: IC interaction with topology_gen === */

UTEST(uic_after_topology_bump_invalidates_entries) {
    /* When topology_gen advances past the value cached in an IC entry, the
     * IC's per-entry topology_gen field no longer matches vm->topology_gen.
     * Fast-path dispatch checks (recv->shape == ic->recv_shapes[k]) AND
     * (vm->topology_gen == ic->topology_gen[k]); the second arm is the
     * topology invalidation gate per §3.1. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    UValue v1 = {.kind = UVAL_INT, .v = {.i = 1}};
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v1), 0);

    /* Fill an IC via the slow path. */
    UIC ic = {0};
    ic.name = foo;
    UValue out;
    UASSERT_EQ(urbi_slot_get_slow(&vm, o, &ic, &out), 0);
    UASSERT_EQ((int)ic.n, 1);
    uint64_t cached_gen = ic.topology_gen[0];
    UASSERT(cached_gen == vm.topology_gen);   /* freshly filled, equal */

    /* Force a bump via property install. */
    UObject *root = urbi_object_root(&vm);
    UValue getter = {.kind = UVAL_CLOSURE, .v = {.p = (void *)root}};
    UASSERT_EQ(urbi_object_install_property(&vm, o, foo,
                                            URBI_SLOT_FLAG_OGET, getter), 0);

    /* The entry's cached topology_gen no longer matches vm->topology_gen.
     * That mismatch is the invalidation signal — the dispatch fast path
     * compares the two. */
    UASSERT(cached_gen != vm.topology_gen);
    UASSERT(ic.topology_gen[0] == cached_gen);   /* IC field unchanged */

    uvm_destroy(&vm);
}

UTEST(uic_stays_hot_after_local_slot_value_write) {
    /* Conversely: in-place value write on a local slot does NOT bump,
     * so the cached topology_gen[0] still matches vm->topology_gen.
     * Fast-path stays warm. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    UValue v1 = {.kind = UVAL_INT, .v = {.i = 1}};
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v1), 0);

    UIC ic = {0};
    ic.name = foo;
    UValue out;
    UASSERT_EQ(urbi_slot_get_slow(&vm, o, &ic, &out), 0);
    UASSERT(ic.topology_gen[0] == vm.topology_gen);

    /* In-place rewrite — no bump. */
    UValue v2 = {.kind = UVAL_INT, .v = {.i = 99}};
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v2), 0);

    /* Cached topology_gen still matches. */
    UASSERT(ic.topology_gen[0] == vm.topology_gen);
    UASSERT_EQ((int)o->slots[0].v.i, 99);

    uvm_destroy(&vm);
}

void test_topology_gen_suite(void) {
    /* §4.1 bump surfaces (T27 + T28). */
    utest_run("topology_gen: §4.1 row 1 — slot remove bumps",
              topology_gen_row_1_remove_slot_bumps);
    utest_run("topology_gen: §4.1 row 3 — set on proto carrying slot is in-place (no bump)",
              topology_gen_row_3_setslot_on_proto_shadowing_bumps);
    utest_run("topology_gen: §4.1 row 4 — install slot on prototype bumps",
              topology_gen_row_4_install_slot_on_prototype_bumps);
    utest_run("topology_gen: §4.1 row 5 — install oget bumps",
              topology_gen_row_5_install_oget_bumps);
    utest_run("topology_gen: §4.1 row 6 — remove oget bumps",
              topology_gen_row_6_remove_oget_bumps);
    utest_run("topology_gen: §4.1 row 7 — in-place oget mutation bumps",
              topology_gen_row_7_in_place_oget_mutation_bumps);
    /* §4.2 non-bump surfaces (T29). */
    utest_run("topology_gen: §4.2 row 1 — local slot value write does not bump",
              topology_gen_row_4_2_1_local_slot_value_write_does_not_bump);
    utest_run("topology_gen: §4.2 row 2 — leaf-shape-add on non-prototype does not bump",
              topology_gen_row_4_2_2_leaf_shape_add_does_not_bump_when_not_prototype);
    /* IC invariant pinning (T29). */
    utest_run("topology_gen: IC after topology bump shows mismatched generation",
              uic_after_topology_bump_invalidates_entries);
    utest_run("topology_gen: IC stays hot after local slot value write",
              uic_stays_hot_after_local_slot_value_write);
}
