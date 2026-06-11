/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit test: executable GC rooting matrix (refactor-3 GC-17).
 *
 * The textual gc-roots gate (tests/scripts/check-gc-roots-coverage.sh)
 * proves a field is MENTIONED under src/gc/ — it cannot prove the field is
 * actually walked.  This suite is the executable counterpart: for each
 * field in the GC rooting cross-check table, construct the situation
 * "this field is the ONLY reference to a GC cell", force two full
 * collections, and assert the cell is still live.  A rooting regression
 * then fails loudly here instead of surfacing as a heisencrash hundreds
 * of allocations later — the bug class behind the catch_value UAF
 * (v0.11.4) and the walk_uevent v1.0 STM32F4 hang.
 *
 * This file lands the harness plus the regression cases that pass today
 * (pinning the rooting strand_walk_roots already provides); later
 * v0.13.2 tasks append their own initially-failing case as each rooting
 * fix lands.
 *
 * Liveness oracle: presence on vm->all_cells_head (the per-cell sidecar
 * list).  gc_sweep_step unlinks a swept cell's sidecar, so membership
 * after collection == survival.  We collect TWICE so a cell that survives
 * one cycle by color accident still gets tested against a full
 * mark-from-roots pass. */

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/gc.h"          /* urbi_gc_alloc, urbi_gc_force_full */
#include "urbi/object.h"      /* URBI_ATOM_DICT (container cases) */
#include "gc/ugc.h"           /* UTYPE_OBJECT */
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "realm/urealm.h"
#include "object/uobject.h"   /* struct UObject (sentinel payload size) */
#include "object/uobject_internal.h" /* urbi_protos_alloc (UProtos wrapper case) */
#include "runtime/uclosure.h" /* UClosure.native_fn (dict container cases) */
#include "stdlib/containers.h" /* urbi_stdlib_list_new_empty / _append_value */
#include "value/uintern.h"    /* ustr_intern (dict keys + native-fn lookup) */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Local mirror of UAllCellsNode's layout — same test-only coupling as
 * test_gc_stress_mode.c / test_ugc_state_machine.c (field order verified
 * against ugc_incremental.c: cell, size, next, next_gray). */
typedef struct MirrorNode {
    void              *cell;
    size_t             size;
    struct MirrorNode *next;
    struct MirrorNode *next_gray;
} MirrorNode;

/* Liveness oracle: cell still on the all-cells sidecar list. */
static int cell_live(UVM *vm, const void *cell)
{
    const MirrorNode *n = (const MirrorNode *)(void *)vm->all_cells_head;
    while (n != NULL) {
        if (n->cell == cell) return 1;
        n = n->next;
    }
    return 0;
}

/* Two full collections: a cell that survives cycle 1 by color accident
 * (e.g. born after the mark phase snapshot) must still be re-proven live
 * by a second full mark-from-roots pass. */
static void collect_twice(UVM *vm)
{
    urbi_gc_force_full(vm);
    urbi_gc_force_full(vm);
}

/* Sentinel cell: a zero-payload UTYPE_OBJECT cell.
 *
 * Walker safety (verified against walk_uobject in
 * src/object/utypes_init.c): an all-zero UObject payload is safe to walk —
 * shape == NULL skips the shape shade, slots == NULL skips the slot-array
 * walk, protos == 0 is the EMPTY storage form for UPROTOS_FOREACH, and
 * changed_events_head == NULL skips the subscriber chain.  urbi_gc_alloc
 * zero-inits the full allocation, so sizeof(struct UObject) bytes give the
 * walker a complete, all-NULL field set. */
static UCell *sentinel_alloc(UVM *vm)
{
    return urbi_gc_alloc(vm, sizeof(struct UObject), UTYPE_OBJECT);
}

/* Wrap a sentinel cell as a UVAL_OBJECT UValue.  v.p carries the CELL
 * BASE: UObject embeds UCell at offset 0, mark_root_callback does
 * `UCell *cell = (UCell *)(slot->v.p)`, and uvalue_as_cell's contract
 * (ugc_incremental.h) explicitly covers "synthetic UCell objects allocated
 * via urbi_gc_alloc".  Same idiom as test_ref_gc_root.c /
 * test_deferred_slot_change_ring_roots.c, which store the object pointer
 * directly in v.p. */
