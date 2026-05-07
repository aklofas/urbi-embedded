/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for src/object/uic.h — UIC inline-cache record + tunable.
 *
 * The header carries a gated _Static_assert pinning sizeof(UIC) == 144 at
 * the default 4-entry / 64-bit-pointer build; if it trips this file won't
 * compile.  These runtime tests give a second, test-runner-visible signal
 * and additionally re-pin the URBI_SLOT_FLAG_* attribute bits for the
 * IC.flags summary defined alongside in uobject.h.
 *
 * T16 also covers UModuleInstance / UProtoInstance (the per-VM IC RAM
 * tier) — multi-instance independence is the load-bearing invariant. */

#include "utest.h"

#include "object/uic.h"
#include "object/umoduleinstance.h"
#include "object/uobject.h"
#include "object/ushape.h"     /* urbi_shape_find_slot — T25 slow-path tests */
#include "urbi/urbi.h"         /* urbi_get_determinism_checksum — URBI_DEBUG only */
#include "value/uintern.h"
#include "umodule.h"
#include "uvm.h"
#include "realm/urealm.h"
#include "value/uarena.h"
#include "parse/uast.h"
#include "uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

UTEST(uic_layout_at_default_4_entries) {
    /* This test pins the v1.0 default (URBI_IC_ENTRIES_PER_SITE=4 → UIC=144 B
     * on 64-bit hosts).  Cross-builds at the footprint preset (=2 per T44)
     * or the extreme footprint (=1) intentionally select a smaller layout;
     * skip the assertion in those builds rather than re-targeting it.  The
     * compile-time sizeof_static asserts in src/object/uic.h cover the
     * non-default sizes — this runtime test gates only the canonical
     * default the determinism-default + cross-arm + cross-riscv targets
     * compile against. */
#if URBI_IC_ENTRIES_PER_SITE == 4
    UASSERT_EQ(URBI_IC_ENTRIES_PER_SITE, 4);
#  if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
    UASSERT_EQ((int)sizeof(UIC), 144);
#  endif
#endif
}

UTEST(uic_flag_bits_distinct) {
    UASSERT_EQ((int)(URBI_SLOT_FLAG_OGET
                   | URBI_SLOT_FLAG_OSET
                   | URBI_SLOT_FLAG_CONSTANT
                   | URBI_SLOT_FLAG_LOCAL),
               0x0F);
}

/* === T16: UModuleInstance + UProtoInstance ===
 *
 * Two instances of the same UModule must hold independent IC tables.
 * Build a minimal module with one nested proto whose ic_count = 3 and
 * ic_names = [foo, bar, baz]; spin up two instances and verify
 *   1. both creates succeed and return distinct pointers
 *   2. each instance has its own UProtoInstanceArr (distinct addresses)
 *   3. each instance's IC tables are distinct allocations
 *   4. the IC name pointers match across both instances (same UModule)
 *   5. every IC entry is zero-initialised (topology_gen[e] == 0 sentinel) */

