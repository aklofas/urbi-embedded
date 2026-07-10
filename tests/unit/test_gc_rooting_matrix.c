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
#include "vm/uvm_internal.h"  /* urbi_vm_open_upvalue (upvalue-close barrier case) */
#include "sched/ustrand.h"
#include "realm/urealm.h"
#include "object/uobject.h"   /* struct UObject (sentinel payload size) */
#include "object/uobject_internal.h" /* urbi_protos_alloc (UProtos wrapper case) */
#include "object/ushape.h"   /* urbi_shape_find_slot (property-path cases) */
#include "runtime/uclosure.h" /* UClosure.native_fn (dict container cases) */
#include "stdlib/containers.h" /* urbi_stdlib_list_new_empty / _append_value */
#include "value/uintern.h"    /* ustr_intern (dict keys + native-fn lookup) */
#include "runtime/ucleanup.h" /* strand_cleanup_* (cleanup owning_tag case) */
#include "tag/utag.h"         /* utag_create (owning_tag + suspend_tag cases) */
#include "utest_e2e_helpers.h" /* utest_e2e_compile_and_run (cleanup saved-value case) */
#include "watcher/uwatcher.h" /* uwatcher_pool_alloc + UWatcher (watcher root cases) */
#include "event/uevent.h"     /* urbi_event_create (event-watcher case) */
#include "event/uevent_subscribe.h" /* uevent_at_watchers_append (event-watcher case) */
#include "stdlib/temporal.h"  /* UPeriodic (periodic owning_tag case) */
#include "stdlib/object_root.h" /* urbi_native_closure_create (watcher closure cases) */

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
 * realm so urbi_gc_sched_walk_roots → strand_walk_roots visits it:
 *   1. Lazy-create global_realm via urbi_realm_global.
 *   2. Stack-allocate the UStrand (caller provides it); zero it.
 *   3. Allocate a register stack via the VM allocator (mimics
 *      urbi_strand_register_stack_alloc); zero it.
 *   4. Link onto gr->strands_head with next_in_realm.
 * Teardown unlinks symmetrically and frees the register stack. */
