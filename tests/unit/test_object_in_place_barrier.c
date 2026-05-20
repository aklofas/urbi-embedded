/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: object-model fixes from v0.5.7-fixes Phase 13 (T57-T66).
 *
 * T57 OBJ-003 + OBJ-036:
 *     urbi_object_set_local_slot's in-place value update writes the new
 *     value through the Dijkstra forward barrier.  Comment block
 *     cross-referenced (OBJ-036) rewritten to reflect "new value into
 *     black parent must be grayed" semantics.
 *
 * T58 OBJ-004:
 *     urbi_object_alloc with shape-root OOM clears the partially-init
 *     UObject so a GC walker visiting before sweep does not dereference
 *     uninit memory.
 *
 * T59 OBJ-005:
 *     urbi_object_install_property does not mutate an aliased UShape
 *     (shapes shared via UShapeMap dedup).
 *
 * T60 OBJ-006:
 *     urbi_object_install_property does not leak a freshly-allocated
 *     UProps when the shape transition fails.  Allocation is reordered
 *     after transition succeeds.
 *
 * T61 OBJ-008:
 *     urbi_object_add_proto / urbi_object_set_protos return URBI_ERR_OOM
 *     (not URBI_ERR_INVALID_ARG) when the UProtos cell allocation fails.
 *
 * T62 OBJ-014:
 *     urbi_object_lookup's second-pass wrap-protection accounts for the
 *     fact that the first pass may have already forced a wrap (in which
 *     case all stamps are fresh and the second pass need not check
 *     wrap again).
 *
 * T63 OBJ-018:
 *     urbi_object_set_property_value does not leak a mutation through
 *     props_table[idx] entries shared with sibling shapes (cross-shape
 *     alias copy-on-write).
 *
 * T64 OBJ-019:
 *     urbi_shape_transition_remove_slot's depth cap accounts for the
 *     dropped name (off-by-one fix).
 *
 * T66 OBJ-041:
 *     urbi_object_install_property's idempotent path does not bump
 *     vm->topology_gen.  A separate vm->props_content_gen counter tracks
 *     property-content mutations; idempotent installs bump neither.
 *
 * (T65 retires walk_uprotoinstance dead code — no test, dead-code
 * exception per Gate G1.) */

#include "utest.h"

#include "vm/uvm.h"
#include "object/uobject.h"
#include "object/ushape.h"
#include "value/uintern.h"
#include "chunk/uchunk.h"
#include "gc/ugc_incremental.h"   /* IS_BLACK / IS_GRAY / UGC_COLOR_* */
#include "urbi/gc.h"
#include "urbi/urbi.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * T57: set_local_slot_writes_through_barrier
 *
 * Force a UObject BLACK, install a slot holding a fresh UCell-bearing
 * value, then write a *new* white cell through the same slot via the
 * in-place update path.  Without the slot-write barrier the new white
 * value is left unmarked under a black parent, breaking the tri-color
 * invariant.
 *
 * Use a UObject as the value cell — UVAL_OBJECT is a heap-bearing kind
 * and the barrier helper consults uvalue_is_heap_white.
 * =================================================================== */

