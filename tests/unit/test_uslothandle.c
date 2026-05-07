/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for src/object/uslothandle.h — USlotHandle creation +
 * validate-or-refresh on read/write (M4 / T37).
 *
 * Three behavioural tests:
 *   1. get_slot_returns_handle_pointing_at_owner — basic creation;
 *      handle's owner / shape_at_create / slot_index match the resolved
 *      holder; read_value returns the stored UValue.
 *   2. refresh_after_shape_transition — adding another slot to obj
 *      transitions its shape; read_value still works (refreshes the
 *      cached shape_at_create), and the cached fields update in place.
 *   3. becomes_invalid_after_slot_removal — removing the slot makes
 *      read_value return -1.
 *
 * Pre-M4 USlot/UProps spec §7. */

#include "utest.h"

#include "object/uobject.h"
#include "object/uslothandle.h"
#include "object/ushape.h"
#include "value/uintern.h"      /* ustr_intern → USymbol* */
#include "module/umodule.h"      /* UValue */
#include "vm/uvm.h"
#include "urbi/object.h"

#include <stdint.h>

#define UTEST(name) static void name(void)

/* ===== Test 1: handle points at the resolving owner + cached fields match ===== */

UTEST(uslothandle_get_slot_returns_handle_pointing_at_owner) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);

    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    UASSERT(foo != NULL);

    UValue v42;
    v42.kind = UVAL_INT;
    v42.v.i  = 42;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v42), 0);

    USlotHandle *h = urbi_object_get_slot(&vm, o, foo);
    UASSERT(h != NULL);
    UASSERT(h->owner == o);
    UASSERT(h->shape_at_create == o->shape);
    UASSERT_EQ((int)h->slot_index, 0);
    UASSERT(h->name == foo);

    UValue out;
    UASSERT_EQ(urbi_slothandle_read_value(&vm, h, &out), 0);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ((int)out.v.i, 42);

    urbi_vm_destroy(&vm);
}

/* ===== Test 2: shape transition refreshes cached state on next access ===== */

UTEST(uslothandle_refresh_after_shape_transition) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);

    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    USymbol *bar = (USymbol *)ustr_intern(&vm, "bar", 3);

    UValue v7; v7.kind = UVAL_INT; v7.v.i = 7;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v7), 0);

    USlotHandle *h = urbi_object_get_slot(&vm, o, foo);
    UASSERT(h != NULL);
    UShape *shape_before = h->shape_at_create;

    /* Add another slot to o.  This transitions o->shape to a child shape
     * (per pre-M2 §7.1).  foo's slot_index in the new shape is unchanged
     * (foo was added first).  The handle's shape_at_create is now stale. */
    UValue v8; v8.kind = UVAL_INT; v8.v.i = 8;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, bar, v8), 0);
    UASSERT(o->shape != shape_before);   /* shape genuinely transitioned */

    /* Read still works: validate_or_refresh re-resolves by name and
     * updates the cached state in place. */
    UValue out;
    UASSERT_EQ(urbi_slothandle_read_value(&vm, h, &out), 0);
    UASSERT_EQ((int)out.v.i, 7);
    UASSERT(h->shape_at_create == o->shape);   /* refresh updated cache */

    urbi_vm_destroy(&vm);
}

/* ===== Test 3: removed slot → handle permanently invalid ===== */

UTEST(uslothandle_becomes_invalid_after_slot_removal) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);

    UValue v1; v1.kind = UVAL_INT; v1.v.i = 1;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v1), 0);

    USlotHandle *h = urbi_object_get_slot(&vm, o, foo);
    UASSERT(h != NULL);

    UASSERT_EQ(urbi_object_remove_slot(&vm, o, foo), 0);

    UValue out;
    UASSERT_EQ(urbi_slothandle_read_value(&vm, h, &out), -1);
    /* Re-read also returns -1 (the slot stays removed). */
    UASSERT_EQ(urbi_slothandle_read_value(&vm, h, &out), -1);

    urbi_vm_destroy(&vm);
}

/* ===== Test 4: write_value path — same validate-or-refresh contract ===== */

UTEST(uslothandle_write_value_updates_owner_slot) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);

    UValue v_init; v_init.kind = UVAL_INT; v_init.v.i = 100;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v_init), 0);

    USlotHandle *h = urbi_object_get_slot(&vm, o, foo);
    UASSERT(h != NULL);

    UValue v_new; v_new.kind = UVAL_INT; v_new.v.i = 999;
    UASSERT_EQ(urbi_slothandle_write_value(&vm, h, v_new), 0);
    UASSERT_EQ((int)o->slots[0].v.i, 999);

    /* Write to a removed slot returns -1 too. */
    UASSERT_EQ(urbi_object_remove_slot(&vm, o, foo), 0);
    UValue v_post; v_post.kind = UVAL_INT; v_post.v.i = 1;
    UASSERT_EQ(urbi_slothandle_write_value(&vm, h, v_post), -1);

    urbi_vm_destroy(&vm);
}

/* ===== Test 5: get_slot on missing name returns NULL ===== */

UTEST(uslothandle_get_slot_miss_returns_null) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *bogus = (USymbol *)ustr_intern(&vm, "doesNotExist", 12);

    UASSERT(urbi_object_get_slot(&vm, o, bogus) == NULL);
    UASSERT(urbi_object_get_slot(&vm, NULL, bogus) == NULL);
    UASSERT(urbi_object_get_slot(&vm, o, NULL) == NULL);

    urbi_vm_destroy(&vm);
}

/* ===== Test 6: get_slot resolves through the prototype chain ===== */

UTEST(uslothandle_get_slot_resolves_via_proto_chain) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* parent has the slot; child inherits via prototype. */
    UObject *parent = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *child  = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);

    UValue v77; v77.kind = UVAL_INT; v77.v.i = 77;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, parent, foo, v77), 0);
    UASSERT_EQ(urbi_object_add_proto(&vm, child, parent), URBI_OK);

    USlotHandle *h = urbi_object_get_slot(&vm, child, foo);
    UASSERT(h != NULL);
    UASSERT(h->owner == parent);   /* points at the holder, not the receiver */

    UValue out;
    UASSERT_EQ(urbi_slothandle_read_value(&vm, h, &out), 0);
    UASSERT_EQ((int)out.v.i, 77);

    urbi_vm_destroy(&vm);
}

/* === Suite entry point === */

void test_uslothandle_suite(void) {
    printf("  [uslothandle]\n");
    utest_run("uslothandle: get_slot returns handle pointing at owner",
              uslothandle_get_slot_returns_handle_pointing_at_owner);
    utest_run("uslothandle: refresh after shape transition",
              uslothandle_refresh_after_shape_transition);
    utest_run("uslothandle: becomes invalid after slot removal",
              uslothandle_becomes_invalid_after_slot_removal);
    utest_run("uslothandle: write_value updates owner slot",
              uslothandle_write_value_updates_owner_slot);
    utest_run("uslothandle: get_slot miss returns NULL",
              uslothandle_get_slot_miss_returns_null);
    utest_run("uslothandle: get_slot resolves via proto chain",
              uslothandle_get_slot_resolves_via_proto_chain);
}