static void matrix_strand_setup(UVM *vm, UStrand *s)
{
    /* Disarm stress mode for the lazy realm bootstrap only.  Historical:
     * before the v0.13.2 T14 fixes (link-first realm create + bootstrap
     * GC pause) the realm-population path crashed under URBI_GC_STRESS;
     * that is fixed and pinned by test_realm.c's
     * stress_realm_create_survives_collect_per_alloc.  The disarm is kept
     * so the matrix's own setup cost stays low (armed bootstrap runs a
     * full collection per allocation); the matrix cases themselves
     * (sentinel alloc + forced collections) run fully armed. */
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
    int rc = urbi_sched_strand_cleanup_stack_init(&s, &vm, (uint16_t)URBI_CLEANUP_MAX);
    UASSERT_EQ(rc, 0);

    /* Push FIRST (pure index bump, no allocation), THEN create the tag,
     * THEN store: under URBI_GC_STRESS utag_create force-collects before
     * allocating, and nothing may allocate between utag_create returning
     * and the owning_tag store while the tag is unrooted. */
    UCleanupEntry *entry = urbi_sched_strand_cleanup_push(&s);
    UASSERT(entry != NULL);
    entry->kind        = (uint8_t)UCLEANUP_TAG_SCOPE;
    entry->strand_back = &s;   /* mirrors urbi_vm_push_tag_scope; the remaining
                                  fields keep their zero/NULL init values.
                                  The real anonymous arm also threads
                                  tag->member_strands_head = entry — omitted
                                  here deliberately so teardown needs no
                                  unlink (walk_utag never traverses that
                                  chain; GC reachability is unaffected). */

    UTag *tag = utag_create(&vm);
    UASSERT(tag != NULL);
    entry->owning_tag = tag;   /* tag's ONLY reference (the anonymous
                                  per-scope shape urbi_vm_push_tag_scope builds) */

    collect_twice(&vm);
    UASSERT(cell_live(&vm, (UCell *)tag));

    /* Clear + pop before teardown so nothing later treats the slot as a
     * live tag scope; the now-unreferenced tag is reclaimed by a later
     * cycle (GC owns it — no utag_destroy here). */
    entry->owning_tag = NULL;
    urbi_sched_strand_cleanup_pop(&s, UCLEANUP_TAG_SCOPE);
    urbi_sched_strand_cleanup_stack_destroy(&s, &vm);
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

/* === Transitively-covered strand fields (frames[i].recv / unwind_target /
 *     wait_event_target) ===
 *
 * These three fields are NOT shaded directly by strand_walk_roots.  Each holds
 * a copy or back-pointer whose referent is kept alive by a DIFFERENT covering
 * root, so the honest matrix contract for them is: with the covering root in
 * place the referent survives (GREEN); with the covering root absent the SAME
 * referent is swept (RED).  Both halves run in one case so the "transitively
 * covered, not independently rooted" invariant is pinned in a single assert
 * pair — a future regression that either (a) starts relying on a direct shade
 * that is not there, or (b) drops the covering root, flips exactly one half.
 *
 * The covering roots, verified against strand_walk_roots
 * (src/sched/usched_cooperative.c):
 *   - frames[i].recv       — the recv UValue is a copy of the caller's R[A+1];
 *                            it survives via the register-window scan, step (1).
 *   - unwind_target        — a tag-stop deposit's target tag also owns a
 *                            TAG_SCOPE cleanup entry on the strand; that
 *                            owning_tag is shaded in step (3).
 *   - wait_event_target    — the waited-on UEvent is reachable through another
 *                            path (register / realm globals / subscriber list),
 *                            NOT the back-pointer; step (4) documents this.
 */

UTEST(matrix_frame_recv_covered_by_register_scan)
{
    /* GREEN: recv holds the sentinel AND the same value is live in a register
     * (the caller's R[A+1] that recv was copied from), so the register-window
     * scan keeps it alive. */
    {
        UVM vm;
        urbi_vm_init(&vm, NULL, NULL);
        UStrand s;
        matrix_strand_setup(&vm, &s);

        UCell *sentinel = sentinel_alloc(&vm);
        UASSERT(sentinel != NULL);
        s.stack[5]         = obj_value_for(sentinel);   /* the covering register */
        s.frames[0].recv   = obj_value_for(sentinel);   /* frame recv copy */
        s.frame_count      = 1;

        collect_twice(&vm);
        UASSERT(cell_live(&vm, sentinel));

        memset(&s.stack[5], 0, sizeof s.stack[5]);
        memset(&s.frames[0].recv, 0, sizeof s.frames[0].recv);
        s.frame_count = 0;
        matrix_strand_teardown(&vm, &s);
        urbi_vm_destroy(&vm);
    }

    /* RED-proof: recv is the ONLY reference (no register copy).  recv is not
     * shaded, so the sentinel is swept — proving recv is transitively covered
     * by the register, not an independent root. */
    {
        UVM vm;
        urbi_vm_init(&vm, NULL, NULL);
        UStrand s;
        matrix_strand_setup(&vm, &s);

        UCell *sentinel = sentinel_alloc(&vm);
        UASSERT(sentinel != NULL);
        s.frames[0].recv = obj_value_for(sentinel);   /* recv ONLY, no register */
        s.frame_count    = 1;

        collect_twice(&vm);
        UASSERT(!cell_live(&vm, sentinel));

        memset(&s.frames[0].recv, 0, sizeof s.frames[0].recv);
        s.frame_count = 0;
        matrix_strand_teardown(&vm, &s);
        urbi_vm_destroy(&vm);
    }
}

UTEST(matrix_unwind_target_covered_by_cleanup_scope)
{
    /* GREEN: the tag-stop target tag also owns a TAG_SCOPE cleanup entry, whose
     * owning_tag is shaded in strand_walk_roots step (3).  Mirrors the deposit
     * shape of urbi_tag_stop (pending_unwind = UEXEC_TAG_STOP; unwind_target =
     * tag) plus the member strand's TAG_SCOPE entry. */
    {
        UVM vm;
        urbi_vm_init(&vm, NULL, NULL);
        UStrand s;
        matrix_strand_setup(&vm, &s);

        int rc = urbi_sched_strand_cleanup_stack_init(&s, &vm,
                                                      (uint16_t)URBI_CLEANUP_MAX);
        UASSERT_EQ(rc, 0);

        /* Push the cleanup entry FIRST (pure index bump, no allocation), THEN
         * create the tag, THEN wire both the cleanup owning_tag and the
         * unwind_target — under URBI_GC_STRESS utag_create force-collects
         * before allocating, and nothing allocates between the tag birth and
         * the two stores while the tag is unrooted. */
        UCleanupEntry *entry = urbi_sched_strand_cleanup_push(&s);
        UASSERT(entry != NULL);
        entry->kind        = (uint8_t)UCLEANUP_TAG_SCOPE;
        entry->strand_back = &s;

        UTag *tag = utag_create(&vm);
        UASSERT(tag != NULL);
        entry->owning_tag = tag;              /* the covering root (step 3) */
        s.pending_unwind  = UEXEC_TAG_STOP;
        s.unwind_target   = tag;              /* the field under test */

        collect_twice(&vm);
        UASSERT(cell_live(&vm, (UCell *)tag));

        s.unwind_target  = NULL;
        s.pending_unwind = UEXEC_OK;
        entry->owning_tag = NULL;
        urbi_sched_strand_cleanup_pop(&s, UCLEANUP_TAG_SCOPE);
        urbi_sched_strand_cleanup_stack_destroy(&s, &vm);
        matrix_strand_teardown(&vm, &s);
        urbi_vm_destroy(&vm);
    }

    /* RED-proof: unwind_target is the ONLY reference (no cleanup entry).
     * unwind_target is not shaded, so the tag is swept. */
    {
        UVM vm;
        urbi_vm_init(&vm, NULL, NULL);
        UStrand s;
        matrix_strand_setup(&vm, &s);

        UTag *tag = utag_create(&vm);
        UASSERT(tag != NULL);
        s.pending_unwind = UEXEC_TAG_STOP;
        s.unwind_target  = tag;               /* unwind_target ONLY */

        collect_twice(&vm);
        UASSERT(!cell_live(&vm, (UCell *)tag));

        s.unwind_target  = NULL;
        s.pending_unwind = UEXEC_OK;
        matrix_strand_teardown(&vm, &s);
        urbi_vm_destroy(&vm);
    }
}

UTEST(matrix_wait_event_target_covered_by_event_reachability)
{
    /* GREEN: the waited-on event is reachable through another path (here a
     * register holding the event value); wait_event_target is a back-pointer
     * for unregister, not a root.  Mirrors c_event_waituntil: state ==
     * USTRAND_WAIT_EVENT, wait_event_target == the event. */
    {
        UVM vm;
        urbi_vm_init(&vm, NULL, NULL);
        UStrand s;
        matrix_strand_setup(&vm, &s);

        UEvent *ev = urbi_event_create(&vm);
        UASSERT(ev != NULL);
        {
            UValue evv;
            memset(&evv, 0, sizeof evv);
            evv.kind = UVAL_EVENT;
            evv.v.p  = (void *)ev;
            s.stack[6] = evv;                 /* the covering register */
        }
        uint8_t state_saved  = s.state;
        s.state              = (uint8_t)USTRAND_WAIT_EVENT;
        s.wait_event_target  = ev;            /* the field under test */

        collect_twice(&vm);
        UASSERT(cell_live(&vm, (UCell *)ev));

        s.wait_event_target = NULL;
        s.state             = state_saved;
        memset(&s.stack[6], 0, sizeof s.stack[6]);
        matrix_strand_teardown(&vm, &s);
        urbi_vm_destroy(&vm);
    }

    /* RED-proof: wait_event_target is the ONLY reference (event on no other
     * path).  The back-pointer is not shaded, so the event is swept. */
    {
        UVM vm;
        urbi_vm_init(&vm, NULL, NULL);
        UStrand s;
        matrix_strand_setup(&vm, &s);

        UEvent *ev = urbi_event_create(&vm);
        UASSERT(ev != NULL);
        uint8_t state_saved  = s.state;
        s.state              = (uint8_t)USTRAND_WAIT_EVENT;
        s.wait_event_target  = ev;            /* back-pointer ONLY */

        collect_twice(&vm);
        UASSERT(!cell_live(&vm, (UCell *)ev));

        s.wait_event_target = NULL;
        s.state             = state_saved;
        matrix_strand_teardown(&vm, &s);
        urbi_vm_destroy(&vm);
    }
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

    /* Finish the cycle.  Unbounded budget per slice (the urbi_gc_force_full
     * idiom) so each spin completes whole phases regardless of heap size:
     * gated builds boot a larger urbi_vm_init heap, and a fixed 4096-byte
     * budget needed >64 slices just to mark+sweep it (the gate-off build
     * finished at 60 of 64 — no headroom).  The spin cap stays purely as a
     * wedge detector: UASSERT records the failure but does NOT abort the
     * case (utest.h), so the explicit break is what actually bounds a
     * wedge regression (phase never reaching IDLE) instead of hanging
     * CI. */
    {
        int spins = 0;
        while (urbi_gc_phase(&vm) != (uint8_t)GC_PHASE_IDLE) {
            urbi_gc_slice(&vm, (size_t)-1U);
            spins++;
            UASSERT(spins < 64);
            if (spins >= 64) break;
        }
    }

    UASSERT(cell_live(&vm, sentinel));

    memset(&s.stack[0], 0, sizeof s.stack[0]);
    memset(&s.stack[1], 0, sizeof s.stack[1]);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

/* === C-stack root frame case (Task 6 / refactor-3 VM-06a) ===
 *
 * Full push/pop lifecycle contract of the UCRootFrame chain
 * (src/sched/ustrand.h): while a frame is pushed, the chained slot is a
 * root (the sentinel's ONLY reference is the C-stack UValue local);
 * after the pop, the slot is invisible again and the sentinel is swept. */

UTEST(matrix_c_root_frame_roots_value)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    UCell *sentinel = sentinel_alloc(&vm);
    UASSERT(sentinel != NULL);
    UValue held = obj_value_for(sentinel);   /* sentinel's ONLY reference */
    UCRootFrame frame;
    ustrand_c_root_push(&s, &frame, &held);

    collect_twice(&vm);
    UASSERT(cell_live(&vm, sentinel));

    ustrand_c_root_pop(&s, &frame);
    urbi_gc_force_full(&vm);
    UASSERT(!cell_live(&vm, sentinel));

    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

/* === Cleanup-executor saved-value case (Task 6 / refactor-3 VM-06a) ===
 *
 * run_cleanup_with_replace (src/runtime/uunwind.c) stashes the suppressed
 * unwind value in a C local (`UValue saved_value`) while the finally body
 * runs via a nested dispatch — and a GC inside that dispatch cannot see C
 * locals (no conservative C-stack scan in this GC).  With the try scope's
 * registers already zeroed (Inv-5), the saved RETURN value's ONLY
 * reference is that C local: a collection mid-finally sweeps it, and the
 * walker then delivers a dangling pointer to the caller.
 *
 * GC pressure is applied surgically via a registered host fn (`gcNow`)
 * that force-collects twice from inside the finally body — deterministic
 * red on EVERY build, not just under URBI_GC_STRESS.  Full-stress script
 * runs are not usable here: realm population under URBI_GC_STRESS hits
 * the known pre-existing baseline boot crash (urbi_populate_realm_globals
 * → walk_ushape on a mid-construction cell — the same class that kills
 * string_literal_e2e), so the case runs with stress disarmed and the
 * forced double-collection in the finally provides strictly more pressure
 * at exactly the VM-06a window than stress mode would. */

static int host_gc_now(struct UVM *vm, UValue self,
                       UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)args; (void)nargs;
    urbi_gc_force_full(vm);
    urbi_gc_force_full(vm);
    *out = urbi_make_nil();
    return 0;
}

UTEST(matrix_cleanup_saved_value_survives_gc_in_cleanup_body)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Disarm stress for the whole case (see header comment): the realm
     * bootstrap inside the e2e run crashes under URBI_GC_STRESS (known
     * baseline), and gcNow() supplies the collection pressure at the one
     * point this case is about. */
    vm.gc_stress_armed = 0U;

    UASSERT_EQ(URBI_OK, urbi_register(&vm, NULL, "gcNow", host_gc_now));

    /* try { return Pair.new(1, 2) } finally { gcNow() }:
     * the RETURN deposits the pair into s->unwind_value; the unwind
     * walker zeroes the try scope's registers (Inv-5) and hands the value
     * to run_cleanup_with_replace's C local; gcNow() collects while it
     * sits there. */
    UValue result = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(
        &vm,
        "var f = function() { try { return Pair.new(1, 2) } "
        "finally { gcNow() } }; f()",
        &result);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_OBJECT, (int)result.kind);

    /* Liveness oracle: pre-fix the pair was swept mid-finally (off the
     * all-cells sidecar list); the walker then delivered a dangling
     * pointer.  Membership is checked WITHOUT dereferencing the cell so
     * the pre-fix red is a clean assert, not a UAF crash. */
    UASSERT(cell_live(&vm, result.v.p));

    /* Value integrity (guarded so the pre-fix red never dereferences the
     * swept cell): the survivor must still be the pair we returned. */
    if (cell_live(&vm, result.v.p)) {
        USymbol *sym_first = (USymbol *)ustr_intern(&vm, "first", 5);
        UASSERT(sym_first != NULL);
        UValue first = urbi_make_nil();
        UASSERT_EQ(0, urbi_object_lookup(&vm, (UObject *)result.v.p,
                                         sym_first, &first));
        UASSERT_EQ((int)UVAL_INT, (int)first.kind);
        UASSERT_EQ(1LL, (long long)first.v.i);
    }

    urbi_vm_destroy(&vm);
}

