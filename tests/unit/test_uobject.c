/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for src/object/uobject.h — UObject/UProtos/USlot layouts.
 *
 * The header itself carries _Static_assert pins on UObject and USlot widths;
 * if those trip, this file won't compile.  These runtime tests give a second,
 * test-runner-visible signal that the layout is what the spec says it is. */

#include "utest.h"

#include "object/uobject.h"
#include "object/ushape.h"  /* UShape — T26 set_local_slot tests */
#include "umodule.h"   /* UValue */
#include "value/uintern.h"   /* ustr_intern — T26 set_local_slot tests */
#include "vm/uvm.h"
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

UTEST(uobject_header_is_56_bytes) {
    /* UObject grew 48 → 56 B at M5 spec #4 §3.1 (changed_events_head added). */
    UASSERT_EQ((int)sizeof(UObject), 56);
}

/* === UObject field offsets ===
 * Field order matters: layout must be exactly cell -> shape -> slots ->
 * protos -> object_id -> lookup_stamp -> flags -> reserved ->
 * changed_events_head. */

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
    /* changed_events_head at 48 (spec #4 §3.1). */
    UASSERT_EQ((int)offsetof(UObject, changed_events_head), 48);
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

/* === T11: NULL-arg defensive contracts on the public mutators ===
 *
 * Replaces the T8 stub-return test once T11 lands real implementations.
 * The five behavioural T11 contract tests live further down. */

UTEST(uobject_proto_mutators_reject_null_args) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_root(&vm);
    UASSERT(o != NULL);

    /* NULL vm / obj / proto rejected on add+remove. */
    UASSERT_EQ((int)urbi_object_add_proto   (NULL, o, o),    (int)URBI_ERR_INVALID_ARG);
    UASSERT_EQ((int)urbi_object_add_proto   (&vm, NULL, o),  (int)URBI_ERR_INVALID_ARG);
    UASSERT_EQ((int)urbi_object_add_proto   (&vm, o, NULL),  (int)URBI_ERR_INVALID_ARG);
    UASSERT_EQ((int)urbi_object_remove_proto(NULL, o, o),    (int)URBI_ERR_INVALID_ARG);
    UASSERT_EQ((int)urbi_object_remove_proto(&vm, NULL, o),  (int)URBI_ERR_INVALID_ARG);
    UASSERT_EQ((int)urbi_object_remove_proto(&vm, o, NULL),  (int)URBI_ERR_INVALID_ARG);

    /* set_protos: NULL list with n>0 rejected; NULL list with n==0 is the
     * "clear all protos" form and is valid. */
    UASSERT_EQ((int)urbi_object_set_protos(NULL, o, NULL, 0u), (int)URBI_ERR_INVALID_ARG);
    UASSERT_EQ((int)urbi_object_set_protos(&vm, NULL, NULL, 0u), (int)URBI_ERR_INVALID_ARG);
    UObject *junk[1] = { NULL };
    UASSERT_EQ((int)urbi_object_set_protos(&vm, o, NULL, 1u), (int)URBI_ERR_INVALID_ARG);
    /* NULL entries inside a non-NULL list are also rejected. */
    UASSERT_EQ((int)urbi_object_set_protos(&vm, o, junk, 1u), (int)URBI_ERR_INVALID_ARG);

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

/* === T11: behavioural contracts on add_proto / remove_proto / set_protos ===
 *
 * Per pre-M2 §5.1-§5.3 and pre-M4 prototype-chain spec §5.5:
 *   - add_proto prepends at MRO position 0
 *   - remove_proto on absent target is a silent no-op
 *   - set_protos dedups first-occurrence-wins
 *   - cross-atom-family inheritance is rejected
 *   - set_protos validates atomically — no partial state on failure */

UTEST(uobject_add_proto_prepends_position_zero) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *a = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *b = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o && a && b);

    /* Empty -> add b -> [b]; add a -> [a, b]. */
    UASSERT_EQ((int)urbi_object_add_proto(&vm, o, b), (int)URBI_OK);
    UASSERT_EQ((int)urbi_object_proto_count(o), 1);
    UASSERT(urbi_object_proto_at(o, 0u) == b);

    UASSERT_EQ((int)urbi_object_add_proto(&vm, o, a), (int)URBI_OK);
    UASSERT_EQ((int)urbi_object_proto_count(o), 2);
    UASSERT(urbi_object_proto_at(o, 0u) == a);   /* prepended */
    UASSERT(urbi_object_proto_at(o, 1u) == b);

    uvm_destroy(&vm);
}