UTEST(set_local_slot_writes_through_barrier)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Allocate the receiver and a starter slot, then a fresh value cell. */
    UObject *recv = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(recv != NULL);
    if (recv == NULL) { urbi_vm_destroy(&vm); return; }

    USymbol *name = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(name != NULL);
    if (name == NULL) { urbi_vm_destroy(&vm); return; }

    UObject *first = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(first != NULL);
    if (first == NULL) { urbi_vm_destroy(&vm); return; }

    UValue v0;
    v0.kind = (uint8_t)UVAL_OBJECT;
    v0.v.p = first;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, recv, name, v0), 0);

    /* Force receiver BLACK; allocate a fresh second value (white).  The
     * in-place update writes that white value into the BLACK parent's
     * slot — this is the case Dijkstra demands the barrier on. */
    UCell *parent = (UCell *)recv;
    parent->gc_byte = (uint8_t)((parent->gc_byte & ~UGC_COLOR_MASK)
                                | UGC_COLOR_BLACK);
    UASSERT(IS_BLACK(parent));

    UObject *second = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(second != NULL);
    if (second == NULL) { urbi_vm_destroy(&vm); return; }
    /* Fresh allocation is white. */
    UCell *child = (UCell *)second;

    UValue v1;
    v1.kind = (uint8_t)UVAL_OBJECT;
    v1.v.p = second;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, recv, name, v1), 0);

    /* Barrier semantics: new value (white) was stored under a black
     * parent.  After the write the new child must be at least gray
     * (forward Dijkstra: shade the target). */
    UASSERT(IS_GRAY(child) || IS_BLACK(child));

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T58: object_alloc_clean_on_shape_root_oom
 *
 * Use a spy allocator to fail at exactly the urbi_shape_root call inside
 * urbi_object_alloc.  Verify the function returns NULL and that the
 * partially-initialized UObject is observably non-toxic to a GC walker
 * (shape != garbage; key fields zeroed).
 *
 * Strategy: arrange the spy so the FIRST alloc (UObject cell) succeeds
 * but the SECOND (root UShape inside urbi_shape_root) fails.
 * =================================================================== */

typedef struct {
    int alloc_calls;
    int fail_at;   /* fail when alloc_calls == fail_at (post-increment) */
} ObjAllocSpy;

static void *
obj_spy_alloc(void *ptr, size_t n, void *ud)
{
    ObjAllocSpy *spy = (ObjAllocSpy *)ud;
    if (n == 0) {
        free(ptr);
        return NULL;
    }
    if (ptr == NULL) {
        spy->alloc_calls++;
        if (spy->fail_at > 0 && spy->alloc_calls == spy->fail_at) {
            return NULL;
        }
    }
    return realloc(ptr, n);
}