/* === Watcher pool + periodic cases (Task 7 / refactor-3 GC-05/GC-03) ===
 *
 * Pool-wide rooting contract: a watcher slot is a root while it is IN USE
 * (URBI_WATCHER_ACTIVE — set by uwatcher_pool_alloc, cleared only by
 * pool_free), regardless of which list (if any) threads it.  These cases
 * therefore construct bare pool slots WITHOUT any list linking: pre-fix,
 * the provider walked vm->active_watchers_head + vm->pending_onleave_head
 * only, so every case below was unrooted.
 *
 * Stress ordering: uwatcher_pool_alloc pops from the pre-allocated slab
 * freelist (no urbi_gc_alloc), so it never triggers a stress collection;
 * each GC-cell birth (utag_create / urbi_event_create /
 * urbi_native_closure_create) is immediately followed by its field store
 * with no allocation in between. */

/* Never invoked — the watcher cases only need a real GC-managed UClosure
 * cell (urbi_native_closure_create rejects a NULL fn). */
static int matrix_native_nop(struct UVM *vm, UValue self,
                             UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    *out = urbi_make_nil();
    return 0;
}

/* Return a pool slot through the production free path.  v0.13.3
 * (SCHED-06): BOTH install paths (urbi_watcher_install_watcher_runtime and
 * urbi_watcher_install_at_event_runtime) now increment vm->watchers->active_count — the
 * count covers all armed watchers — and urbi_watcher_unregister_internal's
 * decrement asserts > 0 instead of saturating.  These matrix cases use raw
 * uwatcher_pool_alloc (no installer ran), so mirror the installer's bump
 * here so the decrement balances. */