UTEST(uobject_remove_absent_proto_is_silent_noop) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o       = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *present = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *absent  = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o && present && absent);

    UASSERT_EQ((int)urbi_object_add_proto(&vm, o, present), (int)URBI_OK);
    UASSERT_EQ((int)urbi_object_proto_count(o), 1);

    /* Removing a proto that was never added returns URBI_OK and does not
     * change the count (legacy semantics per pre-M2 §5.2). */
    UASSERT_EQ((int)urbi_object_remove_proto(&vm, o, absent), (int)URBI_OK);
    UASSERT_EQ((int)urbi_object_proto_count(o), 1);
    UASSERT(urbi_object_proto_at(o, 0u) == present);

    uvm_destroy(&vm);
}

UTEST(uobject_set_protos_dedups_first_occurrence_wins) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *a = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *b = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o && a && b);

    UObject *list[4] = { a, b, a, b };
    UASSERT_EQ((int)urbi_object_set_protos(&vm, o, list, 4u), (int)URBI_OK);

    /* First-occurrence-wins → [a, b]; trailing repeats discarded. */
    UASSERT_EQ((int)urbi_object_proto_count(o), 2);
    UASSERT(urbi_object_proto_at(o, 0u) == a);
    UASSERT(urbi_object_proto_at(o, 1u) == b);

    uvm_destroy(&vm);
}

UTEST(uobject_valid_proto_rejects_cross_atom_family) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *integer = urbi_object_atom(&vm, URBI_ATOM_INTEGER_F);
    UObject *str     = urbi_object_atom(&vm, URBI_ATOM_STRING_F);
    UASSERT(integer && str);

    uint32_t before = urbi_object_proto_count(integer);

    /* Integer.add_proto(String) — cross-family, neither is root Object. */
    UASSERT_EQ((int)urbi_object_add_proto(&vm, integer, str),
               (int)URBI_ERR_INVALID_ARG);

    /* No partial state: Integer's proto count is unchanged. */
    UASSERT_EQ((int)urbi_object_proto_count(integer), (int)before);

    uvm_destroy(&vm);
}

UTEST(uobject_set_protos_aborts_on_invalid_proto_no_partial_state) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *root    = urbi_object_root(&vm);
    UObject *integer = urbi_object_atom(&vm, URBI_ATOM_INTEGER_F);
    UObject *str     = urbi_object_atom(&vm, URBI_ATOM_STRING_F);
    UASSERT(root && integer && str);

    /* Snapshot Integer's pre-call proto state. */
    uint32_t  before_n = urbi_object_proto_count(integer);
    uintptr_t before_p = integer->protos;
    uint64_t  before_g = vm.topology_gen;

    /* set_protos with [root, str] on Integer: root is fine, but str fails
     * the cross-family check.  Atomic — fails before any mutation. */
    UObject *list[2] = { root, str };
    UASSERT_EQ((int)urbi_object_set_protos(&vm, integer, list, 2u),
               (int)URBI_ERR_INVALID_ARG);

    /* No partial state: count, raw protos word, and topology_gen all
     * unchanged. */
    UASSERT_EQ((int)urbi_object_proto_count(integer), (int)before_n);
    UASSERT(integer->protos == before_p);
    UASSERT(vm.topology_gen == before_g);

    uvm_destroy(&vm);
}

/* === T12: cycle-safe DFS lookup ===
 *
 * Per pre-M4 prototype-chain spec §6 + GETSLOT/SETSLOT spec §6.5.
 *
 * Three test bodies in scope at T12:
 *   - cycle safety: a→b→a graph terminates without infinite recursion
 *   - lookup_id pre-bump on each top-level call leaves stamps fresh
 *   - rollover: low-32 wrap of vm->lookup_id triggers force_wrap that
 *     clears every UObject.lookup_stamp and resets lookup_id to 1
 *
 * The "found local slot" + "DFS prefers leftmost" tests defer to T13/T26
 * — urbi_shape_find_slot is stubbed to return -1 here, so no slot lookup
 * succeeds, and the DFS-ordering signal is not observable through the
 * public API at this commit. */

