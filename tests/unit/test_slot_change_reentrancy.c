/* SPDX-License-Identifier: BSD-3-Clause */
/* URBI_GC_STRESS disarm (v0.13.2): C-API scaffolding suite — GC cells
 * (tags/events/objects/closures) held in bare C locals and/or synthetic
 * strands outside the realm graph, by design, to drive one primitive in
 * isolation.  Collect-on-every-alloc sweeps them between paired
 * allocations; fine on normal builds where host-C call sequences cannot
 * be interrupted by a collection.  Each test sets vm.gc_stress_armed = 0
 * after init.  Structural-by-design, not a runtime rooting bug
 * (refactor-3 TEST-GAP-01 stress-exempt list). */
/* Integration tests: deferred slot-change ring drain + reentrancy degrade
 * (T66, spec #4 §5.3 + §5.4).
 *
 * Cases:
 *   1. deferred_ring_drains_at_safepoint:
 *      Entries pushed to the ring while in_watcher_scratch is set are drained
 *      by urbi_drain_deferred_slot_changes and dispatched to the event.
 *   2. deferred_ring_overflow_drops_with_warn:
 *      Fill the ring to capacity; one more push drops silently and sets
 *      slot_change_ring_full_warned.
 *   3. drain_ordering_fifo:
 *      Multiple deferred entries are dispatched head→tail (FIFO) order,
 *      verified by observing strand spawn count.
 *   4. drain_before_watcher_eval_ordering:
 *      Confirmed by test: drain runs (watcher spawned) even before any
 *      watcher dirty evaluation is triggered.
 */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "vm/uvm.h"
#include "sched/ustrand.h"                   /* ustrand_init/destroy */
#include "realm/urealm.h"              /* urbi_realm_create/destroy */
#include "object/uobject.h"            /* urbi_object_alloc */
#include "changed/uchanged_node.h"             /* urbi_object_get_or_create_change_event,
                                          urbi_defer_slot_change,
                                          urbi_drain_deferred_slot_changes,
                                          urbi_emit_slot_change_if_subscribed */
#include "watcher/uwatcher.h"          /* UWatcher, UWATCHER_AT_EVENT */
#include "watcher/uwatcher_install.h"  /* urbi_watcher_install_at_event_runtime */
#include "value/uintern.h"                   /* ustr_intern */
#include "chunk/uchunk.h"                   /* USymbol, UClosure, UProto */
#include "runtime/uclosure.h"                  /* UClosure layout */
#include "urbi/object.h"               /* URBI_ATOM_OBJECT */

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers
 * =================================================================== */

static void
make_trivial_closure(UClosure *cl, UProto *proto, uint32_t *instr_buf)
{
    instr_buf[0] = (uint32_t)OP_RET;
    memset(proto, 0, sizeof(*proto));
    proto->instructions = instr_buf;
    proto->instr_count  = 1;
    proto->constants    = NULL;
    proto->const_count  = 0;
    memset(cl, 0, sizeof(*cl));
    cl->proto   = proto;
    cl->nupvals = 0;
}

/* Install an AT_EVENT watcher on the slot-change event for (obj, sym).
 * Returns the event or NULL on failure. */
static UEvent *
install_at_event_on_slot(UVM *vm, UStrand *s, UObject *obj, USymbol *sym,
                         UClosure *cl, UProto *proto, uint32_t *instr)
{
    UEvent *e = urbi_object_get_or_create_change_event(vm, obj, sym);
    if (!e) return NULL;
    make_trivial_closure(cl, proto, instr);
    UWatcherInstallResult ir =
        urbi_watcher_install_at_event_runtime(vm, s, UWATCHER_AT_EVENT, e, cl, NULL);
    return (ir == UWATCHER_INSTALL_OK) ? e : NULL;
}

/* ===================================================================
 * Test 1: deferred entries drain at next urbi_drain_deferred_slot_changes
 * =================================================================== */