static void matrix_watcher_free(UVM *vm, UWatcher *w)
{
    vm->watchers->active_count++;
    urbi_watcher_unregister_internal(vm, w);
}

UTEST(matrix_watcher_owning_tag_is_rooted)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UWatcher *w = uwatcher_pool_alloc(&vm);
    UASSERT(w != NULL);

    UTag *tag = utag_create(&vm);
    UASSERT(tag != NULL);
    w->owning_tag = tag;   /* tag's ONLY reference */

    collect_twice(&vm);
    UASSERT(cell_live(&vm, (UCell *)tag));

    w->owning_tag = NULL;
    matrix_watcher_free(&vm, w);
    urbi_vm_destroy(&vm);
}

/* THE GC-05 case: an AT_EVENT watcher's closures were previously rooted
 * only via walk_uevent — i.e. only while the EVENT was independently
 * reachable.  Here the event's only reference is w->event and the
 * closure's only reference is w->body: pre-fix, walk_uevent never runs on
 * the unreachable event, so BOTH are swept (and w->event dangles at
 * unregister). */
UTEST(matrix_event_watcher_closures_rooted_without_reachable_event)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UWatcher *w = uwatcher_pool_alloc(&vm);
    UASSERT(w != NULL);
    w->mode = UWATCHER_AT_EVENT;

    UEvent *ev = urbi_event_create(&vm);
    UASSERT(ev != NULL);
    w->event = ev;                       /* event's ONLY reference */
    uevent_at_watchers_append(ev, w);    /* real subscriber threading */

    UClosure *cl = urbi_native_closure_create(&vm, matrix_native_nop);
    UASSERT(cl != NULL);
    w->body = cl;                        /* closure's ONLY reference */

    collect_twice(&vm);
    int ev_live = cell_live(&vm, (UCell *)ev);
    int cl_live = cell_live(&vm, (UCell *)cl);
    UASSERT(ev_live);
    UASSERT(cl_live);

    if (ev_live) {
        /* Production unregister: unlinks from ev->at_watchers_head and
         * returns the slot. */
        matrix_watcher_free(&vm, w);
    } else {
        /* Pre-fix red: ev was swept — drop the dangling pointers WITHOUT
         * dereferencing them so the red stays a clean assert (not a UAF),
         * and leave the slot ACTIVE: uwatcher_pool_destroy's slab walk
         * skips the event-unlink when w->event is NULL and pool_frees the
         * slot before the slab is released. */
        w->event = NULL;
        w->body  = NULL;
    }
    urbi_vm_destroy(&vm);
}

/* Pins the in-use predicate covering PENDING_UNREGISTER slots: a watcher
 * between stop-request and drain still needs its onleave closure alive
 * (urbi_watcher_drain_pending_onleave_queue will run it). */
UTEST(matrix_pending_unregister_watcher_still_rooted)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UWatcher *w = uwatcher_pool_alloc(&vm);
    UASSERT(w != NULL);
    w->flags |= URBI_WATCHER_PENDING_UNREGISTER;

    UClosure *cl = urbi_native_closure_create(&vm, matrix_native_nop);
    UASSERT(cl != NULL);
    w->onleave = cl;                     /* closure's ONLY reference */

    collect_twice(&vm);
    UASSERT(cell_live(&vm, (UCell *)cl));

    w->onleave = NULL;
    matrix_watcher_free(&vm, w);
    urbi_vm_destroy(&vm);
}

UTEST(matrix_periodic_owning_tag_is_rooted)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Mirror periodic_alloc (temporal.c): raw vm->alloc_fn record, zeroed,
     * head-inserted on vm->periodics_head.  Raw allocation — never a GC
     * cell, so no stress hazard. */
    UPeriodic *p = (UPeriodic *)vm.alloc_fn(NULL, sizeof(UPeriodic),
                                            vm.alloc_ud);
    UASSERT(p != NULL);
    memset(p, 0, sizeof *p);
    p->next = vm.periodics_head;
    vm.periodics_head = p;

    UTag *tag = utag_create(&vm);
    UASSERT(tag != NULL);
    p->owning_tag = tag;   /* tag's ONLY reference */

    collect_twice(&vm);
    UASSERT(cell_live(&vm, (UCell *)tag));

    /* Unthread + free before teardown (urbi_periodic_destroy_all must not
     * see a test-owned record). */
    vm.periodics_head = p->next;
    vm.alloc_fn(p, 0, vm.alloc_ud);
    urbi_vm_destroy(&vm);
}