UTEST(module_instance_basic_create) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UModule m = {0};
    UProto *p = umodule_alloc_nested_proto(&m);
    UASSERT(p != NULL);

    /* Pretend the emitter populated three IC sites. */
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    USymbol *bar = (USymbol *)ustr_intern(&vm, "bar", 3);
    USymbol *baz = (USymbol *)ustr_intern(&vm, "baz", 3);
    UASSERT(foo != NULL); UASSERT(bar != NULL); UASSERT(baz != NULL);

    p->ic_count = 3;
    p->ic_names = (USymbol **)malloc(3 * sizeof(USymbol *));
    UASSERT(p->ic_names != NULL);
    p->ic_names[0] = foo;
    p->ic_names[1] = bar;
    p->ic_names[2] = baz;

    UModuleInstance *mi = urbi_module_instance_create(&vm, &m);
    UASSERT(mi != NULL);
    UASSERT(mi->module == &m);
    UASSERT(mi->vm == &vm);
    UASSERT(mi->proto_instances != NULL);

    /* n = 1 (root chunk) + 1 (nested proto) = 2 */
    UASSERT_EQ((int)mi->proto_instances->n, 2);

    /* entries[0]: root chunk — sentinel (proto NULL, ic_table NULL). */
    UASSERT(mi->proto_instances->entries[0].proto    == NULL);
    UASSERT(mi->proto_instances->entries[0].ic_table == NULL);

    /* entries[1]: the nested proto with ic_count = 3 and a real ic_table. */
    UProtoInstance *pi = &mi->proto_instances->entries[1];
    UASSERT(pi->proto    == p);
    UASSERT(pi->ic_table != NULL);

    /* Each IC site has its name copied from the proto, n == 0 (no fills),
     * and every entry zero-initialised — topology_gen[e] == 0 is the
     * pre-M4 §3.1 "unfilled" sentinel. */
    UASSERT(pi->ic_table[0].name == foo);
    UASSERT(pi->ic_table[1].name == bar);
    UASSERT(pi->ic_table[2].name == baz);
    for (int k = 0; k < 3; k++) {
        UASSERT_EQ((int)pi->ic_table[k].n,              0);
        UASSERT_EQ((int)pi->ic_table[k].replace_cursor, 0);
        for (int e = 0; e < URBI_IC_ENTRIES_PER_SITE; e++) {
            UASSERT(pi->ic_table[k].recv_shapes[e] == NULL);
            UASSERT(pi->ic_table[k].slots[e]       == NULL);
            UASSERT(pi->ic_table[k].uprops[e]      == NULL);
            UASSERT_EQ((int)pi->ic_table[k].topology_gen[e], 0);
            UASSERT_EQ((int)pi->ic_table[k].flags[e],        0);
        }
    }

    urbi_module_instance_destroy(&vm, mi);
    umodule_destroy(&m);
    uvm_destroy(&vm);
}

UTEST(module_instance_two_instances_independent) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UModule m = {0};
    UProto *p = umodule_alloc_nested_proto(&m);
    UASSERT(p != NULL);

    USymbol *alpha = (USymbol *)ustr_intern(&vm, "alpha", 5);
    USymbol *beta  = (USymbol *)ustr_intern(&vm, "beta",  4);
    p->ic_count = 2;
    p->ic_names = (USymbol **)malloc(2 * sizeof(USymbol *));
    UASSERT(p->ic_names != NULL);
    p->ic_names[0] = alpha;
    p->ic_names[1] = beta;

    UModuleInstance *mi_a = urbi_module_instance_create(&vm, &m);
    UModuleInstance *mi_b = urbi_module_instance_create(&vm, &m);
    UASSERT(mi_a != NULL);
    UASSERT(mi_b != NULL);

    /* Distinct UModuleInstance cells. */
    UASSERT(mi_a != mi_b);

    /* Distinct UProtoInstanceArr bulk allocations. */
    UASSERT(mi_a->proto_instances != mi_b->proto_instances);

    /* Distinct ic_table allocations (different memory regions). */
    UProtoInstance *pi_a = &mi_a->proto_instances->entries[1];
    UProtoInstance *pi_b = &mi_b->proto_instances->entries[1];
    UASSERT(pi_a->ic_table != pi_b->ic_table);

    /* Same UModule means matching IC names by pointer identity (interned). */
    UASSERT(pi_a->ic_table[0].name == pi_b->ic_table[0].name);
    UASSERT(pi_a->ic_table[0].name == alpha);
    UASSERT(pi_a->ic_table[1].name == pi_b->ic_table[1].name);
    UASSERT(pi_a->ic_table[1].name == beta);

    /* Mutate one IC entry and confirm the other instance is untouched —
     * the load-bearing per-VM independence invariant.  Use n + replace_cursor
     * (scalar fields) so we don't need to allocate UShape / USlot here. */
    pi_a->ic_table[0].n              = 1;
    pi_a->ic_table[0].replace_cursor = 1;
    UASSERT_EQ((int)pi_b->ic_table[0].n,              0);
    UASSERT_EQ((int)pi_b->ic_table[0].replace_cursor, 0);

    urbi_module_instance_destroy(&vm, mi_b);
    urbi_module_instance_destroy(&vm, mi_a);
    umodule_destroy(&m);
    uvm_destroy(&vm);
}