static UValue obj_value_for(UCell *cell)
{
    UValue v;
    memset(&v, 0, sizeof v);
    v.kind = UVAL_OBJECT;
    v.v.p  = (void *)cell;
    return v;
}

/* === Strand construction (copied from test_gc_scratch_rooting.c) ===
 *
 * Hand-construct a minimal transient strand and link it onto the global
 * realm so sched_walk_roots → strand_walk_roots visits it:
 *   1. Lazy-create global_realm via urbi_realm_global.
 *   2. Stack-allocate the UStrand (caller provides it); zero it.
 *   3. Allocate a register stack via the VM allocator (mimics
 *      urbi_strand_register_stack_alloc); zero it.
 *   4. Link onto gr->strands_head with next_in_realm.
 * Teardown unlinks symmetrically and frees the register stack. */
static void matrix_strand_setup(UVM *vm, UStrand *s)
{
    /* Disarm stress mode for the lazy realm bootstrap only.  Under
     * URBI_GC_STRESS the realm-population path (urbi_realm_create →
     * urbi_populate_realm_globals) hits the known pre-existing baseline
     * boot crash — a stress collection fires mid-population and sweeps a
     * mid-construction cell (same class that kills string_literal_e2e in
     * the stress build).  That crash is a baseline finding outside this
     * matrix's scope; the matrix cases themselves (sentinel alloc +
     * forced collections) run fully armed. */
    uint8_t stress_saved = vm->gc_stress_armed;
    vm->gc_stress_armed = 0U;
    URealm *gr = urbi_realm_global(vm);
    vm->gc_stress_armed = stress_saved;
    UASSERT(gr != NULL);

    memset(s, 0, sizeof(*s));
    s->vm                  = vm;
    s->state               = USTRAND_STATE_RUNNING;
    s->is_transient_strand = 1U;

    const size_t stack_bytes = UVM_STACK_CAP * sizeof(UValue);
    s->stack = (UValue *)vm->alloc_fn(NULL, stack_bytes, vm->alloc_ud);
    UASSERT(s->stack != NULL);
    memset(s->stack, 0, stack_bytes);
    s->R = s->stack;

    s->realm         = gr;
    s->next_in_realm = gr->strands_head;
    gr->strands_head = s;
}

static void matrix_strand_teardown(UVM *vm, UStrand *s)
{
    URealm *gr = urbi_realm_global(vm);
    UStrand **pp = &gr->strands_head;
    while (*pp != NULL) {
        if (*pp == s) {
            *pp = s->next_in_realm;
            s->next_in_realm = NULL;
            break;
        }
        pp = &(*pp)->next_in_realm;
    }

    vm->alloc_fn(s->stack, 0, vm->alloc_ud);
    s->stack = NULL;
    s->R     = NULL;
}

/* === Matrix cases ===
 *
 * Each case: fresh VM, strand setup, plant the sentinel into exactly ONE
 * strand field (the only reference anywhere), collect twice, assert live,
 * clear the field, teardown.
 *
 * Stress-build compatibility: under URBI_GC_STRESS every urbi_gc_alloc
 * force-collects FIRST, so the sentinel must be allocated AFTER all other
 * allocations in the case and planted immediately — no allocation happens
 * between sentinel_alloc returning and the field store, so no stress
 * collection can fire while the sentinel is unrooted. */

UTEST(matrix_strand_catch_value_is_rooted)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    UCell *sentinel = sentinel_alloc(&vm);
    UASSERT(sentinel != NULL);
    s.catch_value = obj_value_for(sentinel);

    collect_twice(&vm);
    UASSERT(cell_live(&vm, sentinel));

    memset(&s.catch_value, 0, sizeof s.catch_value);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

UTEST(matrix_strand_unwind_value_is_rooted)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    UCell *sentinel = sentinel_alloc(&vm);
    UASSERT(sentinel != NULL);
    s.unwind_value = obj_value_for(sentinel);

    collect_twice(&vm);
    UASSERT(cell_live(&vm, sentinel));

    memset(&s.unwind_value, 0, sizeof s.unwind_value);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

