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
#include "urbi/gc.h"       /* urbi_gc_alloc — T9 heap UProtos */
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

    /* Single-tag prototype encoding per spec §4.1 (the canonical form
     * decoded by UPROTOS_FOREACH, T9).
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

/* === T9: UPROTOS_FOREACH dispatches across all three storage forms ===
 *
 * Per pre-M4 prototype-chain spec §4.1: UObject.protos has three forms —
 * empty (0), single ((p<<1)|1), heap (UProtos*).  These tests exercise
 * each form through UPROTOS_FOREACH plus the count/at convenience inlines. */

UTEST(uobject_protos_foreach_empty_form_yields_nothing) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_root(&vm);   /* root has empty-form protos (0) */
    UASSERT(o != NULL);
    UASSERT_EQ((int)o->protos, 0);

    int seen = 0;
    UObject *p;
    UPROTOS_FOREACH(o, p) {
        (void)p;
        seen++;
    }
    UASSERT_EQ(seen, 0);

    /* Convenience inlines agree. */
    UASSERT_EQ((int)urbi_object_proto_count(o), 0);
    UASSERT(urbi_object_proto_at(o, 0u) == NULL);

    uvm_destroy(&vm);
}

UTEST(uobject_protos_foreach_single_form_yields_one) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* Integer atom uses the single-tag form ((root << 1) | 1). */
    UObject *integer = urbi_object_atom(&vm, URBI_ATOM_INTEGER_F);
    UObject *root    = vm.atom_object;
    UASSERT(integer != NULL);
    UASSERT(root != NULL);
    UASSERT((integer->protos & 1u) == 1u);   /* sanity: single form */

    int seen = 0;
    UObject *p   = NULL;
    UObject *got = NULL;
    UPROTOS_FOREACH(integer, p) {
        if (seen == 0) got = p;
        seen++;
    }
    UASSERT_EQ(seen, 1);
    UASSERT(got == root);

    /* Convenience inlines agree. */
    UASSERT_EQ((int)urbi_object_proto_count(integer), 1);
    UASSERT(urbi_object_proto_at(integer, 0u) == root);
    UASSERT(urbi_object_proto_at(integer, 1u) == NULL);

    uvm_destroy(&vm);
}

UTEST(uobject_protos_foreach_heap_form_yields_all) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *root = urbi_object_root(&vm);
    UObject *a    = urbi_object_atom(&vm, URBI_ATOM_INTEGER_F);
    UObject *b    = urbi_object_atom(&vm, URBI_ATOM_FLOAT_F);
    UObject *c    = urbi_object_atom(&vm, URBI_ATOM_STRING_F);
    UASSERT(root != NULL); UASSERT(a != NULL); UASSERT(b != NULL); UASSERT(c != NULL);

    /* Heap form: allocate a UProtos block with three entries.  Bit 0 of
     * the raw pointer must be clear (heap pointers are 8-byte aligned by
     * the GC allocator), which is what UPROTOS_FOREACH checks to dispatch. */
    UProtos *up = (UProtos *)urbi_gc_alloc(
            &vm, sizeof(UProtos) + 3u * sizeof(UObject *), UTYPE_PROTOS);
    UASSERT(up != NULL);
    UASSERT(((uintptr_t)up & 1u) == 0u);   /* alignment sanity */
    up->n        = 3u;
    up->items[0] = a;
    up->items[1] = b;
    up->items[2] = c;

    /* Splice the heap UProtos into root->protos so iteration sees the
     * heap form (provisional direct write — T11 lands the formal mutator). */
    root->protos = (uintptr_t)up;

    int seen = 0;
    UObject *p;
    UObject *visited[3] = { NULL, NULL, NULL };
    UPROTOS_FOREACH(root, p) {
        if (seen < 3) visited[seen] = p;
        seen++;
    }
    UASSERT_EQ(seen, 3);
    UASSERT(visited[0] == a);
    UASSERT(visited[1] == b);
    UASSERT(visited[2] == c);

    /* Convenience inlines agree. */
    UASSERT_EQ((int)urbi_object_proto_count(root), 3);
    UASSERT(urbi_object_proto_at(root, 0u) == a);
    UASSERT(urbi_object_proto_at(root, 1u) == b);
    UASSERT(urbi_object_proto_at(root, 2u) == c);
    UASSERT(urbi_object_proto_at(root, 3u) == NULL);

    /* Restore empty form before destroy so any GC walker re-entry stays
     * within the well-defined empty case. */
    root->protos = 0u;

    uvm_destroy(&vm);
}