UTEST(module_instance_zero_nested_protos) {
    /* Edge case: module with no nested protos.  proto_instances should
     * still be allocated with n = 1 (just the root-chunk sentinel) and
     * zero IC bytes trailing the entries[] array. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UModule m = {0};

    UModuleInstance *mi = urbi_module_instance_create(&vm, &m);
    UASSERT(mi != NULL);
    UASSERT(mi->proto_instances != NULL);
    UASSERT_EQ((int)mi->proto_instances->n, 1);
    UASSERT(mi->proto_instances->entries[0].proto    == NULL);
    UASSERT(mi->proto_instances->entries[0].ic_table == NULL);

    urbi_module_instance_destroy(&vm, mi);
    umodule_destroy(&m);
    uvm_destroy(&vm);
}

UTEST(module_instance_proto_with_zero_ic_count) {
    /* Edge case: nested proto whose ic_count == 0.  ic_table should be
     * NULL for that entry; bulk allocation accounts for zero IC bytes
     * from this proto. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UModule m = {0};
    UProto *p = umodule_alloc_nested_proto(&m);
    UASSERT(p != NULL);
    /* Leave p->ic_count = 0, p->ic_names = NULL (zero-init from
     * umodule_alloc_nested_proto). */

    UModuleInstance *mi = urbi_module_instance_create(&vm, &m);
    UASSERT(mi != NULL);
    UASSERT_EQ((int)mi->proto_instances->n, 2);
    UASSERT(mi->proto_instances->entries[1].proto    == p);
    UASSERT(mi->proto_instances->entries[1].ic_table == NULL);

    urbi_module_instance_destroy(&vm, mi);
    umodule_destroy(&m);
    uvm_destroy(&vm);
}

UTEST(module_instance_invalid_args_return_null) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    UModule m = {0};
    UASSERT(urbi_module_instance_create(NULL, &m) == NULL);
    UASSERT(urbi_module_instance_create(&vm, NULL) == NULL);
    uvm_destroy(&vm);
}

/* === T25: slow-path helpers ===
 *
 * These tests exercise urbi_slot_get_slow / urbi_slot_set_slow at the
 * library level — no VM dispatch required.  The full end-to-end OP_GETSLOT
 * / OP_SETSLOT story is exercised at T42 by the legacy `.chk` revival
 * fixtures (lookup, inheritance, slot-cow-const, shared-protos), so the
 * dispatch-loop integration is verified there rather than reimplemented
 * with synthetic UModule scaffolding here. */

UTEST(get_slow_resolves_via_proto_walk_and_fills_ic) {
    /* parent.bar = 123; child has parent as its proto.  child.bar must
     * resolve via the prototype walk; the IC entry must record child's
     * shape (not parent's) and clear FLAG_LOCAL (slot lives on parent). */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *parent = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *child  = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(parent != NULL); UASSERT(child != NULL);
    urbi_object_set_protos_single(&vm, child, parent);

    USymbol *bar = (USymbol *)ustr_intern(&vm, "bar", 3);
    UASSERT(bar != NULL);

    UValue v123;
    v123.kind = UVAL_INT;
    v123.v.i  = 123;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, parent, bar, v123), 0);

    UIC ic = {0};
    ic.name = bar;

    UValue out;
    UASSERT_EQ(urbi_slot_get_slow(&vm, child, &ic, &out), 0);
    UASSERT_EQ((int)out.v.i, 123);
    UASSERT_EQ((int)ic.n, 1);
    UASSERT(ic.recv_shapes[0] == child->shape);
    UASSERT_EQ((int)(ic.flags[0] & URBI_SLOT_FLAG_LOCAL), 0);
    UASSERT(ic.slots[0] == &parent->slots[0]);
    UASSERT_EQ((int)ic.replace_cursor, 1);

    uvm_destroy(&vm);
}