UTEST(uobject_lookup_safe_under_cycle) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* Build a→b→a cycle in the proto graph.  Atom OBJECT permits any
     * inheritance (valid_proto accepts root-Object on either side), so
     * urbi_object_add_proto won't reject the second set_protos.
     *
     * Use the T10 primitive directly to bypass any v1.x cycle-check that
     * might land at the public mutator surface — T12's contract is that
     * the lookup primitive itself tolerates cycles. */
    UObject *a = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *b = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(a && b);

    urbi_object_set_protos_single(&vm, a, b);
    urbi_object_set_protos_single(&vm, b, a);

    /* Sanity: the cycle is wired. */
    UASSERT(urbi_object_proto_at(a, 0u) == b);
    UASSERT(urbi_object_proto_at(b, 0u) == a);

    /* Lookup of any name on a returns -1 (miss) without infinite recursion.
     * The cycle guard fires on lookup_stamp the second time we reach a. */
    UValue out;
    out.kind = UVAL_NIL;
    int rc = urbi_object_lookup(&vm, a, /*name*/ NULL, &out);
    UASSERT_EQ(rc, -1);

    /* Both a and b were stamped exactly once with the new lookup_id. */
    UASSERT_EQ((int)a->lookup_stamp, (int)(uint32_t)vm.lookup_id);
    UASSERT_EQ((int)b->lookup_stamp, (int)(uint32_t)vm.lookup_id);

    uvm_destroy(&vm);
}

UTEST(uobject_lookup_pre_bumps_lookup_id_each_call) {
    /* Each top-level urbi_object_lookup call bumps vm->lookup_id under
     * non-rollover.  Pre-T40 the bump count was exactly 1 per call; T40
     * adds the GET_FALLBACK retry, so a miss now bumps twice (once for
     * the original DFS, once for the fallback DFS).  Two consecutive
     * miss-calls on a freshly allocated object therefore leave
     * vm->lookup_id == initial + 4. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    UASSERT_EQ((int)vm.lookup_id, 1);   /* uvm_init invariant per pre-M4 spec */

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);

    UValue out;
    out.kind = UVAL_NIL;
    UASSERT_EQ(urbi_object_lookup(&vm, o, NULL, &out), -1);
    UASSERT_EQ((int)vm.lookup_id, 3);   /* +1 for original, +1 for fallback retry (T40) */

    UASSERT_EQ(urbi_object_lookup(&vm, o, NULL, &out), -1);
    UASSERT_EQ((int)vm.lookup_id, 5);

    /* The single object's stamp tracks the most recent stamp it received,
     * which is the fallback-retry pass at the end of the second call. */
    UASSERT_EQ((int)o->lookup_stamp, 5);

    uvm_destroy(&vm);
}

UTEST(uobject_lookup_id_rollover_clears_stamps) {
    /* Per pre-M4 prototype-chain spec §7.2: when the low 32 bits of
     * vm->lookup_id would wrap to 0, urbi_object_lookup_id_force_wrap
     * runs an immediate clear-pass over every UObject and resets
     * vm->lookup_id back to 1.
     *
     * Triggered transparently from inside urbi_object_lookup when
     * (uint32_t)(lookup_id + 1) == 0, i.e. when lookup_id reaches a
     * value whose low 32 bits are UINT32_MAX.
     *
     * T40 wrinkle: each top-level lookup call now bumps lookup_id twice
     * on a miss (original DFS + fallback retry DFS).  The wrap protocol
     * still gates each individual bump, so an internal rollover during
     * the fallback retry triggers force_wrap mid-call.  This test sets
     * lookup_id explicitly to UINT32_MAX to drive the wrap on entry to
     * a call (rather than mid-call), keeping the assertion shape simple. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o1 = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *o2 = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o1 && o2);

    /* Stamp both cells with a non-zero stamp under the current lookup_id. */
    UValue out;
    out.kind = UVAL_NIL;
    UASSERT_EQ(urbi_object_lookup(&vm, o1, NULL, &out), -1);
    UASSERT_EQ(urbi_object_lookup(&vm, o2, NULL, &out), -1);
    UASSERT(o1->lookup_stamp != 0u);
    UASSERT(o2->lookup_stamp != 0u);

    /* Drive lookup_id to UINT32_MAX so the very next bump tries to write
     * (uint32_t)0 — i.e. force_wrap fires on entry to the next call. */
    vm.lookup_id = (uint64_t)UINT32_MAX;

    /* On entry: (UINT32_MAX + 1) low-32 == 0, force_wrap runs, lookup_id=1,
     * all stamps cleared.  Inner lookup(o1, NULL) misses, stamps o1=1.
     * Then T40 fallback retry: bump checks (1+1)=2 (no rollover), bump to 2,
     * inner lookup(o1, "fallback") misses, re-stamps o1=2.  Returns -1. */
    UASSERT_EQ(urbi_object_lookup(&vm, o1, NULL, &out), -1);
    UASSERT_EQ((int64_t)vm.lookup_id, 2);
    UASSERT_EQ((int)o1->lookup_stamp, 2);
    /* o2 was cleared by the wrap pass and never re-stamped. */
    UASSERT_EQ((int)o2->lookup_stamp, 0);

    uvm_destroy(&vm);
}