/* === T10: prototype-mutation primitives ===
 *
 * Per pre-M4 prototype-chain spec §5.1-§5.4: every chain mutation routes
 * through urbi_object_set_protos_{empty,single,heap}.  Each primitive must
 *   (1) shade existing protos via the forward Dijkstra barrier,
 *   (2) shade the inserted child(ren),
 *   (3) bump vm->topology_gen.
 * The five transitions exercised below cover all storage-form pairs the
 * higher-level mutators (T11) can produce.
 *
 * vm->topology_gen is initialised to 1 at uvm_init (per pre-M4 topology
 * spec §3.1, reserves 0 as the IC-unfilled sentinel).  Each primitive call
 * bumps by exactly 1, so we sample pre/post around the call under test
 * after all the setup allocations have happened. */

/* Helper: allocate a UProtos block of size n via the GC, populate items[]
 * from src.  Caller wraps with urbi_object_set_protos_heap. */
static UProtos *
make_uprotos(UVM *vm, UObject **src, uint32_t n) {
    UProtos *up = (UProtos *)urbi_gc_alloc(
            vm, sizeof(UProtos) + n * sizeof(UObject *), UTYPE_PROTOS);
    if (up == NULL) return NULL;
    up->n = n;
    for (uint32_t i = 0; i < n; i++) {
        up->items[i] = src[i];
    }
    return up;
}

UTEST(uobject_set_protos_empty_to_single_bumps_topology) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *p = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL); UASSERT(p != NULL);
    UASSERT_EQ((int)urbi_object_proto_count(o), 0);     /* sanity: empty form */
    UASSERT_EQ((int)o->protos, 0);

    uint64_t pre = vm.topology_gen;
    urbi_object_set_protos_single(&vm, o, p);

    /* Single form encoded; one proto reachable; topology bumped exactly once. */
    UASSERT((o->protos & 1u) == 1u);
    UASSERT_EQ((int)urbi_object_proto_count(o), 1);
    UASSERT(urbi_object_proto_at(o, 0u) == p);
    UASSERT(vm.topology_gen == pre + 1u);

    uvm_destroy(&vm);
}

UTEST(uobject_set_protos_single_to_heap_bumps_topology) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *a = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *b = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *c = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o && a && b && c);

    /* First transition o into single form so the next mutation exercises
     * single → heap. */
    urbi_object_set_protos_single(&vm, o, a);
    UASSERT_EQ((int)urbi_object_proto_count(o), 1);

    UObject *items[3] = { a, b, c };
    UProtos *up = make_uprotos(&vm, items, 3u);
    UASSERT(up != NULL);

    uint64_t pre = vm.topology_gen;
    urbi_object_set_protos_heap(&vm, o, up);

    /* Heap form: bit 0 clear, raw UProtos*; three protos visible; bumped. */
    UASSERT_EQ((int)(o->protos & 1u), 0);
    UASSERT((UProtos *)o->protos == up);
    UASSERT_EQ((int)urbi_object_proto_count(o), 3);
    UASSERT(urbi_object_proto_at(o, 0u) == a);
    UASSERT(urbi_object_proto_at(o, 1u) == b);
    UASSERT(urbi_object_proto_at(o, 2u) == c);
    UASSERT(vm.topology_gen == pre + 1u);

    uvm_destroy(&vm);
}