UTEST(get_slow_local_hit_sets_flag_local) {
    /* Receiver owns the slot directly — IC fill must record FLAG_LOCAL. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    UValue v9;
    v9.kind = UVAL_INT;
    v9.v.i  = 9;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v9), 0);

    UIC ic = {0};
    ic.name = foo;

    UValue out;
    UASSERT_EQ(urbi_slot_get_slow(&vm, o, &ic, &out), 0);
    UASSERT_EQ((int)out.v.i, 9);
    UASSERT_EQ((int)ic.n, 1);
    UASSERT(ic.recv_shapes[0] == o->shape);
    UASSERT(ic.flags[0] & URBI_SLOT_FLAG_LOCAL);
    UASSERT(ic.slots[0] == &o->slots[0]);

    uvm_destroy(&vm);
}

UTEST(get_slow_miss_returns_minus_one) {
    /* No slot named foo anywhere on the chain → urbi_slot_get_slow returns
     * -1 and does NOT fill the IC. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *missing = (USymbol *)ustr_intern(&vm, "missing", 7);

    UIC ic = {0};
    ic.name = missing;
    UValue out;
    UASSERT_EQ(urbi_slot_get_slow(&vm, o, &ic, &out), -1);
    UASSERT_EQ((int)ic.n, 0);

    uvm_destroy(&vm);
}

UTEST(set_slow_does_cow_when_resolution_via_proto_chain) {
    /* parent.bar = 0; child has parent as proto; set child.bar = 42.
     * After the set: child has its own local slot bar = 42; parent.bar
     * is unchanged at 0. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *parent = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *child  = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    urbi_object_set_protos_single(&vm, child, parent);

    USymbol *bar = (USymbol *)ustr_intern(&vm, "bar", 3);
    UValue v0;
    v0.kind = UVAL_INT;
    v0.v.i  = 0;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, parent, bar, v0), 0);

    /* Pre-condition: child has no local 'bar' yet. */
    UASSERT_EQ((int)urbi_shape_find_slot(child->shape, bar), -1);

    UIC ic = {0};
    ic.name = bar;

    UValue v42;
    v42.kind = UVAL_INT;
    v42.v.i  = 42;
    UASSERT_EQ(urbi_slot_set_slow(&vm, child, &ic, v42), 0);

    /* COW: child gained its own local 'bar' slot at index 0. */
    int32_t cidx = urbi_shape_find_slot(child->shape, bar);
    UASSERT(cidx >= 0);
    UASSERT_EQ((int)child->slots[cidx].v.i, 42);

    /* Parent's slot must still hold 0. */
    int32_t pidx = urbi_shape_find_slot(parent->shape, bar);
    UASSERT(pidx >= 0);
    UASSERT_EQ((int)parent->slots[pidx].v.i, 0);

    uvm_destroy(&vm);
}

UTEST(set_slow_local_hit_writes_in_place_and_fills_ic) {
    /* Receiver already owns the slot — write is in-place and the IC fills. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    UValue v1;
    v1.kind = UVAL_INT;
    v1.v.i  = 1;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v1), 0);
    UShape *shape_before = o->shape;
    USlot *slots_before  = o->slots;

    UIC ic = {0};
    ic.name = foo;

    UValue v55;
    v55.kind = UVAL_INT;
    v55.v.i  = 55;
    UASSERT_EQ(urbi_slot_set_slow(&vm, o, &ic, v55), 0);

    /* In-place: shape and slot wrapper unchanged. */
    UASSERT(o->shape == shape_before);
    UASSERT(o->slots == slots_before);
    UASSERT_EQ((int)o->slots[0].v.i, 55);

    /* IC filled with FLAG_LOCAL. */
    UASSERT_EQ((int)ic.n, 1);
    UASSERT(ic.flags[0] & URBI_SLOT_FLAG_LOCAL);
    UASSERT(ic.slots[0] == &o->slots[0]);

    uvm_destroy(&vm);
}

UTEST(set_slow_miss_installs_local_slot_on_receiver) {
    /* No slot anywhere on the chain → install on receiver via leaf-shape-
     * add.  No IC fill (subsequent miss-by-shape will re-resolve). */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *fresh = (USymbol *)ustr_intern(&vm, "fresh", 5);

    UIC ic = {0};
    ic.name = fresh;

    UValue v7;
    v7.kind = UVAL_INT;
    v7.v.i  = 7;
    UASSERT_EQ(urbi_slot_set_slow(&vm, o, &ic, v7), 0);

    int32_t idx = urbi_shape_find_slot(o->shape, fresh);
    UASSERT(idx >= 0);
    UASSERT_EQ((int)o->slots[idx].v.i, 7);
    UASSERT_EQ((int)ic.n, 0);   /* no fill on the miss-install path */

    uvm_destroy(&vm);
}