/* === Upvalue-close write barrier case (Task 9 / refactor-3 GC-07) ===
 *
 * urbi_vm_close_upvalues copies *cell->u.stack_ptr into the UUpvalCell — a store
 * of a possibly-WHITE value into a possibly-BLACK cell.  The close path had
 * no barrier at all, so a value whose ONLY reference migrates into a black
 * closed cell mid-mark is never re-scanned and gets swept while reachable.
 * (OP_SETUPVAL had a barrier here but on the WRONG parent — the executing
 * closure instead of the shared cell; that half is Task 9c's case below.)
 *
 * Construction (extends the T4 slice-machine idiom):
 *   1. Plant pre-cycle: parker object in stack[0] (registers are shaded in
 *      ascending order by strand_walk_roots section (1) and the gray list
 *      is LIFO, so the FIRST-shaded root is traced LAST — the parker keeps
 *      the worklist non-empty while the carrier closure + upvalue cell
 *      blacken); carrier closure in stack[1] with nupvals=1 wired to an
 *      open upvalue over stack[2] (the way OP_CLOSURE publishes upvals[]);
 *      sentinel C2 held ONLY in a C local — invisible to every provider, so
 *      MARK_ROOTS leaves it white.  C2 is allocated PRE-cycle: the cycle
 *      flip (urbi_gc_slice IDLE arm) turns its birth color into the
 *      sweepable OTHER_WHITE; a mid-cycle newborn would carry current_white
 *      and survive the sweep regardless of the barrier (no red).
 *   2. Start a cycle (gc_debt poke + one slice → MARK_INCREMENTAL), then
 *      drain budget-1 slices (one gray pop each) until closure AND cell are
 *      BLACK with the parker still gray (phase stays MARK_INCREMENTAL).
 *   3. Mutator move: stack[2] = C2 (register write — deliberately
 *      barrier-free), then urbi_vm_close_upvalues copies C2 into the BLACK cell
 *      and unlinks it from open_upvals.  Pre-fix: no shade.
 *   4. Nil stack[2]: the closed cell's u.value is now C2's ONLY reference,
 *      and the ATOMIC_FINISH root re-scan (T4) sees neither the register
 *      (nil) nor the cell (unlinked from the section-(8) chain; the black
 *      carrier closure is skipped by mark_root_callback's idempotency
 *      check, so it is never re-traced).
 *   5. Finish the cycle.  Pre-fix: C2 is OTHER_WHITE at SWEEP → IS_DEAD →
 *      freed while reachable via closure → upvals[0] → u.value (assert
 *      fails).  Post-fix: the close-path barrier shades C2 gray.
 *
 * Stress-build safety: every pre-cycle allocation is rooted before the next
 * allocation (C2, the last birth, needs no root: nothing allocates between
 * its birth and the case's hand-driven cycle, and the in-cycle mutator
 * section performs no allocation at all). */

UTEST(matrix_upvalue_close_fires_barrier)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    /* Parker: first-shaded root → traced last (see header). */
    UCell *parker = sentinel_alloc(&vm);
    UASSERT(parker != NULL);
    s.stack[0] = obj_value_for(parker);

    /* Carrier closure: a real GC-managed UClosure (native variant — proto
     * NULL, so uclosure_destroy is a no-op).  sizeof(UClosure) already
     * includes the upvals[1] trailing slot, so wiring one upval into a
     * native closure stays in-bounds. */
    UClosure *cl = urbi_native_closure_create(&vm, matrix_native_nop);
    UASSERT(cl != NULL);
    {
        UValue clv;
        memset(&clv, 0, sizeof clv);
        clv.kind = (uint8_t)UVAL_CLOSURE;
        clv.v.p  = (void *)cl;
        s.stack[1] = clv;
    }

    /* Open upvalue over R[2] via the production path (links s.open_upvals,
     * rooted via strand_walk_roots section (8)); publish it in upvals[]
     * the way OP_CLOSURE does. */
    UUpvalCell *uc = urbi_vm_open_upvalue(&vm, &s, &s.stack[2]);
    UASSERT(uc != NULL);
    cl->nupvals   = 1U;
    cl->upvals[0] = uc;

    /* C2: the lost-object candidate (see header for why pre-cycle). */
    UCell *c2 = sentinel_alloc(&vm);
    UASSERT(c2 != NULL);

    /* Start the cycle: flip + MARK_ROOTS in one slice (T4 idiom). */
    vm.gc_debt = 1;
    urbi_gc_slice(&vm, 1U);
    UASSERT_EQ((uint8_t)GC_PHASE_MARK_INCREMENTAL, urbi_gc_phase(&vm));
    UASSERT(IS_WHITE(c2));   /* unreferenced by any root → stayed white */

    /* Drain one gray pop per slice until carrier + cell are black.  The
     * parker (traced last) keeps the worklist non-empty, so the phase
     * cannot advance past MARK_INCREMENTAL before we stop. */
    {
        int spins = 0;
        while (!(IS_BLACK(&cl->cell) && IS_BLACK(&uc->cell))
               && urbi_gc_phase(&vm) == (uint8_t)GC_PHASE_MARK_INCREMENTAL
               && spins < 100000) {
            urbi_gc_slice(&vm, 1U);
            spins++;
        }
        UASSERT(spins < 100000);
    }
    UASSERT(IS_BLACK(&cl->cell));
    UASSERT(IS_BLACK(&uc->cell));
    UASSERT_EQ((uint8_t)GC_PHASE_MARK_INCREMENTAL, urbi_gc_phase(&vm));
    UASSERT(vm.gray_work_head != NULL);   /* parker still parked */
    UASSERT(IS_WHITE(c2));                /* still untouched by the mark */

    /* Mutator move: load C2 into the captured register (barrier-free by
     * design), then close — the close copies *stack_ptr into the BLACK
     * cell.  Pre-fix: no shade fires here. */
    s.stack[2] = obj_value_for(c2);
    urbi_vm_close_upvalues(&s, &s.stack[2]);
    UASSERT(uc->on_heap);
    UASSERT(s.open_upvals == NULL);   /* unlinked — section (8) re-scan
                                         cannot rescue it */

    /* Nil the register BEFORE the ATOMIC_FINISH root re-scan runs: the
     * closed cell's u.value is now C2's ONLY reference. */
    memset(&s.stack[2], 0, sizeof s.stack[2]);

    /* Finish the cycle (T4 idiom: wedge-capped unbounded drain). */
    {
        int spins = 0;
        while (urbi_gc_phase(&vm) != (uint8_t)GC_PHASE_IDLE) {
            /* Unbounded budget (urbi_gc_force_full idiom): heap size must
             * not dictate the spin count (see T4 drain-loop comment). */
            urbi_gc_slice(&vm, (size_t)-1U);
            spins++;
            UASSERT(spins < 64);
            if (spins >= 64) break;
        }
    }

    /* Pre-fix: C2 swept while reachable (black cell never re-scanned).
     * Post-fix: the close-path Dijkstra barrier shaded it gray. */
    UASSERT(cell_live(&vm, c2));

    memset(&s.stack[0], 0, sizeof s.stack[0]);
    memset(&s.stack[1], 0, sizeof s.stack[1]);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