UTEST(uobject_set_protos_heap_to_fresh_heap_bumps_topology) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *a = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *b = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *c = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *d = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o && a && b && c && d);

    /* Set up heap form first. */
    UObject *items_old[2] = { a, b };
    UProtos *up_old = make_uprotos(&vm, items_old, 2u);
    UASSERT(up_old != NULL);
    urbi_object_set_protos_heap(&vm, o, up_old);
    UASSERT_EQ((int)urbi_object_proto_count(o), 2);

    /* Replace with a fresh, larger heap UProtos. */
    UObject *items_new[3] = { c, d, a };
    UProtos *up_new = make_uprotos(&vm, items_new, 3u);
    UASSERT(up_new != NULL);
    UASSERT(up_new != up_old);

    uint64_t pre = vm.topology_gen;
    urbi_object_set_protos_heap(&vm, o, up_new);

    /* New UProtos installed; old one shaded by barrier (not validated here —
     * the GC's bookkeeping owns that signal); count + items reflect new set. */
    UASSERT((UProtos *)o->protos == up_new);
    UASSERT_EQ((int)urbi_object_proto_count(o), 3);
    UASSERT(urbi_object_proto_at(o, 0u) == c);
    UASSERT(urbi_object_proto_at(o, 1u) == d);
    UASSERT(urbi_object_proto_at(o, 2u) == a);
    UASSERT(vm.topology_gen == pre + 1u);

    uvm_destroy(&vm);
}

UTEST(uobject_set_protos_heap_to_single_collapse_bumps_topology) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *a = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *b = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o && a && b);

    UObject *items[2] = { a, b };
    UProtos *up = make_uprotos(&vm, items, 2u);
    UASSERT(up != NULL);
    urbi_object_set_protos_heap(&vm, o, up);
    UASSERT_EQ((int)urbi_object_proto_count(o), 2);
    UASSERT_EQ((int)(o->protos & 1u), 0);             /* heap form */

    uint64_t pre = vm.topology_gen;
    urbi_object_set_protos_single(&vm, o, a);

    /* Collapsed to single form; storage-form transition observed; bumped. */
    UASSERT((o->protos & 1u) == 1u);
    UASSERT_EQ((int)urbi_object_proto_count(o), 1);
    UASSERT(urbi_object_proto_at(o, 0u) == a);
    UASSERT(vm.topology_gen == pre + 1u);

    uvm_destroy(&vm);
}

UTEST(uobject_set_protos_single_to_empty_collapse_bumps_topology) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *p = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o && p);

    urbi_object_set_protos_single(&vm, o, p);
    UASSERT_EQ((int)urbi_object_proto_count(o), 1);

    uint64_t pre = vm.topology_gen;
    urbi_object_set_protos_empty(&vm, o);

    /* Empty form (raw 0); zero protos; bumped. */
    UASSERT_EQ((int)o->protos, 0);
    UASSERT_EQ((int)urbi_object_proto_count(o), 0);
    UASSERT(vm.topology_gen == pre + 1u);

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
    utest_run("uobject: protos foreach empty form yields nothing",
              uobject_protos_foreach_empty_form_yields_nothing);
    utest_run("uobject: protos foreach single form yields one",
              uobject_protos_foreach_single_form_yields_one);
    utest_run("uobject: protos foreach heap form yields all",
              uobject_protos_foreach_heap_form_yields_all);
    utest_run("uobject: set_protos empty -> single bumps topology",
              uobject_set_protos_empty_to_single_bumps_topology);
    utest_run("uobject: set_protos single -> heap bumps topology",
              uobject_set_protos_single_to_heap_bumps_topology);
    utest_run("uobject: set_protos heap -> fresh-heap bumps topology",
              uobject_set_protos_heap_to_fresh_heap_bumps_topology);
    utest_run("uobject: set_protos heap -> single (collapse) bumps topology",
              uobject_set_protos_heap_to_single_collapse_bumps_topology);
    utest_run("uobject: set_protos single -> empty (collapse) bumps topology",
              uobject_set_protos_single_to_empty_collapse_bumps_topology);
}