UTEST(slot_helpers_reject_invalid_args) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *n = (USymbol *)ustr_intern(&vm, "n", 1);
    UIC ic = {0};
    ic.name = n;
    UValue out;
    UValue v;
    v.kind = UVAL_NIL;

    UASSERT_EQ(urbi_slot_get_slow(NULL, o, &ic, &out), -1);
    UASSERT_EQ(urbi_slot_get_slow(&vm, NULL, &ic, &out), -1);
    UASSERT_EQ(urbi_slot_get_slow(&vm, o, NULL, &out), -1);
    UASSERT_EQ(urbi_slot_get_slow(&vm, o, &ic, NULL), -1);

    UASSERT_EQ(urbi_slot_set_slow(NULL, o, &ic, v), -1);
    UASSERT_EQ(urbi_slot_set_slow(&vm, NULL, &ic, v), -1);
    UASSERT_EQ(urbi_slot_set_slow(&vm, o, NULL, v), -1);

    /* ic with NULL name. */
    UIC ic_no_name = {0};
    UASSERT_EQ(urbi_slot_get_slow(&vm, o, &ic_no_name, &out), -1);
    UASSERT_EQ(urbi_slot_set_slow(&vm, o, &ic_no_name, v), -1);

    uvm_destroy(&vm);
}

UTEST(resolve_slot_finds_via_protos) {
    /* Direct test of urbi_object_resolve_slot — the shared helper used by
     * the slow paths and (later) USlotHandle. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *gp = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *p  = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *c  = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    urbi_object_set_protos_single(&vm, p, gp);
    urbi_object_set_protos_single(&vm, c, p);

    USymbol *deep = (USymbol *)ustr_intern(&vm, "deep", 4);
    UValue v77;
    v77.kind = UVAL_INT;
    v77.v.i  = 77;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, gp, deep, v77), 0);

    UObject *holder = NULL;
    uint32_t idx = 0u;
    UASSERT_EQ(urbi_object_resolve_slot(&vm, c, deep, &holder, &idx), 1);
    UASSERT(holder == gp);
    UASSERT_EQ((int)idx, 0);

    /* Miss case. */
    USymbol *gone = (USymbol *)ustr_intern(&vm, "gone", 4);
    UASSERT_EQ(urbi_object_resolve_slot(&vm, c, gone, &holder, &idx), 0);

    /* Bad-arg case. */
    UASSERT_EQ(urbi_object_resolve_slot(NULL, c, deep, &holder, &idx), -1);
    UASSERT_EQ(urbi_object_resolve_slot(&vm, NULL, deep, &holder, &idx), -1);

    uvm_destroy(&vm);
}

/* === T3 follow-up: entries[0] populated from UModule.ic_count ===
 *
 * Verifies that urbi_module_instance_create wires up entries[0].ic_table
 * from UModule.ic_count / ic_names (root-chunk IC sites added by T2). */