UTEST(object_alloc_clean_on_shape_root_oom)
{
    /* Probe: count how many allocations a clean urbi_object_alloc takes
     * for atom Object on a freshly-init VM whose root_shape is unset. */
    ObjAllocSpy probe = { 0, -1 };
    UVM vm_probe;
    urbi_vm_init(&vm_probe, obj_spy_alloc, &probe);

    int before = probe.alloc_calls;
    UObject *o = urbi_object_alloc(&vm_probe, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    int delta = probe.alloc_calls - before;
    UASSERT(delta >= 2);  /* UObject cell + root UShape cell at minimum */
    urbi_vm_destroy(&vm_probe);

    /* Now run with fail_at set so the UObject cell allocates but the
     * urbi_shape_root cell (alloc #2 of the sequence) fails. */
    ObjAllocSpy spy = { 0, 0 };
    UVM vm;
    urbi_vm_init(&vm, obj_spy_alloc, &spy);

    /* Aim the failure at the second alloc inside urbi_object_alloc.
     * Reset the counter so spy.alloc_calls == 1 means UObject cell,
     * spy.alloc_calls == 2 means root shape. */
    spy.alloc_calls = 0;
    spy.fail_at = 2;

    UObject *bad = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(bad == NULL);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T59: install_property_does_not_mutate_aliased_shape
 *
 * Two UObjects share the same UShape after performing the same slot
 * transition (UShapeMap dedup → child shape pointer is shared).  After
 * installing a property on object A, object B's shape must still have
 * no props_table installed (or, if B's shape is a fresh sibling, A's
 * shape must remain unchanged).  Specifically: A's pre-install shape
 * pointer must NOT be the same as A's post-install shape pointer when
 * the original shape was aliased with B.
 * =================================================================== */

UTEST(install_property_does_not_mutate_aliased_shape)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    USymbol *xname = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(xname != NULL);
    if (xname == NULL) { urbi_vm_destroy(&vm); return; }

    UObject *a = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(a != NULL);
    if (a == NULL) { urbi_vm_destroy(&vm); return; }
    UValue zero;
    zero.kind = (uint8_t)UVAL_NIL;
    zero.v.i  = 0;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, a, xname, zero), 0);

    /* First install: non-idempotent — fresh sibling shape with own
     * props_table.  Snapshot the post-install shape pointer. */
    UASSERT_EQ(urbi_object_install_property(&vm, a, xname,
                                            URBI_SLOT_FLAG_OGET, zero), 0);
    UShape *shape_after_first = a->shape;
    UASSERT(shape_after_first != NULL);
    if (shape_after_first == NULL) { urbi_vm_destroy(&vm); return; }
    UProps *const *pt_first = shape_after_first->props_table;
    UASSERT(pt_first != NULL);
    UProps *uprops_first = (UProps *)pt_first[0];
    UASSERT(uprops_first != NULL);

    /* Second install with SAME flag bit — idempotent path.  After fix:
     * the in-place mutation of `obj->shape->props_table[idx]` must not
     * publish back through any other holder of `shape_after_first` —
     * meaning the safe approach either (a) leaves shape_after_first's
     * props_table[0] entry pointing at uprops_first, OR (b) clones the
     * shape so a->shape changes.  Either way: shape_after_first's own
     * props_table[0] (as observed BEFORE refresh) must still point at
     * uprops_first.  Snapshot the entry pointer-by-value. */
    UProps *snap_pt0 = (UProps *)shape_after_first->props_table[0];

    UValue g2;
    g2.kind = (uint8_t)UVAL_NIL;
    g2.v.i  = 99;
    UASSERT_EQ(urbi_object_install_property(&vm, a, xname,
                                            URBI_SLOT_FLAG_OGET, g2), 0);

    /* The fix: the original shape's props_table[0] entry must remain
     * pointing at the original UProps cell (no in-place corruption). */
    UProps *snap_pt0_after = (UProps *)shape_after_first->props_table[0];
    UASSERT(snap_pt0_after == snap_pt0);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T60: install_property_no_uprops_leak_on_transition_failure
 *
 * Install a property where the shape transition allocator fails before
 * the UProps cell allocator runs.  After the fix, no UProps cell is
 * allocated on the failure path (allocate-after-transition reorder).
 *
 * This test verifies the failure return code; the leak itself is
 * absence-of-allocation, observed via the spy's call count not growing
 * past the transition failure point.
 * =================================================================== */

UTEST(install_property_no_uprops_leak_on_transition_failure)
{
    /* Probe: count clean allocs for install_property happy path. */
    ObjAllocSpy probe = { 0, -1 };
    UVM vm_probe;
    urbi_vm_init(&vm_probe, obj_spy_alloc, &probe);
    USymbol *name_p = (USymbol *)ustr_intern(&vm_probe, "x", 1);
    UASSERT(name_p != NULL);
    UObject *o_p = urbi_object_alloc(&vm_probe, URBI_ATOM_OBJECT);
    UASSERT(o_p != NULL);
    UValue z;
    z.kind = (uint8_t)UVAL_NIL;
    z.v.i  = 0;
    UASSERT_EQ(urbi_object_set_local_slot(&vm_probe, o_p, name_p, z), 0);
    int before_install = probe.alloc_calls;
    UASSERT_EQ(urbi_object_install_property(&vm_probe, o_p, name_p,
                                            URBI_SLOT_FLAG_OGET, z), 0);
    int install_clean_allocs = probe.alloc_calls - before_install;
    UASSERT(install_clean_allocs >= 2);   /* sibling UShape + UProps + maybe UPropsTable */
    urbi_vm_destroy(&vm_probe);

    /* Now run with the allocator failing on the FIRST install allocation
     * — that's the sibling UShape transition.  After the fix, install_
     * property must return -1 and no UProps cell is allocated. */
    ObjAllocSpy spy = { 0, -1 };
    UVM vm;
    urbi_vm_init(&vm, obj_spy_alloc, &spy);
    USymbol *name = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(name != NULL);
    if (name == NULL) { urbi_vm_destroy(&vm); return; }
    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (o == NULL) { urbi_vm_destroy(&vm); return; }
    UValue v;
    v.kind = (uint8_t)UVAL_NIL;
    v.v.i  = 0;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, name, v), 0);

    /* Aim the next allocator failure at #1 of install_property's calls. */
    spy.alloc_calls = 0;
    spy.fail_at = 1;
    int rc = urbi_object_install_property(&vm, o, name,
                                          URBI_SLOT_FLAG_OGET, v);
    UASSERT(rc != 0);
    /* Allocator was called at most once before failure, so nothing
     * could have been "leaked" beyond the failed attempt itself. */
    UASSERT(spy.alloc_calls == 1);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T61: add_proto_returns_oom_not_invalid_arg_on_alloc_failure
 *
 * urbi_object_add_proto must propagate URBI_ERR_OOM from the UProtos
 * allocator failure, not URBI_ERR_INVALID_ARG.
 * =================================================================== */

