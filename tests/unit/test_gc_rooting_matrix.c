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
#include "urbi/gc.h"          /* urbi_gc_alloc, urbi_gc_force_full, urbi_gc_slice */
#include "urbi/object.h"      /* URBI_ATOM_DICT (container cases) */
#include "gc/ugc.h"           /* UTYPE_OBJECT, urbi_gc_phase */
#include "gc/ugc_incremental.h" /* GC_PHASE_* (ATOMIC_FINISH re-scan case) */
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "realm/urealm.h"
#include "object/uobject.h"   /* struct UObject (sentinel payload size) */
#include "object/uobject_internal.h" /* urbi_protos_alloc (UProtos wrapper case) */
#include "runtime/uclosure.h" /* UClosure.native_fn (dict container cases) */
#include "stdlib/containers.h" /* urbi_stdlib_list_new_empty / _append_value */
#include "value/uintern.h"    /* ustr_intern (dict keys + native-fn lookup) */
#include "runtime/ucleanup.h" /* strand_cleanup_* (cleanup owning_tag case) */
#include "tag/utag.h"         /* utag_create (owning_tag + suspend_tag cases) */

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

/* === Strand tag/value root cases (Task 5 / refactor-3 GC-03/GC-04/SCHED-07) === */

UTEST(matrix_strand_unblock_value_is_rooted)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    UCell *sentinel = sentinel_alloc(&vm);
    UASSERT(sentinel != NULL);
    s.unblock_value = obj_value_for(sentinel);

    collect_twice(&vm);
    UASSERT(cell_live(&vm, sentinel));

    memset(&s.unblock_value, 0, sizeof s.unblock_value);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

UTEST(matrix_cleanup_owning_tag_is_rooted)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    /* Real cleanup stack via the runtime's own init (raw vm->alloc_fn —
     * never a GC allocation, so no stress hazard); entries arrive zeroed. */
    int rc = strand_cleanup_stack_init(&s, &vm, (uint16_t)URBI_CLEANUP_MAX);
    UASSERT_EQ(rc, 0);

    /* Push FIRST (pure index bump, no allocation), THEN create the tag,
     * THEN store: under URBI_GC_STRESS utag_create force-collects before
     * allocating, and nothing may allocate between utag_create returning
     * and the owning_tag store while the tag is unrooted. */
    UCleanupEntry *entry = strand_cleanup_push(&s);
    UASSERT(entry != NULL);
    entry->kind        = (uint8_t)UCLEANUP_TAG_SCOPE;
    entry->strand_back = &s;   /* mirrors vm_push_tag_scope; the remaining
                                  fields keep their zero/NULL init values,
                                  matching the anonymous-scope arm */

    UTag *tag = utag_create(&vm);
    UASSERT(tag != NULL);
    entry->owning_tag = tag;   /* tag's ONLY reference (the anonymous
                                  per-scope shape vm_push_tag_scope builds) */

    collect_twice(&vm);
    UASSERT(cell_live(&vm, (UCell *)tag));

    /* Clear + pop before teardown so nothing later treats the slot as a
     * live tag scope; the now-unreferenced tag is reclaimed by a later
     * cycle (GC owns it — no utag_destroy here). */
    entry->owning_tag = NULL;
    strand_cleanup_pop(&s, UCLEANUP_TAG_SCOPE);
    strand_cleanup_stack_destroy(&s, &vm);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

UTEST(matrix_suspend_tag_is_rooted)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    /* Stamp the SUSPENDED|BLOCK composite directly — the exact state byte
     * urbi_strand_suspend writes (the queue splice it also performs does
     * not apply: this hand-built strand was never enqueued).  Stamp BEFORE
     * the tag is born so the union arm is already discriminated live when
     * a stress collection fires inside utag_create. */
    uint8_t state_saved = s.state;
    s.state = (uint8_t)USTRAND_STATE_SUSPENDED_BLOCK;

    UTag *tag = utag_create(&vm);
    UASSERT(tag != NULL);
    s.wait_payload.suspend_tag = tag;   /* tag's ONLY reference */

    collect_twice(&vm);
    UASSERT(cell_live(&vm, (UCell *)tag));

    /* Restore the pre-suspend state + clear the payload so teardown and
     * urbi_vm_destroy never see a fake-suspended strand. */
    s.wait_payload.suspend_tag = NULL;
    s.state = state_saved;
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