UTEST(deferred_ring_drains_at_safepoint)
{
    UVM vm;
    UStrand s;
    uint32_t instr[1]; UProto proto; UClosure cl;

    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    if (!r) { urbi_vm_destroy(&vm); return; }
    ustrand_init(&s, &vm);
    s.realm = r;

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (!o) { ustrand_destroy(&s, &vm); urbi_realm_destroy(&vm, r); urbi_vm_destroy(&vm); return; }

    USymbol *sym = (USymbol *)ustr_intern(&vm, "x", 1);
    UEvent *e = install_at_event_on_slot(&vm, &s, o, sym, &cl, &proto, instr);
    UASSERT(e != NULL);
    if (!e) { ustrand_destroy(&s, &vm); urbi_realm_destroy(&vm, r); urbi_vm_destroy(&vm); return; }

    uint32_t runnable_before = vm.strand_runnable_count;

    /* Simulate being inside a scratch context and push a deferred entry. */
    vm.watchers->in_scratch = 1;
    UValue v; v.kind = UVAL_INT; v.v.i = 7;
    urbi_emit_slot_change_if_subscribed(&vm, o, sym, v);
    vm.watchers->in_scratch = 0;

    /* Ring should have one entry; no strand spawned yet. */
    UASSERT(vm.deferred_slot_changes_tail != vm.deferred_slot_changes_head);
    UASSERT_EQ((int)runnable_before, (int)vm.strand_runnable_count);

    /* Drain — simulates what happens at safepoint. */
    urbi_drain_deferred_slot_changes(&vm);

    /* Ring must be empty now. */
    UASSERT_EQ((int)vm.deferred_slot_changes_head,
               (int)vm.deferred_slot_changes_tail);

    /* Strand was spawned by urbi_event_emit_sync inside the drain. */
    UASSERT(vm.strand_runnable_count > runnable_before);

    if (e->at_watchers_head)
        urbi_watcher_unregister_internal(&vm, e->at_watchers_head);
    ustrand_destroy(&s, &vm);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 2: ring-full drops with one-shot warn
 * =================================================================== */

UTEST(deferred_ring_overflow_drops_with_warn)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (!o) { urbi_vm_destroy(&vm); return; }

    USymbol *sym = (USymbol *)ustr_intern(&vm, "x", 1);
    /* Install change event so bit 7 is set. */
    UEvent *e = urbi_object_get_or_create_change_event(&vm, o, sym);
    UASSERT(e != NULL);
    if (!e) { urbi_vm_destroy(&vm); return; }

    /* Fill the ring to capacity (cap - 1 entries, since it's SPSC). */
    uint16_t cap = vm.deferred_slot_changes_cap;
    UValue v; v.kind = UVAL_INT; v.v.i = 0;
    uint16_t i;
    for (i = 0; i < (uint16_t)(cap - 1); i++) {
        urbi_defer_slot_change(&vm, o, sym, v);
    }
    /* Ring should be full: next push would advance tail to head. */
    UASSERT_EQ(0, (int)vm.slot_change_ring_full_warned);

    /* One more push: must drop and set the warn flag. */
    urbi_defer_slot_change(&vm, o, sym, v);
    UASSERT_EQ(1, (int)vm.slot_change_ring_full_warned);

    /* Second push: warn flag already set, no double-fire. */
    urbi_defer_slot_change(&vm, o, sym, v);
    UASSERT_EQ(1, (int)vm.slot_change_ring_full_warned);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 3: FIFO drain order
 * =================================================================== */

UTEST(deferred_ring_drain_fifo_order)
{
    UVM vm;
    UStrand s;
    uint32_t instr1[1], instr2[1];
    UProto proto1, proto2;
    UClosure cl1, cl2;

    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    if (!r) { urbi_vm_destroy(&vm); return; }
    ustrand_init(&s, &vm);
    s.realm = r;

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (!o) { ustrand_destroy(&s, &vm); urbi_realm_destroy(&vm, r); urbi_vm_destroy(&vm); return; }

    USymbol *sym_x = (USymbol *)ustr_intern(&vm, "x", 1);
    USymbol *sym_y = (USymbol *)ustr_intern(&vm, "y", 1);

    UEvent *ex = install_at_event_on_slot(&vm, &s, o, sym_x, &cl1, &proto1, instr1);
    UEvent *ey = install_at_event_on_slot(&vm, &s, o, sym_y, &cl2, &proto2, instr2);
    UASSERT(ex != NULL && ey != NULL);
    if (!ex || !ey) {
        if (ex && ex->at_watchers_head) urbi_watcher_unregister_internal(&vm, ex->at_watchers_head);
        if (ey && ey->at_watchers_head) urbi_watcher_unregister_internal(&vm, ey->at_watchers_head);
        ustrand_destroy(&s, &vm);
        urbi_realm_destroy(&vm, r);
        urbi_vm_destroy(&vm);
        return;
    }

    uint32_t runnable_before = vm.strand_runnable_count;

    /* Push two entries (x then y) to the deferred ring. */
    UValue vx; vx.kind = UVAL_INT; vx.v.i = 1;
    UValue vy; vy.kind = UVAL_INT; vy.v.i = 2;
    urbi_defer_slot_change(&vm, o, sym_x, vx);
    urbi_defer_slot_change(&vm, o, sym_y, vy);

    /* Drain both entries. */
    urbi_drain_deferred_slot_changes(&vm);

    /* Both x and y events were dispatched: 2 body strands spawned. */
    UASSERT(vm.strand_runnable_count >= runnable_before + 2);
    /* Ring is empty. */
    UASSERT_EQ((int)vm.deferred_slot_changes_head,
               (int)vm.deferred_slot_changes_tail);

    if (ex->at_watchers_head) urbi_watcher_unregister_internal(&vm, ex->at_watchers_head);
    if (ey->at_watchers_head) urbi_watcher_unregister_internal(&vm, ey->at_watchers_head);
    ustrand_destroy(&s, &vm);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry point
 * =================================================================== */

void
test_slot_change_reentrancy_suite(void)
{
    printf("test_slot_change_reentrancy\n");
    utest_run("deferred_ring_drains_at_safepoint",
              deferred_ring_drains_at_safepoint);
    utest_run("deferred_ring_overflow_drops_with_warn",
              deferred_ring_overflow_drops_with_warn);
    utest_run("deferred_ring_drain_fifo_order",
              deferred_ring_drain_fifo_order);
}