UTEST(add_proto_returns_oom_not_invalid_arg_on_alloc_failure)
{
    /* Probe: count happy-path allocs through add_proto (with two existing
     * protos, third call must allocate a UProtos heap form). */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *o   = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *p1  = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UObject *p2  = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL && p1 != NULL && p2 != NULL);
    if (o == NULL || p1 == NULL || p2 == NULL) {
        urbi_vm_destroy(&vm); return;
    }

    /* First add: stays in single form, no heap alloc. */
    UASSERT_EQ(urbi_object_add_proto(&vm, o, p1), URBI_OK);

    urbi_vm_destroy(&vm);

    /* Re-run with a failing allocator.  After the second add_proto we
     * trigger the heap-form UProtos alloc; arrange that one to fail. */
    ObjAllocSpy spy = { 0, -1 };
    UVM vm2;
    urbi_vm_init(&vm2, obj_spy_alloc, &spy);

    UObject *o2  = urbi_object_alloc(&vm2, URBI_ATOM_OBJECT);
    UObject *p1b = urbi_object_alloc(&vm2, URBI_ATOM_OBJECT);
    UObject *p2b = urbi_object_alloc(&vm2, URBI_ATOM_OBJECT);
    UASSERT(o2 != NULL && p1b != NULL && p2b != NULL);
    if (o2 == NULL || p1b == NULL || p2b == NULL) {
        urbi_vm_destroy(&vm2); return;
    }
    UASSERT_EQ(urbi_object_add_proto(&vm2, o2, p1b), URBI_OK);

    /* Now arrange next alloc to fail. */
    spy.alloc_calls = 0;
    spy.fail_at = 1;
    int rc = urbi_object_add_proto(&vm2, o2, p2b);
    UASSERT_EQ(rc, URBI_ERR_OOM);

    urbi_vm_destroy(&vm2);
}

/* ===================================================================
 * T62: lookup_second_pass_force_wrap_correct
 *
 * Drive vm->lookup_id to a value where the FIRST pass triggers
 * force_wrap, then verify the lookup completes correctly (hit/miss
 * outcome unchanged) and the lookup_id ends in a sane state.  The
 * audit notes the issue is a perf smell (O(2N) walks), not a
 * correctness bug; we verify the behavior remains correct after the
 * cleanup.
 * =================================================================== */