/* === Slot-store barrier OTHER_WHITE case (Task 9b / refactor-3 GC-07) ===
 *
 * The Dijkstra barrier predicate (uvalue_is_heap_white) tested
 * `color == current_white` only.  current_white flips at cycle START, so
 * mid-cycle the cells actually at risk of the sweep (IS_DEAD checks
 * OTHER_WHITE — i.e. everything allocated before the cycle and not yet
 * marked) read as "not white" and NO barrier site ever shaded them; the
 * sites only shaded mid-cycle newborns, which survive the sweep
 * regardless.  This case drives the canonical lost-object store through
 * urbi_gc_slot_store (the slot in-place-update path): BLACK parent object,
 * OTHER_WHITE child whose only other reference is a C local.
 *
 * Construction mirrors matrix_upvalue_close_fires_barrier: parker in
 * stack[0] pins the cycle in MARK_INCREMENTAL; holder P in stack[1] with
 * slot "x" pre-installed as nil (the mid-cycle store is then a pure
 * in-place update — no shape transition, no allocation); sentinel C2 born
 * pre-cycle, held only in a C local.  Drain until P is BLACK, store C2
 * into P.x via urbi_object_set_local_slot (Case 1 → urbi_gc_slot_store),
 * finish the cycle.  P is black and never re-traced; the ATOMIC_FINISH
 * re-scan covers ROOTS only (stack[1] holds the already-black P, which
 * mark_root_callback's idempotency check skips) — so the barrier is the
 * only thing standing between C2 and the sweep.  Red pre-fix, green once
 * the predicate covers both whites. */

UTEST(matrix_slot_store_barrier_shades_other_white)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    /* Intern the slot symbol up front (interning may allocate). */
    USymbol *sym = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(sym != NULL);

    /* Parker: first-shaded root → traced last (see upvalue-close case). */
    UCell *parker = sentinel_alloc(&vm);
    UASSERT(parker != NULL);
    s.stack[0] = obj_value_for(parker);

    /* P: slot-capable holder rooted in stack[1]; pre-install "x" = nil. */
    UObject *holder = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(holder != NULL);
    s.stack[1] = obj_value_for((UCell *)(void *)holder);
    UASSERT_EQ(0, urbi_object_set_local_slot(&vm, holder, sym,
                                             urbi_make_nil()));

    /* C2: pre-cycle birth → OTHER_WHITE once the cycle flips; only
     * reference is this C local (invisible to every root provider). */
    UCell *c2 = sentinel_alloc(&vm);
    UASSERT(c2 != NULL);

    /* Start the cycle: flip + MARK_ROOTS in one slice (T4 idiom). */
    vm.gc_debt = 1;
    urbi_gc_slice(&vm, 1U);
    UASSERT_EQ((uint8_t)GC_PHASE_MARK_INCREMENTAL, urbi_gc_phase(&vm));
    UASSERT(IS_WHITE(c2));

    /* Drain one gray pop per slice until P is BLACK (traced with "x"
     * still nil); the parker keeps the worklist non-empty. */
    {
        int spins = 0;
        while (!IS_BLACK((UCell *)(void *)holder)
               && urbi_gc_phase(&vm) == (uint8_t)GC_PHASE_MARK_INCREMENTAL
               && spins < 100000) {
            urbi_gc_slice(&vm, 1U);
            spins++;
        }
        UASSERT(spins < 100000);
    }
    UASSERT(IS_BLACK((UCell *)(void *)holder));
    UASSERT_EQ((uint8_t)GC_PHASE_MARK_INCREMENTAL, urbi_gc_phase(&vm));
    UASSERT(vm.gray_work_head != NULL);   /* parker still parked */
    UASSERT(IS_WHITE(c2));                /* still untouched by the mark */

    /* Mutator: in-place store of the OTHER_WHITE sentinel into the BLACK
     * parent's slot through the barriered path (no allocation — the slot
     * exists, so Case 1 / urbi_gc_slot_store).  Pre-fix the predicate
     * rejects C2 (not current_white) and no shade fires. */
    UASSERT_EQ(0, urbi_object_set_local_slot(&vm, holder, sym,
                                             obj_value_for(c2)));

    /* Finish the cycle (wedge-capped unbounded drain). */
    {
        int spins = 0;
        while (urbi_gc_phase(&vm) != (uint8_t)GC_PHASE_IDLE) {
            /* Unbounded budget (urbi_gc_force_full idiom): heap size must
             * not dictate the spin count (see T4 drain-loop comment). */
            urbi_gc_slice(&vm, (size_t)-1U);
            spins++;
            UASSERT(spins < 64);
            if (spins >= 64) break;
        }
    }

    /* Pre-fix: C2 swept while reachable via P.x (black parent never
     * re-scanned, root re-scan skips black P).  Post-fix: the barrier
     * shaded C2 gray at the store. */
    UASSERT(cell_live(&vm, c2));

    /* Teardown: drop the C2 ref + register roots; GC reclaims later. */
    UASSERT_EQ(0, urbi_object_set_local_slot(&vm, holder, sym,
                                             urbi_make_nil()));
    memset(&s.stack[0], 0, sizeof s.stack[0]);
    memset(&s.stack[1], 0, sizeof s.stack[1]);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

/* === OP_SETUPVAL shared-cell barrier case (Task 9c / refactor-3 GC-07) ===
 *
 * OP_SETUPVAL's barrier (urbi_gc_upvalue_pre_store) checked the EXECUTING
 * CLOSURE's color — but for the on_heap arm the store target is
 * uvc->u.value, i.e. the UUpvalCell, which is SHARED between sibling
 * closures (OP_CLOSURE re-capture arm copies the parent's cell pointer).
 * The cell's color diverges from any one closure's: sibling A traced →
 * shared cell BLACK; sibling B still GRAY executes the store → barrier
 * sees a gray closure parent → no shade; when B is traced later,
 * walk_uclosure's urbi_gc_shade_gray on the BLACK cell idempotency-skips →
 * the stored value is swept while reachable via the cell.  (The stack
 * arm needs nothing: it stores into a register, which the ATOMIC_FINISH
 * root re-scan covers.)
 *
 * Construction (parker + drain idiom from the T9 cases): closures B then
 * A rooted in stack[1]/stack[2] — registers shade ascending and the gray
 * list is LIFO, so A is traced BEFORE B; tracing A shades the shared cell
 * (closed pre-cycle, holding nil), which blackens on the next pop while B
 * is still gray and the parker keeps the cycle in MARK_INCREMENTAL.  Then
 * the mutator performs the exact CASE(OP_SETUPVAL) on_heap sequence with
 * cur_cl = B: barrier, store of the OTHER_WHITE sentinel into the black
 * cell, register cleared.  KEEP THE MIRRORED SEQUENCE IN LOCKSTEP WITH
 * CASE(OP_SETUPVAL) in src/vm/uvm.c.  Red pre-fix (closure-parent check
 * never fires), green once the barrier targets the cell. */