UTEST(uobject_lookup_id_force_wrap_clears_all_object_stamps) {
    /* Direct test of urbi_object_lookup_id_force_wrap: every UObject's
     * lookup_stamp returns to 0, lookup_id resets to 1. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o1 = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *o2 = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *o3 = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o1 && o2 && o3);

    /* Manually stamp them to exercise the clear pass. */
    o1->lookup_stamp = 0xDEADBEEFu;
    o2->lookup_stamp = 0xCAFEBABEu;
    o3->lookup_stamp = 0x12345678u;

    vm.lookup_id = 0xFEEDFACEull;

    urbi_object_lookup_id_force_wrap(&vm);

    UASSERT_EQ((int)o1->lookup_stamp, 0);
    UASSERT_EQ((int)o2->lookup_stamp, 0);
    UASSERT_EQ((int)o3->lookup_stamp, 0);
    UASSERT_EQ((int)vm.lookup_id, 1);

    uvm_destroy(&vm);
}

/* === T26: urbi_object_set_local_slot ===
 *
 * Per pre-M2 §6.1 + pre-M4 topology-generation spec §4.2 row 2.  Two
 * behavioural tests:
 *   - Adding a fresh slot grows the slot array via shape transition.
 *   - Re-setting an existing slot is in-place value update (no growth). */

UTEST(uobject_set_local_slot_grows_slots_array_and_transitions_shape) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    UASSERT_EQ((int)o->shape->count, 0);
    UASSERT(o->slots == NULL);

    /* USymbol is opaque (forward-decl in umodule.h); intern returns
     * const char* and we cast to USymbol* — same pattern as test_ushape.c
     * and test_funcstate.c. */
    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    UASSERT(foo != NULL);

    UValue v7;
    v7.kind = UVAL_INT;
    v7.v.i  = 7;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v7), 0);

    UASSERT_EQ((int)o->shape->count, 1);
    UASSERT(o->shape->name == foo);
    UASSERT_EQ((int)o->shape->index, 0);
    UASSERT(o->slots != NULL);
    UASSERT_EQ((int)o->slots[0].kind, (int)UVAL_INT);
    UASSERT_EQ((int)o->slots[0].v.i, 7);

    /* Add a second slot — exercises the copy-old-then-write-new branch
     * with a non-NULL old wrapper (forward Dijkstra barrier path). */
    USymbol *bar = (USymbol *)ustr_intern(&vm, "bar", 3);
    UASSERT(bar != NULL);

    UValue v11;
    v11.kind = UVAL_INT;
    v11.v.i  = 11;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, bar, v11), 0);

    UASSERT_EQ((int)o->shape->count, 2);
    UASSERT(o->shape->name == bar);
    UASSERT_EQ((int)o->shape->index, 1);
    UASSERT_EQ((int)o->slots[0].v.i, 7);   /* preserved */
    UASSERT_EQ((int)o->slots[1].v.i, 11);  /* new */

    uvm_destroy(&vm);
}

UTEST(uobject_set_local_slot_replaces_existing_value_when_present) {
    /* Re-setting an already-local slot is in-place value update.  Shape
     * stays put (count unchanged); no fresh USlotArray allocation. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);

    USymbol *foo = (USymbol *)ustr_intern(&vm, "foo", 3);
    UASSERT(foo != NULL);

    UValue v1;
    v1.kind = UVAL_INT;
    v1.v.i  = 1;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v1), 0);
    UShape *s_after_first = o->shape;
    USlot  *slots_after_first = o->slots;

    UValue v2;
    v2.kind = UVAL_INT;
    v2.v.i  = 2;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, foo, v2), 0);

    UASSERT_EQ((int)o->shape->count, 1);          /* no growth — same name */
    UASSERT(o->shape == s_after_first);           /* shape unchanged */
    UASSERT(o->slots == slots_after_first);       /* slots wrapper unchanged */
    UASSERT_EQ((int)o->slots[0].v.i, 2);          /* value overwritten */

    uvm_destroy(&vm);
}