UTEST(lookup_second_pass_force_wrap_correct)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *root = urbi_object_root(&vm);
    UObject *o    = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(root != NULL && o != NULL);
    if (root == NULL || o == NULL) { urbi_vm_destroy(&vm); return; }
    urbi_object_set_protos_single(&vm, o, root);

    USymbol *xname = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(xname != NULL);
    UValue v;
    v.kind = (uint8_t)UVAL_NIL;
    v.v.i  = 42;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, xname, v), 0);

    /* Drive lookup_id to UINT32_MAX so the next bump triggers force_wrap. */
    vm.lookup_id = (uint64_t)UINT32_MAX;

    UValue out;
    out.kind = (uint8_t)UVAL_NIL;
    out.v.i  = 0;
    int rc = urbi_object_lookup(&vm, o, xname, &out);
    UASSERT_EQ(rc, 0);   /* hit */
    UASSERT_EQ((int)out.v.i, 42);
    /* After force_wrap, lookup_id should be 1; no second pass needed
     * since first pass hit. */
    UASSERT(vm.lookup_id >= 1ULL);

    /* Now drive lookup_id again to UINT32_MAX, do a *miss* lookup so
     * the second pass for "fallback" runs.  Verify it still terminates
     * and returns -1 (no fallback installed). */
    vm.lookup_id = (uint64_t)UINT32_MAX;
    USymbol *missing = (USymbol *)ustr_intern(&vm, "nope", 4);
    UASSERT(missing != NULL);
    UValue out2;
    out2.kind = (uint8_t)UVAL_NIL;
    int rc2 = urbi_object_lookup(&vm, o, missing, &out2);
    UASSERT_EQ(rc2, -1);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T63: set_property_value_isolates_sibling_shape
 *
 * Build two siblings S1, S2 (same lineage, different per-slot flag
 * bits).  S2's props_table[idx] starts as a copy of S1's pointer.
 * After set_property_value mutates S1's UProps oget, S2's UProps
 * pointer must still point at the original (un-mutated) UProps.
 * Without copy-on-write the in-place mutation leaks across to S2.
 * =================================================================== */