UTEST(matrix_setupval_barrier_targets_shared_cell)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    /* Parker: first-shaded root → traced last. */
    UCell *parker = sentinel_alloc(&vm);
    UASSERT(parker != NULL);
    s.stack[0] = obj_value_for(parker);

    /* Sibling closures: B in stack[1] (traced AFTER A), A in stack[2]
     * (traced first → blackens the shared cell). */
    UClosure *cl_b = urbi_native_closure_create(&vm, matrix_native_nop);
    UASSERT(cl_b != NULL);
    {
        UValue v;
        memset(&v, 0, sizeof v);
        v.kind = (uint8_t)UVAL_CLOSURE;
        v.v.p  = (void *)cl_b;
        s.stack[1] = v;
    }
    UClosure *cl_a = urbi_native_closure_create(&vm, matrix_native_nop);
    UASSERT(cl_a != NULL);
    {
        UValue v;
        memset(&v, 0, sizeof v);
        v.kind = (uint8_t)UVAL_CLOSURE;
        v.v.p  = (void *)cl_a;
        s.stack[2] = v;
    }

    /* Shared upvalue cell over R[3], published into A the way OP_CLOSURE's
     * in_stack arm does, then into B the way the re-capture arm does
     * (cl->upvals[i] = par_cl->upvals[src_idx] — same pointer). */
    UUpvalCell *uc = urbi_vm_open_upvalue(&vm, &s, &s.stack[3]);
    UASSERT(uc != NULL);
    cl_a->nupvals   = 1U;
    cl_a->upvals[0] = uc;
    cl_b->nupvals   = 1U;
    cl_b->upvals[0] = uc;

    /* Close pre-cycle (scope exit): u.value = nil, on_heap = true; the
     * cell is now reachable only via the two siblings' upvals[]. */
    urbi_vm_close_upvalues(&s, &s.stack[3]);
    UASSERT(uc->on_heap);

    /* C2: pre-cycle birth → OTHER_WHITE once the cycle flips; only
     * reference is this C local. */
    UCell *c2 = sentinel_alloc(&vm);
    UASSERT(c2 != NULL);

    /* Start the cycle: flip + MARK_ROOTS in one slice. */
    vm.gc_debt = 1;
    urbi_gc_slice(&vm, 1U);
    UASSERT_EQ((uint8_t)GC_PHASE_MARK_INCREMENTAL, urbi_gc_phase(&vm));
    UASSERT(IS_WHITE(c2));

    /* Drain until A AND the shared cell are BLACK; B must still be GRAY
     * (rooted, so shaded at MARK_ROOTS, but popped only after A). */
    {
        int spins = 0;
        while (!(IS_BLACK(&cl_a->cell) && IS_BLACK(&uc->cell))
               && urbi_gc_phase(&vm) == (uint8_t)GC_PHASE_MARK_INCREMENTAL
               && spins < 100000) {
            urbi_gc_slice(&vm, 1U);
            spins++;
        }
        UASSERT(spins < 100000);
    }
    UASSERT(IS_BLACK(&cl_a->cell));
    UASSERT(IS_BLACK(&uc->cell));
    UASSERT(IS_GRAY(&cl_b->cell));        /* executing closure NOT black */
    UASSERT_EQ((uint8_t)GC_PHASE_MARK_INCREMENTAL, urbi_gc_phase(&vm));
    UASSERT(vm.gray_work_head != NULL);   /* parker still parked */
    UASSERT(IS_WHITE(c2));

    /* Mutator: the exact CASE(OP_SETUPVAL) on_heap sequence with
     * cur_cl = cl_b (gray), uvc = uc (black shared cell), R[a] = R[3].
     * Pre-fix (closure-parent barrier: urbi_gc_upvalue_pre_store(vm, cl_b,
     * 0, R[a])) the gray closure never fired the shade; post-fix the
     * barrier targets the CELL. */
    s.stack[3] = obj_value_for(c2);       /* R[a] := sentinel */
    urbi_gc_upvalue_pre_store(&vm, &uc->cell, s.stack[3]);
    uc->u.value = s.stack[3];             /* on_heap arm store */
    memset(&s.stack[3], 0, sizeof s.stack[3]);

    /* Finish the cycle.  B's eventual trace shades the BLACK cell —
     * idempotency-skipped, so it cannot rescue C2. */
    {
        int spins = 0;
        while (urbi_gc_phase(&vm) != (uint8_t)GC_PHASE_IDLE) {
            /* Unbounded budget (urbi_gc_force_full idiom): heap size must
             * not dictate the spin count (see T4 drain-loop comment). */
            urbi_gc_slice(&vm, (size_t)-1U);
            spins++;
            UASSERT(spins < 64);
            if (spins >= 64) break;
        }
    }

    /* Pre-fix: C2 swept while reachable via the shared cell's u.value.
     * Post-fix: the cell-parent barrier shaded C2 gray at the store. */
    UASSERT(cell_live(&vm, c2));

    memset(&s.stack[0], 0, sizeof s.stack[0]);
    memset(&s.stack[1], 0, sizeof s.stack[1]);
    memset(&s.stack[2], 0, sizeof s.stack[2]);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

/* NOTE (Task 9b): no container sibling case.  The container element
 * barrier (container_element_pre_store, src/stdlib/containers.c) is
 * PACING-ONLY: container backing stores are walked as ROOTS every cycle
 * (urbi_stdlib_containers_walk_roots), and the GC-02 ATOMIC_FINISH
 * stop-the-world re-scan re-runs every provider — so an element stored
 * mid-cycle into any reachable container is re-discovered before SWEEP
 * regardless of the barrier.  A lost-object red is therefore not
 * constructible through the container path; the predicate fix still
 * applies there for pacing (shade at store time instead of deferring the
 * work to the atomic phase). */

/* === Suite entry point === */