/* === T40: fallback retry on lookup miss ===
 *
 * Per pre-M2 §4.3.  On full-tree miss, urbi_object_lookup retries once
 * with name = "fallback".  If that hits, the fallback's value flows back
 * to the caller; if it misses too, the original miss stands.
 *
 * Cycle-safety: looking up "fallback" itself must not recurse — the
 * retry is gated on (name != "fallback").
 *
 * Per the third-party-corpus-compatibility-audit B disposition v1.0 does
 * NOT carry the legacy `call.message` reflection, so this test stores a
 * plain UVAL_INT in the fallback slot.  The caller (here the test, real
 * runtime: the OP_CALL site) is responsible for invoking it as a method
 * if the value is a closure; here we just assert the value transferred. */

UTEST(uobject_fallback_retry_on_miss) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);

    USymbol *fallback_sym = (USymbol *)ustr_intern(&vm, "fallback", 8);
    USymbol *bogus        = (USymbol *)ustr_intern(&vm, "doesNotExist", 12);
    UASSERT(fallback_sym != NULL);
    UASSERT(bogus != NULL);

    /* Install a fallback slot holding UVAL_INT(99).  Real runtime would
     * store a UClosure here; the lookup mechanism doesn't care. */
    UValue v99;
    v99.kind = UVAL_INT;
    v99.v.i  = 99;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, fallback_sym, v99), 0);

    /* Lookup of a missing name retries via "fallback" and succeeds with
     * the fallback's value. */
    UValue out;
    out.kind = UVAL_NIL;
    UASSERT_EQ(urbi_object_lookup(&vm, o, bogus, &out), 0);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ((int)out.v.i, 99);

    uvm_destroy(&vm);
}

UTEST(uobject_fallback_lookup_of_fallback_itself_does_not_recurse) {
    /* Looking up "fallback" on an object that doesn't have one must NOT
     * trigger the retry (would recurse forever).  Returns -1 cleanly. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);

    USymbol *fallback_sym = (USymbol *)ustr_intern(&vm, "fallback", 8);

    UValue out;
    out.kind = UVAL_NIL;
    UASSERT_EQ(urbi_object_lookup(&vm, o, fallback_sym, &out), -1);

    uvm_destroy(&vm);
}

UTEST(uobject_fallback_no_fallback_slot_returns_miss) {
    /* If the receiver has no "fallback" slot AND the original name misses,
     * the retry also misses — overall result is -1. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *bogus = (USymbol *)ustr_intern(&vm, "doesNotExist", 12);

    UValue out;
    out.kind = UVAL_NIL;
    UASSERT_EQ(urbi_object_lookup(&vm, o, bogus, &out), -1);

    uvm_destroy(&vm);
}

/* === T39: urbi_object_clone ===
 *
 * Per pre-M2 §4.4 + atom-clone.chk.  The C primitive used by Class.new() /
 * Object.new() once stdlib wiring lands.  v1.0 atom-aware: clone preserves
 * parent's atom family in its flags low-4 and threads parent into protos
 * as the single-tag form. */

UTEST(uobject_clone_preserves_atom_family_and_protos_single) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *integer = urbi_object_atom(&vm, URBI_ATOM_INTEGER_F);
    UASSERT(integer != NULL);

    UObject *c = urbi_object_clone(&vm, integer);
    UASSERT(c != NULL);
    /* Atom family preserved. */
    UASSERT_EQ((int)(c->flags & URBI_OBJ_ATOM_MASK), (int)URBI_ATOM_INTEGER);
    /* Clone's single proto is parent. */
    UASSERT_EQ((int)urbi_object_proto_count(c), 1);
    UASSERT(urbi_object_proto_at(c, 0u) == integer);
    /* Clone has fresh object_id (distinct from parent). */
    UASSERT(c->object_id != integer->object_id);
    /* Clone has the root shape (no slots yet) — same shared singleton as parent. */
    UASSERT(c->shape == integer->shape);
    UASSERT_EQ((int)c->shape->count, 0);

    /* Slots inherited via prototype walk: install bar on parent, then
     * lookup bar on clone resolves to parent.bar's value. */
    USymbol *bar = (USymbol *)ustr_intern(&vm, "bar", 3);
    UValue v55; v55.kind = UVAL_INT; v55.v.i = 55;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, integer, bar, v55), 0);

    UValue out;
    UASSERT_EQ(urbi_object_lookup(&vm, c, bar, &out), 0);
    UASSERT_EQ((int)out.v.i, 55);

    /* Parent flagged IS_PROTOTYPE by set_protos_single (called inside clone). */
    UASSERT((integer->flags & URBI_OBJ_FLAG_IS_PROTOTYPE) != 0u);

    uvm_destroy(&vm);
}