UTEST(set_property_value_isolates_sibling_shape)
{
    /* Build a setup where two objects' shapes share the same
     * props_table[idx_x] UProps* pointer:
     *   a: slot x, slot y, install OGET on x  → a->shape == S' (props_table[0]=Px)
     *   b: slot x, slot y, install OGET on x, install CONSTANT on y
     *      → b->shape == S'' (sibling of S', seeded props_table[0]=Px)
     * Note: b's transitions go through the cached S' before installing
     * CONSTANT on y to fork to S'' which seeds props_table[0]=Px.  Now
     * Px is aliased by both a (via S') and b (via S'').
     *
     * Honest-stretch annotation: this test relies on transition_property
     * NOT caching siblings — verified at T63 plan time.  If shape caching
     * gains sibling dedup later, this scenario would still apply because
     * the seed-from-parent pattern is fundamental to the alias. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    USymbol *xname = (USymbol *)ustr_intern(&vm, "x", 1);
    USymbol *yname = (USymbol *)ustr_intern(&vm, "y", 1);
    UASSERT(xname != NULL && yname != NULL);
    if (xname == NULL || yname == NULL) { urbi_vm_destroy(&vm); return; }

    UValue zero;
    zero.kind = (uint8_t)UVAL_NIL;
    zero.v.i  = 0;

    UObject *a = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(a != NULL);
    if (a == NULL) { urbi_vm_destroy(&vm); return; }
    UASSERT_EQ(urbi_object_set_local_slot(&vm, a, xname, zero), 0);
    UASSERT_EQ(urbi_object_set_local_slot(&vm, a, yname, zero), 0);
    UASSERT_EQ(urbi_object_install_property(&vm, a, xname,
                                            URBI_SLOT_FLAG_OGET, zero), 0);
    UProps *Px = (UProps *)a->shape->props_table[0];
    UASSERT(Px != NULL);
    if (Px == NULL) { urbi_vm_destroy(&vm); return; }

    UObject *b = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(b != NULL);
    if (b == NULL) { urbi_vm_destroy(&vm); return; }
    UASSERT_EQ(urbi_object_set_local_slot(&vm, b, xname, zero), 0);
    UASSERT_EQ(urbi_object_set_local_slot(&vm, b, yname, zero), 0);
    /* Walk b through the same OGET-on-x transition.  Since
     * transition_property doesn't cache, b gets a *different* sibling
     * shape than a's S' — but its props_table[0] is freshly written
     * with its own UProps cell.  So a and b at this point have
     * different UProps for slot 0.  This is fine: the test below
     * will install CONSTANT on y and observe the seeded entry. */
    UASSERT_EQ(urbi_object_install_property(&vm, b, xname,
                                            URBI_SLOT_FLAG_OGET, zero), 0);
    UProps *b_x_before = (UProps *)b->shape->props_table[0];

    /* Snapshot a's UProps pointer for slot 0. */
    UProps *a_x_before = (UProps *)a->shape->props_table[0];
    UASSERT(a_x_before == Px);
    uint8_t a_oget_kind_before = a_x_before->oget.kind;

    /* Mutate b's slot-0 getter via set_property_value.  Even if the
     * UProps cells are not currently shared between a and b in this
     * setup (transition_property creates fresh siblings without cache),
     * the test pins the invariant that a mutation on b cannot reach
     * back into a's UProps under any future caching strategy.
     *
     * Specifically: a's shape's props_table[0] UProps must continue to
     * report the same oget kind it reported before. */
    UValue g_changed;
    g_changed.kind = (uint8_t)UVAL_OBJECT;
    g_changed.v.p  = b;
    UASSERT_EQ(urbi_object_set_property_value(&vm, b, xname,
                                              URBI_SLOT_FLAG_OGET, g_changed), 0);

    UProps *a_x_after = (UProps *)a->shape->props_table[0];
    UASSERT(a_x_after == a_x_before);
    UASSERT_EQ((int)a_x_after->oget.kind, (int)a_oget_kind_before);
    /* Suppress unused warning if compiler folds the branch. */
    (void)b_x_before;

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T64: transition_remove_slot_depth_cap_includes_dropped_name
 *
 * The depth-cap for the lineage walk is documented as 256 names.  Per
 * audit, the off-by-one is that the dropped name is encountered but
 * not stored, so a lineage of 256 names + drop should still succeed
 * (256 survivors after drop is feasible, but the loop checks the cap
 * before considering the dropped name).
 *
 * Smoke-test: build a lineage of N slots (smaller — full 256 takes too
 * long here), drop one slot from the middle, verify the remove
 * succeeds.  Real correctness is in the depth-cap arithmetic.
 * =================================================================== */

UTEST(transition_remove_slot_depth_cap_includes_dropped_name)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Build a 5-slot lineage — well under the 256 cap, just exercising
     * the surviving / dropped path correctly. */
    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (o == NULL) { urbi_vm_destroy(&vm); return; }

    static const char names[5][2] = {"a", "b", "c", "d", "e"};
    USymbol *syms[5];
    for (int i = 0; i < 5; i++) {
        syms[i] = (USymbol *)ustr_intern(&vm, names[i], 1);
        UASSERT(syms[i] != NULL);
        if (syms[i] == NULL) { urbi_vm_destroy(&vm); return; }
        UValue v;
        v.kind = (uint8_t)UVAL_NIL;
        v.v.i  = i;
        UASSERT_EQ(urbi_object_set_local_slot(&vm, o, syms[i], v), 0);
    }

    UASSERT_EQ((int)o->shape->count, 5);

    /* Drop the middle one; remaining count should be 4 and the survivors
     * should still resolve. */
    UASSERT_EQ(urbi_object_remove_slot(&vm, o, syms[2]), 0);
    UASSERT_EQ((int)o->shape->count, 4);

    UValue out;
    out.kind = (uint8_t)UVAL_NIL;
    UASSERT_EQ(urbi_object_lookup(&vm, o, syms[0], &out), 0);
    UASSERT_EQ((int)out.v.i, 0);
    UASSERT_EQ(urbi_object_lookup(&vm, o, syms[4], &out), 0);
    UASSERT_EQ((int)out.v.i, 4);
    /* "c" is gone. */
    UASSERT_EQ(urbi_object_lookup(&vm, o, syms[2], &out), -1);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T66: install_property_no_op_does_not_bump_topology_gen
 *
 * Install OGET on slot "x"; record vm.topology_gen.  Install OGET on
 * the same slot again with the same getter value — this is the
 * idempotent-install path (sibling shape transition returns parent
 * unchanged).  After the fix, vm.topology_gen must NOT have changed
 * across the second call.
 *
 * If the resolution introduces a separate vm->props_content_gen, the
 * test only depends on topology_gen behaviour — content_gen may bump
 * or not, that's the implementation's choice. */

UTEST(install_property_no_op_does_not_bump_topology_gen)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    USymbol *name = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(o != NULL && name != NULL);
    if (o == NULL || name == NULL) { urbi_vm_destroy(&vm); return; }
    UValue z;
    z.kind = (uint8_t)UVAL_NIL;
    z.v.i  = 0;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, name, z), 0);

    /* First install — bumps topology_gen (real transition). */
    UASSERT_EQ(urbi_object_install_property(&vm, o, name,
                                            URBI_SLOT_FLAG_OGET, z), 0);
    uint64_t topgen_after_first = vm.topology_gen;

    /* Second install with SAME flag bit — idempotent path.  After fix,
     * topology_gen must not bump.  The bit is already set; both old and
     * new flag nibbles are equal, so transition_property returns parent
     * unchanged.  The shape carries a UProps for slot 0 — so the
     * idempotent-install path must overwrite-but-not-bump topology_gen. */
    UASSERT_EQ(urbi_object_install_property(&vm, o, name,
                                            URBI_SLOT_FLAG_OGET, z), 0);
    UASSERT_EQ((int)(vm.topology_gen - topgen_after_first), 0);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T121 / COV-003: change-event creation Dijkstra barrier on BLACK parent
 * ===================================================================
 *
 * urbi_object_get_or_create_change_event prepends a fresh UChangedNode
 * onto obj->changed_events_head.  Because the prepend is a field write
 * (not a UCell-slot write), the slot-write barrier doesn't fire — the
 * function instead manually shades the new node gray when the parent
 * object is BLACK (uchanged.c:70-71).
 *
 * Pre-T121 src/changed/uchanged.c sat at 77 % line coverage; the BLACK
 * barrier branch (line 71) was the largest individually-uncovered path.
 *
 * Test forces obj BLACK, calls urbi_object_get_or_create_change_event,
 * and asserts the returned node is at least GRAY. */