UTEST(matrix_strand_fatal_value_is_rooted)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    UCell *sentinel = sentinel_alloc(&vm);
    UASSERT(sentinel != NULL);
    s.fatal_value = obj_value_for(sentinel);

    collect_twice(&vm);
    UASSERT(cell_live(&vm, sentinel));

    memset(&s.fatal_value, 0, sizeof s.fatal_value);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

UTEST(matrix_strand_last_event_payload_is_rooted)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    UCell *sentinel = sentinel_alloc(&vm);
    UASSERT(sentinel != NULL);
    s.last_event_payload = obj_value_for(sentinel);

    collect_twice(&vm);
    UASSERT(cell_live(&vm, sentinel));

    memset(&s.last_event_payload, 0, sizeof s.last_event_payload);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

UTEST(matrix_uprotos_wrapper_survives_when_owner_live)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    /* Pin the walk_uobject heap-form UProtos wrapper shade (the rooting
     * hole this harness surfaced under URBI_GC_STRESS): a live owner
     * object whose UProtos wrapper cell is reachable ONLY through
     * owner->protos.
     *
     * Stress ordering: every cell is rooted in a register slot before the
     * next allocation, so the collect-before-alloc cannot sweep it. */
    UCell *owner_cell = sentinel_alloc(&vm);
    UASSERT(owner_cell != NULL);
    s.stack[0] = obj_value_for(owner_cell);   /* owner's ONLY root */

    UCell *proto_a = sentinel_alloc(&vm);
    UASSERT(proto_a != NULL);
    s.stack[1] = obj_value_for(proto_a);      /* transient root */

    UCell *proto_b = sentinel_alloc(&vm);
    UASSERT(proto_b != NULL);
    s.stack[2] = obj_value_for(proto_b);      /* transient root */

    /* Heap-form UProtos block (n == 2), caller-filled per the
     * urbi_object_set_protos_heap contract (uobject.h).
     * urbi_protos_alloc makes exactly one allocation, so nothing
     * allocates between it and the publish below — the unrooted window
     * is stress-safe. */
    UProtos *up = urbi_protos_alloc(&vm, 2U);
    UASSERT(up != NULL);
    UCell *wrapper_cell = (UCell *)(void *)up;
    up->items[0] = (UObject *)(void *)proto_a;
    up->items[1] = (UObject *)(void *)proto_b;

    UObject *owner = (UObject *)(void *)owner_cell;
    urbi_object_set_protos_heap(&vm, owner, up);

    /* Drop the transient proto roots: wrapper AND protos are now
     * reachable only via owner->protos. */
    memset(&s.stack[1], 0, sizeof s.stack[1]);
    memset(&s.stack[2], 0, sizeof s.stack[2]);

    /* THREE full collections — the observed pre-fix failure cadence:
     * set_protos_heap's install barrier shades the wrapper gray, so it
     * drained black in cycle 1; after the color flip nothing re-shaded
     * it, so cycle 2's sweep freed it while the owner was live, and
     * cycle 3's mark walked freed memory through owner->protos (UAF). */
    collect_twice(&vm);
    urbi_gc_force_full(&vm);

    UASSERT(cell_live(&vm, owner_cell));
    UASSERT(cell_live(&vm, wrapper_cell));
    UASSERT(cell_live(&vm, proto_a));
    UASSERT(cell_live(&vm, proto_b));
    /* Owner's protos must still be the published heap form, intact. */
    UASSERT(owner->protos == (uintptr_t)up);
    UASSERT_EQ(up->n, 2U);
    UASSERT(up->items[0] == (UObject *)(void *)proto_a);
    UASSERT(up->items[1] == (UObject *)(void *)proto_b);

    memset(&s.stack[0], 0, sizeof s.stack[0]);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

UTEST(matrix_register_window_is_rooted)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    UCell *sentinel = sentinel_alloc(&vm);
    UASSERT(sentinel != NULL);
    s.stack[3] = obj_value_for(sentinel);

    collect_twice(&vm);
    UASSERT(cell_live(&vm, sentinel));

    memset(&s.stack[3], 0, sizeof s.stack[3]);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