UTEST(uobject_clone_root_returns_object_atom_chained) {
    /* Cloning the root Object yields a fresh Object whose single proto is
     * the root itself.  Atom family stays URBI_ATOM_OBJECT. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *root = urbi_object_root(&vm);
    UObject *c    = urbi_object_clone(&vm, root);
    UASSERT(c != NULL);
    UASSERT_EQ((int)(c->flags & URBI_OBJ_ATOM_MASK), (int)URBI_ATOM_OBJECT);
    UASSERT_EQ((int)urbi_object_proto_count(c), 1);
    UASSERT(urbi_object_proto_at(c, 0u) == root);

    uvm_destroy(&vm);
}

UTEST(uobject_clone_null_returns_null) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT(urbi_object_clone(&vm, NULL) == NULL);
    UASSERT(urbi_object_clone(NULL, NULL) == NULL);

    uvm_destroy(&vm);
}

void test_uobject_suite(void) {
    utest_run("uobject: USlot == UValue (16 B)", uobject_uslot_is_exactly_uvalue);
    utest_run("uobject: header is 56 bytes", uobject_header_is_56_bytes);
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
    utest_run("uobject: proto mutators reject NULL args",
              uobject_proto_mutators_reject_null_args);
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
    utest_run("uobject: add_proto prepends at position 0",
              uobject_add_proto_prepends_position_zero);
    utest_run("uobject: remove absent proto is silent no-op",
              uobject_remove_absent_proto_is_silent_noop);
    utest_run("uobject: set_protos dedups (first occurrence wins)",
              uobject_set_protos_dedups_first_occurrence_wins);
    utest_run("uobject: valid_proto rejects cross-atom-family",
              uobject_valid_proto_rejects_cross_atom_family);
    utest_run("uobject: set_protos aborts on invalid proto (no partial state)",
              uobject_set_protos_aborts_on_invalid_proto_no_partial_state);
    utest_run("uobject: lookup safe under proto-graph cycle",
              uobject_lookup_safe_under_cycle);
    utest_run("uobject: lookup pre-bumps lookup_id on each call",
              uobject_lookup_pre_bumps_lookup_id_each_call);
    utest_run("uobject: lookup_id rollover clears stamps + resets to 1",
              uobject_lookup_id_rollover_clears_stamps);
    utest_run("uobject: lookup_id_force_wrap clears all UObject stamps",
              uobject_lookup_id_force_wrap_clears_all_object_stamps);
    utest_run("uobject: set_local_slot grows slots array + transitions shape",
              uobject_set_local_slot_grows_slots_array_and_transitions_shape);
    utest_run("uobject: set_local_slot replaces existing value when present",
              uobject_set_local_slot_replaces_existing_value_when_present);
    utest_run("uobject: fallback retry on lookup miss (T40)",
              uobject_fallback_retry_on_miss);
    utest_run("uobject: looking up 'fallback' itself does not recurse",
              uobject_fallback_lookup_of_fallback_itself_does_not_recurse);
    utest_run("uobject: no fallback slot → original miss",
              uobject_fallback_no_fallback_slot_returns_miss);
    utest_run("uobject: clone preserves atom family + protos single (T39)",
              uobject_clone_preserves_atom_family_and_protos_single);
    utest_run("uobject: clone of root → fresh Object chained to root",
              uobject_clone_root_returns_object_atom_chained);
    utest_run("uobject: clone NULL returns NULL",
              uobject_clone_null_returns_null);
}