/* === v0.13.2 T14 follow-up: property-path construction windows ===
 *
 * urbi_object_install_property / urbi_object_remove_property materialise
 * a sibling shape + props-table wrapper that are held ONLY in C locals
 * while uprops_alloc runs.  Property siblings are not cached in
 * parent->transitions (unlike add-slot children), so nothing else
 * reaches them mid-construction: under URBI_GC_STRESS the unpinned
 * sibling was swept and a dangling shape published into obj->shape
 * (review repro on remove: normal build prints shape count=1 flags=0,
 * stress build printed count=0 flags=6 and survived silently).  Both
 * cases run FULLY ARMED with the receiver rooted in a strand register,
 * so on a stress build every allocation inside the call collects with
 * the construction cells at peak exposure; on a normal build they are
 * semantic smokes. */
UTEST(matrix_install_property_pins_construction)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    UObject *o = (UObject *)(void *)sentinel_alloc(&vm);
    UASSERT(o != NULL);
    s.stack[0] = obj_value_for((UCell *)(void *)o);
    o->shape = urbi_shape_root(&vm);
    UASSERT(o->shape != NULL);

    USymbol *x = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(x != NULL);
    UValue v42;
    memset(&v42, 0, sizeof v42);
    v42.kind = UVAL_INT;
    v42.v.i  = 42;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, x, v42), 0);

    /* The call under test: armed install of CONSTANT on slot 0. */
    UASSERT_EQ(urbi_object_install_property(&vm, o, x,
                                            URBI_SLOT_FLAG_CONSTANT, v42),
               URBI_OK);

    /* Published shape is sane and its construction cells survived. */
    UASSERT_EQ((int)o->shape->count, 1);
    UASSERT_EQ((int)urbi_shape_find_slot(o->shape, x), 0);
    UASSERT((o->shape->flags & 0xFU) & URBI_SLOT_FLAG_CONSTANT);
    UASSERT(o->shape->props_table != NULL);
    UASSERT(o->shape->props_table[0] != NULL);
    UASSERT_EQ((unsigned)o->shape->props_table[0]->constant, 1U);
    UASSERT(cell_live(&vm, o->shape));

    collect_twice(&vm);
    UASSERT(cell_live(&vm, o->shape));
    UASSERT_EQ((int)o->shape->count, 1);
    UASSERT_EQ((unsigned)o->shape->props_table[0]->constant, 1U);

    memset(&s.stack[0], 0, sizeof s.stack[0]);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

UTEST(matrix_remove_property_pins_sibling_shape)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UStrand s;
    matrix_strand_setup(&vm, &s);

    UObject *o = (UObject *)(void *)sentinel_alloc(&vm);
    UASSERT(o != NULL);
    s.stack[0] = obj_value_for((UCell *)(void *)o);
    o->shape = urbi_shape_root(&vm);
    UASSERT(o->shape != NULL);

    USymbol *x = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(x != NULL);
    UValue v42;
    memset(&v42, 0, sizeof v42);
    v42.kind = UVAL_INT;
    v42.v.i  = 42;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, o, x, v42), 0);
    UASSERT_EQ(urbi_object_install_property(&vm, o, x,
                                            URBI_SLOT_FLAG_CONSTANT, v42),
               URBI_OK);

    /* The call under test: armed removal — the transition sibling +
     * wrapper must be pinned across the internal uprops_alloc. */
    UASSERT_EQ(urbi_object_remove_property(&vm, o, x,
                                           URBI_SLOT_FLAG_CONSTANT), 0);

    /* Published shape is sane: one slot, CONSTANT nibble cleared, the
     * all-clear UProps dropped to NULL. */
    UASSERT_EQ((int)o->shape->count, 1);
    UASSERT_EQ((int)urbi_shape_find_slot(o->shape, x), 0);
    UASSERT_EQ((unsigned)((o->shape->flags & 0xFU)
                          & URBI_SLOT_FLAG_CONSTANT), 0U);
    UASSERT(o->shape->props_table != NULL);
    UASSERT(o->shape->props_table[0] == NULL);
    UASSERT(cell_live(&vm, o->shape));

    collect_twice(&vm);
    UASSERT(cell_live(&vm, o->shape));
    UASSERT_EQ((int)o->shape->count, 1);
    UASSERT_EQ((int)urbi_shape_find_slot(o->shape, x), 0);

    memset(&s.stack[0], 0, sizeof s.stack[0]);
    matrix_strand_teardown(&vm, &s);
    urbi_vm_destroy(&vm);
}

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
    utest_run("matrix_frame_recv_covered_by_register_scan",
              matrix_frame_recv_covered_by_register_scan);
    utest_run("matrix_unwind_target_covered_by_cleanup_scope",
              matrix_unwind_target_covered_by_cleanup_scope);
    utest_run("matrix_wait_event_target_covered_by_event_reachability",
              matrix_wait_event_target_covered_by_event_reachability);
    utest_run("matrix_list_element_is_rooted",
              matrix_list_element_is_rooted);
    utest_run("matrix_dict_value_is_rooted",
              matrix_dict_value_is_rooted);
    utest_run("matrix_dict_key_is_rooted",
              matrix_dict_key_is_rooted);
    utest_run("matrix_atomic_finish_rescans_mutated_registers",
              matrix_atomic_finish_rescans_mutated_registers);
    utest_run("matrix_c_root_frame_roots_value",
              matrix_c_root_frame_roots_value);
    utest_run("matrix_cleanup_saved_value_survives_gc_in_cleanup_body",
              matrix_cleanup_saved_value_survives_gc_in_cleanup_body);
    utest_run("matrix_watcher_owning_tag_is_rooted",
              matrix_watcher_owning_tag_is_rooted);
    utest_run("matrix_event_watcher_closures_rooted_without_reachable_event",
              matrix_event_watcher_closures_rooted_without_reachable_event);
    utest_run("matrix_pending_unregister_watcher_still_rooted",
              matrix_pending_unregister_watcher_still_rooted);
    utest_run("matrix_periodic_owning_tag_is_rooted",
              matrix_periodic_owning_tag_is_rooted);
    utest_run("matrix_upvalue_close_fires_barrier",
              matrix_upvalue_close_fires_barrier);
    utest_run("matrix_slot_store_barrier_shades_other_white",
              matrix_slot_store_barrier_shades_other_white);
    utest_run("matrix_setupval_barrier_targets_shared_cell",
              matrix_setupval_barrier_targets_shared_cell);
    utest_run("matrix_install_property_pins_construction",
              matrix_install_property_pins_construction);
    utest_run("matrix_remove_property_pins_sibling_shape",
              matrix_remove_property_pins_sibling_shape);
}