UTEST(module_instance_populates_root_chunk_ic_table) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UModule m = {0};

    /* Intern two symbols for the root-chunk IC sites. */
    USymbol *sx = (USymbol *)ustr_intern(&vm, "x", 1);
    USymbol *sy = (USymbol *)ustr_intern(&vm, "y", 1);
    UASSERT(sx != NULL); UASSERT(sy != NULL);

    /* Populate root-chunk IC fields directly (mimics what T2's emitter does). */
    m.ic_count = 2;
    USymbol *names[2];
    names[0] = sx;
    names[1] = sy;
    m.ic_names = names;

    UModuleInstance *mi = urbi_module_instance_create(&vm, &m);
    UASSERT(mi != NULL);

    /* entries[0] must carry a real ic_table (not NULL). */
    UASSERT(mi->proto_instances->entries[0].ic_table != NULL);

    /* Name pointers must match by identity (interned symbols). */
    UASSERT(mi->proto_instances->entries[0].ic_table[0].name == sx);
    UASSERT(mi->proto_instances->entries[0].ic_table[1].name == sy);

    /* Both sites must be zero-initialized. */
    UASSERT_EQ((int)mi->proto_instances->entries[0].ic_table[0].n, 0);
    UASSERT_EQ((int)mi->proto_instances->entries[0].ic_table[0].replace_cursor, 0);
    UASSERT_EQ((int)mi->proto_instances->entries[0].ic_table[1].n, 0);
    UASSERT_EQ((int)mi->proto_instances->entries[0].ic_table[1].replace_cursor, 0);

    urbi_module_instance_destroy(&vm, mi);
    /* Prevent umodule_destroy from freeing the static names[] array. */
    m.ic_names = NULL;
    m.ic_count = 0;
    umodule_destroy(&m);
    uvm_destroy(&vm);
}

/* === T30: cross-VM IC isolation + determinism-checksum extension ===
 *
 * Two complementary tests:
 *   1. Independent IC tables in two VMs — same UModule loaded into both
 *      yields distinct UProtoInstanceArr cells; mutating one IC entry
 *      does not bleed into the other (extends T16's same-VM independence
 *      check across the VM boundary).
 *   2. Determinism checksum incorporates IC state — manually filling an
 *      IC entry must change the checksum value on the next call.
 *
 * The second test only runs under URBI_DEBUG (the checksum function is
 * declared only in that build mode). */

UTEST(multi_vm_two_vms_have_independent_ic_tables) {
    UVM vm_a, vm_b;
    uvm_init(&vm_a, NULL, NULL);
    uvm_init(&vm_b, NULL, NULL);

    /* Same module shape — but each VM gets its own UModuleInstance.  The
     * IC tables (allocated via each VM's GC) must live in disjoint memory
     * regions so a fill in vm_a never bleeds into vm_b. */
    UModule m = {0};
    UProto *p = umodule_alloc_nested_proto(&m);
    UASSERT(p != NULL);

    USymbol *foo_a = (USymbol *)ustr_intern(&vm_a, "foo", 3);
    USymbol *foo_b = (USymbol *)ustr_intern(&vm_b, "foo", 3);
    p->ic_count = 1;
    p->ic_names = (USymbol **)malloc(sizeof(USymbol *));
    UASSERT(p->ic_names != NULL);
    /* Module's ic_names points at vm_a's interned symbol; vm_b's instance
     * inherits that pointer.  That's intentional — the load-bearing test
     * is per-VM IC mutation isolation, not interned-symbol isolation. */
    p->ic_names[0] = foo_a;

    UModuleInstance *mi_a = urbi_module_instance_create(&vm_a, &m);
    UModuleInstance *mi_b = urbi_module_instance_create(&vm_b, &m);
    UASSERT(mi_a != NULL); UASSERT(mi_b != NULL);

    /* Each VM's registry head points at its own instance. */
    UASSERT(vm_a.module_instances_head == mi_a);
    UASSERT(vm_b.module_instances_head == mi_b);
    UASSERT(mi_a->next_in_vm == NULL);
    UASSERT(mi_b->next_in_vm == NULL);

    /* Distinct UProtoInstanceArr blocks. */
    UASSERT(mi_a->proto_instances != mi_b->proto_instances);

    /* Distinct ic_table allocations. */
    UProtoInstance *pi_a = &mi_a->proto_instances->entries[1];
    UProtoInstance *pi_b = &mi_b->proto_instances->entries[1];
    UASSERT(pi_a->ic_table != pi_b->ic_table);

    /* Mutate IC[0] in vm_a; vm_b's IC[0] stays zero. */
    pi_a->ic_table[0].n              = 2;
    pi_a->ic_table[0].replace_cursor = 1;
    pi_a->ic_table[0].topology_gen[0] = 42u;

    UASSERT_EQ((int)pi_b->ic_table[0].n,              0);
    UASSERT_EQ((int)pi_b->ic_table[0].replace_cursor, 0);
    UASSERT_EQ((int)pi_b->ic_table[0].topology_gen[0], 0);

    /* Sanity: vm_b's interned `foo` is not the same pointer as vm_a's,
     * so per-VM intern tables are truly independent.  (Not a T30 invariant
     * per se but useful pin against future regressions.) */
    UASSERT(foo_a != foo_b);

    urbi_module_instance_destroy(&vm_b, mi_b);
    urbi_module_instance_destroy(&vm_a, mi_a);
    umodule_destroy(&m);
    uvm_destroy(&vm_b);
    uvm_destroy(&vm_a);
}