/* === ATOMIC_FINISH root re-scan case (Task 4 / refactor-3 B5/GC-02) ===
 *
 * The classic incremental-marking lost-object window: the mutator loads
 * the LAST reference to a white cell into a root that the cycle already
 * scanned (a strand register — urbi_gc_register_write is deliberately
 * barrier-free), then clears the original heap reference before its
 * holder is traced.  Without a stop-the-world root re-scan at
 * ATOMIC_FINISH, the cycle finishes with the cell unmarked and SWEEP
 * frees it while it is still reachable.
 *
 * Construction (drives the slice machine by hand, same idiom as
 * test_ugc_state_machine.c):
 *   1. Plant BEFORE the cycle: holder P (slot-capable object, rooted
 *      only via s.stack[0]) holds sentinel C via local slot "c" — C's
 *      ONLY reference.  Planting pre-cycle keeps the set_local_slot
 *      install barrier from shading C inside the cycle.
 *   2. One tiny-budget slice: IDLE -> MARK_ROOTS runs ALL providers in
 *      a single step (gc_mark_roots_step is one-shot; it returns 1024
 *      work units, exhausting the budget), leaving the machine in
 *      MARK_INCREMENTAL with the register window already scanned and P
 *      gray-but-untraced on the worklist.
 *   3. Mutator move: s.stack[1] = C (no barrier — register write) and
 *      P.c = nil (in-place slot update; storing nil shades nothing,
 *      and the forward Dijkstra barrier protects only the NEW value).
 *   4. Drain the cycle to IDLE.  P is traced after the clear, so the
 *      trace never reaches C; C's only ref sits in an already-scanned
 *      register.  Pre-fix: C is swept (this assert fails).  Post-fix:
 *      the ATOMIC_FINISH full provider re-scan re-discovers stack[1].
 *
 * Stress-build safety: every allocation in the plant happens with both
 * cells rooted in register slots (collect-before-alloc cannot sweep
 * them), and the in-cycle mutator section performs no allocation at
 * all (register store + in-place nil store), so no stress collection
 * can fire mid-cycle and run the staged scenario to completion. */

UTEST(matrix_atomic_finish_rescans_mutated_registers)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    /* Intern the slot symbol up front (interning may allocate). */
    USymbol *sym = (USymbol *)ustr_intern(&vm, "c", 1);
    UASSERT(sym != NULL);

    /* P: a real slot-capable object (urbi_object_alloc wires the root
     * shape, which set_local_slot's shape transition requires; a zeroed
     * sentinel has shape == NULL and cannot hold slots).  Rooted in
     * stack[0] — its ONLY root. */
    UObject *holder = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(holder != NULL);
    s.stack[0] = obj_value_for((UCell *)(void *)holder);

    /* C: the sentinel.  Temp-rooted in stack[1] while the slot install
     * below allocates (child shape + USlotArray wrapper). */
    UCell *sentinel = sentinel_alloc(&vm);
    UASSERT(sentinel != NULL);
    s.stack[1] = obj_value_for(sentinel);

    int rc = urbi_object_set_local_slot(&vm, holder, sym,
                                        obj_value_for(sentinel));
    UASSERT_EQ(rc, 0);

    /* Drop the temp root: P.c is now C's only reference anywhere. */
    memset(&s.stack[1], 0, sizeof s.stack[1]);

    /* Drive ONE slice: completes MARK_ROOTS (all providers run; the
     * register window is scanned, P is shaded gray) and stops in
     * MARK_INCREMENTAL before P is traced. */
    vm.gc_debt = 1;   /* the IDLE arm starts a cycle only when debt > 0
                       * (gc_pending is a dispatcher-side request flag the
                       * slice machine clears but never gates on) */
    urbi_gc_slice(&vm, 1U);
    UASSERT_EQ((uint8_t)GC_PHASE_MARK_INCREMENTAL, urbi_gc_phase(&vm));

    /* Mutator move: C's only ref migrates into the ALREADY-SCANNED
     * register window; the original slot is cleared before P is traced. */
    s.stack[1] = obj_value_for(sentinel);
    rc = urbi_object_set_local_slot(&vm, holder, sym, urbi_make_nil());
    UASSERT_EQ(rc, 0);

    /* Finish the cycle.  Capped so a wedge regression (phase never
     * reaching IDLE) fails the case instead of hanging CI. */
    {
        int spins = 0;
        while (urbi_gc_phase(&vm) != (uint8_t)GC_PHASE_IDLE) {
            urbi_gc_slice(&vm, 4096U);
            spins++;
            UASSERT(spins < 64);
        }
    }

    UASSERT(cell_live(&vm, sentinel));

    memset(&s.stack[0], 0, sizeof s.stack[0]);
    memset(&s.stack[1], 0, sizeof s.stack[1]);
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
    utest_run("matrix_strand_unblock_value_is_rooted",
              matrix_strand_unblock_value_is_rooted);
    utest_run("matrix_cleanup_owning_tag_is_rooted",
              matrix_cleanup_owning_tag_is_rooted);
    utest_run("matrix_suspend_tag_is_rooted",
              matrix_suspend_tag_is_rooted);
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
    utest_run("matrix_atomic_finish_rescans_mutated_registers",
              matrix_atomic_finish_rescans_mutated_registers);
}