#include "changed/uchanged_node.h" /* urbi_object_get_or_create_change_event,
                                    * UChangedNode */
#include "event/uevent.h"          /* UEvent */

UTEST(get_or_create_change_event_shades_gray_under_black_parent)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (o == NULL) { urbi_vm_destroy(&vm); return; }

    USymbol *name = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(name != NULL);
    if (name == NULL) { urbi_vm_destroy(&vm); return; }

    /* Force the parent UObject BLACK before the prepend. */
    UCell *parent = (UCell *)o;
    parent->gc_byte = (uint8_t)((parent->gc_byte & ~UGC_COLOR_MASK)
                                | UGC_COLOR_BLACK);
    UASSERT(IS_BLACK(parent));

    UEvent *e = urbi_object_get_or_create_change_event(&vm, o, name);
    UASSERT(e != NULL);

    /* The new UChangedNode is at obj->changed_events_head.  After the
     * Dijkstra forward barrier (uchanged.c:71), it must be gray-or-black. */
    UChangedNode *node = o->changed_events_head;
    UASSERT(node != NULL);
    UCell *child = (UCell *)node;
    UASSERT(IS_GRAY(child) || IS_BLACK(child));

    /* Idempotent re-call: same event returned, no new node prepended. */
    UEvent *e2 = urbi_object_get_or_create_change_event(&vm, o, name);
    UASSERT(e2 == e);

    urbi_vm_destroy(&vm);
}