/* === T5: urbi_get_or_create_module_instance cache helper ===
 *
 * Same (vm, module) pair must return the same UModuleInstance on repeated
 * calls; a different module must yield a different instance. */

UTEST(get_or_create_module_instance_caches_per_module) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UModule m1 = {0};
    UModule m2 = {0};

    UModuleInstance *a1 = urbi_get_or_create_module_instance(&vm, &m1);
    UModuleInstance *a2 = urbi_get_or_create_module_instance(&vm, &m1);
    UModuleInstance *b1 = urbi_get_or_create_module_instance(&vm, &m2);

    UASSERT(a1 != NULL);
    UASSERT(a1 == a2);   /* same module → same instance */
    UASSERT(a1 != b1);   /* different module → different instance */

    umodule_destroy(&m1);
    umodule_destroy(&m2);
    uvm_destroy(&vm);
}

/* Same module loaded into two different VMs must produce distinct
 * UModuleInstances, each threaded onto its own vm->module_instances_head. */
UTEST(get_or_create_module_instance_isolated_per_vm) {
    UVM vm_a, vm_b;
    uvm_init(&vm_a, NULL, NULL);
    uvm_init(&vm_b, NULL, NULL);

    UModule m = {0};

    UModuleInstance *mi_a = urbi_get_or_create_module_instance(&vm_a, &m);
    UModuleInstance *mi_b = urbi_get_or_create_module_instance(&vm_b, &m);

    UASSERT(mi_a != NULL);
    UASSERT(mi_b != NULL);
    UASSERT(mi_a != mi_b);  /* distinct instances per VM */

    /* Each instance is reachable from its own VM's registry head. */
    UASSERT(vm_a.module_instances_head == mi_a);
    UASSERT(vm_b.module_instances_head == mi_b);

    umodule_destroy(&m);
    uvm_destroy(&vm_b);
    uvm_destroy(&vm_a);
}

#ifdef URBI_DEBUG
UTEST(determinism_checksum_includes_ic_state) {
    /* Snapshot checksum, manually fill an IC entry, snapshot again — the
     * two checksums must differ.  Tests the §6 fold step in
     * urbi_get_determinism_checksum. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UModule m = {0};
    UProto *p = umodule_alloc_nested_proto(&m);
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    p->ic_count = 1;
    p->ic_names = (USymbol **)malloc(sizeof(USymbol *));
    p->ic_names[0] = foo;

    UModuleInstance *mi = urbi_module_instance_create(&vm, &m);
    UASSERT(mi != NULL);

    uint64_t h_before = urbi_get_determinism_checksum(&vm);

    /* Fill IC[0] entry 0 by hand. */
    UProtoInstance *pi = &mi->proto_instances->entries[1];
    pi->ic_table[0].n               = 1;
    pi->ic_table[0].replace_cursor  = 1;
    pi->ic_table[0].topology_gen[0] = 7u;

    uint64_t h_after = urbi_get_determinism_checksum(&vm);

    UASSERT(h_before != h_after);

    urbi_module_instance_destroy(&vm, mi);
    umodule_destroy(&m);
    uvm_destroy(&vm);
}

UTEST(determinism_checksum_stable_with_no_module_instances) {
    /* Two consecutive checksum reads on a VM with no UModuleInstance must
     * agree (the per-IC fold is a no-op when the registry head is NULL). */
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    uint64_t h1 = urbi_get_determinism_checksum(&vm);
    uint64_t h2 = urbi_get_determinism_checksum(&vm);
    UASSERT(h1 == h2);
    uvm_destroy(&vm);
}
#endif  /* URBI_DEBUG */