/* === Container element cases (Task 3 / refactor-3 B2/GC-01/STD-01) ===
 *
 * UList / UDict backing stores are raw vm->alloc_fn buffers threaded onto
 * vm->stdlib_containers; the script-visible object's _storage slot is
 * deliberately UVAL_INT, so the object walker treats it as a leaf and the
 * elements need a dedicated root provider
 * (urbi_stdlib_containers_walk_roots). */

/* Dict has no host-side C constructor/mutator (the v0.9.1 host helpers in
 * containers.h cover List only), so the dict cases drive the Dict atom
 * proto's native methods directly — same resolve-the-native_fn idiom as
 * fetch_native_fn in test_event_native.c. */
static urbi_native_method_fn
container_native_fn(UVM *vm, UObject *proto, const char *name, size_t len)
{
    USymbol *sym = (USymbol *)ustr_intern(vm, name, len);
    if (sym == NULL) return NULL;
    UValue slot;
    memset(&slot, 0, sizeof slot);
    if (urbi_object_lookup(vm, proto, sym, &slot) != 0) return NULL;
    if (slot.kind != (uint8_t)UVAL_CLOSURE || slot.v.p == NULL) return NULL;
    return ((UClosure *)slot.v.p)->native_fn;
}

/* Build an empty Dict via its native `new` and root the Dict OBJECT in
 * s->stack[0] (the object must stay live across the case's forced
 * collections so the set call below isn't a UAF; its elements are still
 * invisible to the object walker).  Construction runs with stress
 * disarmed: the clone+attach ctor path shares the mid-construction
 * stress-hazard class noted in matrix_strand_setup; the armed window each
 * case cares about is sentinel alloc → element store → collections. */
static UValue
matrix_dict_new_rooted(UVM *vm, UStrand *s)
{
    UObject *dict_proto = urbi_object_atom(vm, URBI_ATOM_DICT);
    UASSERT(dict_proto != NULL);
    urbi_native_method_fn new_fn = container_native_fn(vm, dict_proto, "new", 3);
    UASSERT(new_fn != NULL);

    uint8_t stress_saved = vm->gc_stress_armed;
    vm->gc_stress_armed = 0U;
    UValue d = urbi_make_nil();
    int rc = new_fn(vm, obj_value_for((UCell *)(void *)dict_proto),
                    NULL, 0, &d);
    vm->gc_stress_armed = stress_saved;
    UASSERT_EQ(rc, UEXEC_OK);
    UASSERT_EQ((int)d.kind, (int)UVAL_OBJECT);
    s->stack[0] = d;
    return d;
}

UTEST(matrix_list_element_is_rooted)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    /* List construction with stress disarmed (see matrix_dict_new_rooted);
     * the List OBJECT is rooted in s.stack[0] so the append call below
     * stays valid across stress collections. */
    uint8_t stress_saved = vm.gc_stress_armed;
    vm.gc_stress_armed = 0U;
    UObject *lst = urbi_stdlib_list_new_empty(&vm);
    vm.gc_stress_armed = stress_saved;
    UASSERT(lst != NULL);
    s.stack[0] = obj_value_for((UCell *)(void *)lst);

    /* Sentinel's ONLY reference becomes the UList backing's items[0].
     * Stress-safe unrooted window: append performs no urbi_gc_alloc
     * (the _storage lookup is a pure intern hit; the fresh list's cap is
     * 4 > len 0, so no grow). */
    UCell *sentinel = sentinel_alloc(&vm);
    UASSERT(sentinel != NULL);
    int rc = urbi_stdlib_list_append_value(&vm, lst, obj_value_for(sentinel));
    UASSERT_EQ(rc, URBI_OK);

    collect_twice(&vm);
    UASSERT(cell_live(&vm, sentinel));

    memset(&s.stack[0], 0, sizeof s.stack[0]);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