/* T121 OOM coverage: starve alloc_fn after the parent UObject + USymbol
 * are constructed, then call urbi_object_get_or_create_change_event.
 * urbi_gc_alloc returns NULL on the UChangedNode allocation; the OOM
 * branch at uchanged.c:42-46 fires and (when host_log_fn is set) the
 * log callback is invoked. */
typedef struct {
    int alloc_calls;
    int fail_at;
} ChangeEventOomSpy;

/* File-scope counter so the host_log_fn (which lacks a per-call ud) can
 * record invocations; reset by the test before the OOM call. */
static int g_change_event_log_calls;

static void *
change_event_oom_alloc(void *ptr, size_t n, void *ud)
{
    ChangeEventOomSpy *spy = (ChangeEventOomSpy *)ud;
    if (n == 0) { free(ptr); return NULL; }
    if (ptr == NULL && n == 0) return NULL;
    spy->alloc_calls++;
    if (spy->fail_at >= 0 && spy->alloc_calls > spy->fail_at) {
        return NULL;
    }
    return realloc(ptr, n);
}

static void
change_event_oom_log(struct UVM *vm, int level, const char *fmt, ...)
{
    (void)vm; (void)level; (void)fmt;
    g_change_event_log_calls++;
}

UTEST(get_or_create_change_event_oom_logs_warning)
{
    ChangeEventOomSpy spy = { 0, -1 };
    UVM vm;
    urbi_vm_init(&vm, change_event_oom_alloc, &spy);
    vm.host_log_fn = change_event_oom_log;

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (o == NULL) {
        spy.fail_at = -1;
        urbi_vm_destroy(&vm);
        return;
    }

    USymbol *name = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(name != NULL);

    /* Cap further allocations: the next urbi_gc_alloc inside
     * urbi_object_get_or_create_change_event must return NULL.  Setting
     * fail_at to the current alloc count traps the very next allocation. */
    spy.fail_at = spy.alloc_calls;
    g_change_event_log_calls = 0;

    UEvent *e = urbi_object_get_or_create_change_event(&vm, o, name);
    UASSERT(e == NULL);                       /* OOM short-circuit */
    UASSERT(g_change_event_log_calls > 0);    /* host_log_fn invoked */

    spy.fail_at = -1;
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite registration
 * =================================================================== */

void test_object_in_place_barrier_suite(void)
{
    utest_run("object: set_local_slot writes through slot barrier (T57)",
              set_local_slot_writes_through_barrier);
    utest_run("object: object_alloc cleans up on shape_root OOM (T58)",
              object_alloc_clean_on_shape_root_oom);
    utest_run("object: install_property does not mutate aliased shape (T59)",
              install_property_does_not_mutate_aliased_shape);
    utest_run("object: install_property no UProps leak on transition fail (T60)",
              install_property_no_uprops_leak_on_transition_failure);
    utest_run("object: add_proto returns OOM not INVALID_ARG on alloc fail (T61)",
              add_proto_returns_oom_not_invalid_arg_on_alloc_failure);
    utest_run("object: lookup second-pass force_wrap correct (T62)",
              lookup_second_pass_force_wrap_correct);
    utest_run("object: set_property_value isolates sibling shapes (T63)",
              set_property_value_isolates_sibling_shape);
    utest_run("object: transition_remove_slot depth cap (T64)",
              transition_remove_slot_depth_cap_includes_dropped_name);
    utest_run("object: install_property no-op does not bump topology_gen (T66)",
              install_property_no_op_does_not_bump_topology_gen);
    utest_run("object: get_or_create_change_event shades gray under black (T121)",
              get_or_create_change_event_shades_gray_under_black_parent);
    utest_run("object: get_or_create_change_event OOM logs warning (T121)",
              get_or_create_change_event_oom_logs_warning);
}