/* === T6: urbi_run_chunk binds UModuleInstance on first run ===
 *
 * Verifies that urbi_run_chunk populates vm->module_instances_head for the
 * module it runs, so downstream OP_GETSLOT/SETSLOT can find the IC table. */

UTEST(urbi_run_chunk_creates_module_instance_on_first_run) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* Compile a trivial source ("var x = 1;") into a fresh module. */
    UModule m = {0};
    UArena arena;
    uarena_init(&arena, 4096);

    const char *src = "var x = 1;";
    size_t src_len = sizeof("var x = 1;") - 1u;
    ULexer lex;
    ulex_init(&lex, src, src_len);

    UEmitter e;
    uemit_init(&e, &m, &arena, &vm, NULL);

    UParser p;
    uparse_init(&p, &lex, &arena);

    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) break;
        if (uemit_statement(&e, node) != EMIT_OK) break;
        uarena_reset(&arena);
    }
    UASSERT_EQ((int)uemit_finish(&e), (int)EMIT_OK);

    /* Pre-condition: no module instance exists yet. */
    UASSERT(vm.module_instances_head == NULL);

    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UValue out = {0};
    int rc = urbi_run_chunk(&vm, r, &m, &out);
    UASSERT_EQ(rc, (int)URBI_OK);

    /* Post-condition: module_instances_head is populated and points to our module. */
    UASSERT(vm.module_instances_head != NULL);
    UASSERT(vm.module_instances_head->module == &m);

    umodule_destroy(&m);
    uarena_destroy(&arena);
    uvm_destroy(&vm);
}

void test_uic_suite(void) {
    utest_run("uic: layout at default 4 entries",
              uic_layout_at_default_4_entries);
    utest_run("uic: flag bits distinct",
              uic_flag_bits_distinct);
    utest_run("uic: module instance basic create",
              module_instance_basic_create);
    utest_run("uic: module instance two instances are independent",
              module_instance_two_instances_independent);
    utest_run("uic: module instance with zero nested protos",
              module_instance_zero_nested_protos);
    utest_run("uic: module instance with proto.ic_count == 0",
              module_instance_proto_with_zero_ic_count);
    utest_run("uic: module instance invalid args return NULL",
              module_instance_invalid_args_return_null);
    utest_run("uic: get_slow resolves via proto walk and fills IC",
              get_slow_resolves_via_proto_walk_and_fills_ic);
    utest_run("uic: get_slow local hit sets FLAG_LOCAL",
              get_slow_local_hit_sets_flag_local);
    utest_run("uic: get_slow miss returns -1 (no IC fill)",
              get_slow_miss_returns_minus_one);
    utest_run("uic: set_slow does COW when resolution via proto chain",
              set_slow_does_cow_when_resolution_via_proto_chain);
    utest_run("uic: set_slow local hit writes in place and fills IC",
              set_slow_local_hit_writes_in_place_and_fills_ic);
    utest_run("uic: set_slow miss installs local slot on receiver",
              set_slow_miss_installs_local_slot_on_receiver);
    utest_run("uic: slot helpers reject invalid args",
              slot_helpers_reject_invalid_args);
    utest_run("uic: resolve_slot finds via protos",
              resolve_slot_finds_via_protos);
    utest_run("uic: two VMs have independent IC tables",
              multi_vm_two_vms_have_independent_ic_tables);
    utest_run("uic: module instance populates root chunk IC table",
              module_instance_populates_root_chunk_ic_table);
    utest_run("uic: get_or_create_module_instance caches per module",
              get_or_create_module_instance_caches_per_module);
    utest_run("uic: get_or_create_module_instance isolated per VM",
              get_or_create_module_instance_isolated_per_vm);
    utest_run("uic: urbi_run_chunk creates module instance on first run",
              urbi_run_chunk_creates_module_instance_on_first_run);
#ifdef URBI_DEBUG
    utest_run("uic: determinism checksum includes IC state",
              determinism_checksum_includes_ic_state);
    utest_run("uic: determinism checksum stable with no module instances",
              determinism_checksum_stable_with_no_module_instances);
#endif
}