UTEST(matrix_dict_value_is_rooted)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    UValue d = matrix_dict_new_rooted(&vm, &s);
    UObject *dict_proto = urbi_object_atom(&vm, URBI_ATOM_DICT);
    urbi_native_method_fn set_fn = container_native_fn(&vm, dict_proto, "set", 3);
    UASSERT(set_fn != NULL);

    /* Intern the key BEFORE the sentinel is born — interning a new string
     * may allocate, and nothing may allocate while the sentinel is
     * unrooted. */
    UValue key;
    memset(&key, 0, sizeof key);
    key.kind = UVAL_STR;
    key.v.p  = (void *)ustr_intern(&vm, "k", 1);
    UASSERT(key.v.p != NULL);

    /* Sentinel's ONLY reference becomes the UDict backing's e->val.
     * Stress-safe unrooted window: dict_set performs no urbi_gc_alloc
     * (storage lookup is a pure intern hit; table growth uses the raw
     * allocator). */
    UCell *sentinel = sentinel_alloc(&vm);
    UASSERT(sentinel != NULL);
    UValue args[2];
    args[0] = key;
    args[1] = obj_value_for(sentinel);
    UValue out = urbi_make_nil();
    int rc = set_fn(&vm, d, args, 2, &out);
    UASSERT_EQ(rc, UEXEC_OK);

    collect_twice(&vm);
    UASSERT(cell_live(&vm, sentinel));

    memset(&s.stack[0], 0, sizeof s.stack[0]);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

UTEST(matrix_dict_key_is_rooted)
{
    /* Dict keys are UVAL_STR-only at v1.0 (dict_key_check raises on
     * anything else), and interned strings live in the intern table —
     * raw vm->alloc_fn memory that is never on all_cells_head, so a key
     * can never be a swept GC cell today.  This case future-proofs the
     * provider's e->key yield: walking a USED entry's key must not crash
     * (mark_root_callback ignores UVAL_STR as a non-heap leaf), and the
     * value planted alongside it must survive. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    UValue d = matrix_dict_new_rooted(&vm, &s);
    UObject *dict_proto = urbi_object_atom(&vm, URBI_ATOM_DICT);
    urbi_native_method_fn set_fn = container_native_fn(&vm, dict_proto, "set", 3);
    UASSERT(set_fn != NULL);

    UValue key;
    memset(&key, 0, sizeof key);
    key.kind = UVAL_STR;
    key.v.p  = (void *)ustr_intern(&vm, "key2", 4);
    UASSERT(key.v.p != NULL);

    UCell *sentinel = sentinel_alloc(&vm);
    UASSERT(sentinel != NULL);
    UValue args[2];
    args[0] = key;
    args[1] = obj_value_for(sentinel);
    UValue out = urbi_make_nil();
    int rc = set_fn(&vm, d, args, 2, &out);
    UASSERT_EQ(rc, UEXEC_OK);

    collect_twice(&vm);
    UASSERT(cell_live(&vm, sentinel));

    memset(&s.stack[0], 0, sizeof s.stack[0]);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

/* === Suite entry point === */

void test_gc_rooting_matrix_suite(void)
{
    printf("  [gc_rooting_matrix]\n");
    utest_run("matrix_strand_catch_value_is_rooted",
              matrix_strand_catch_value_is_rooted);
    utest_run("matrix_strand_unwind_value_is_rooted",
              matrix_strand_unwind_value_is_rooted);
    utest_run("matrix_strand_fatal_value_is_rooted",
              matrix_strand_fatal_value_is_rooted);
    utest_run("matrix_strand_last_event_payload_is_rooted",
              matrix_strand_last_event_payload_is_rooted);
    utest_run("matrix_uprotos_wrapper_survives_when_owner_live",
              matrix_uprotos_wrapper_survives_when_owner_live);
    utest_run("matrix_register_window_is_rooted",
              matrix_register_window_is_rooted);
    utest_run("matrix_list_element_is_rooted",
              matrix_list_element_is_rooted);
    utest_run("matrix_dict_value_is_rooted",
              matrix_dict_value_is_rooted);
    utest_run("matrix_dict_key_is_rooted",
              matrix_dict_key_is_rooted);
}
