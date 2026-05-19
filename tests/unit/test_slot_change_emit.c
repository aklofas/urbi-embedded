/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: urbi_emit_slot_change_if_subscribed (T64, spec #4 §5.1).
 *
 * Cases:
 *   1. emit_skips_when_bit7_clear:
 *      Object with no subscriber — bit 7 clear — fast path returns without
 *      calling the slow path.  Verify no crash.
 *   2. emit_dispatches_when_subscriber_present:
 *      Object with a watcher installed on its slot-change event — slow path
 *      finds the chain entry, calls c_event_emit_sync, watcher fires (body
 *      strand spawned).
 *   3. emit_defers_when_in_scratch_context:
 *      With in_watcher_scratch set, a call must route to the deferred ring
 *      (not emit immediately) and set slot_change_reentrancy_warned once.
 */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "vm/uvm.h"
#include "sched/ustrand.h"                   /* ustrand_init/destroy */
#include "realm/urealm.h"              /* urbi_realm_create/destroy */
#include "object/uobject.h"            /* urbi_object_alloc */
#include "changed/uchanged_node.h"             /* urbi_object_get_or_create_change_event,
                                          urbi_emit_slot_change_if_subscribed */
#include "watcher/uwatcher.h"          /* UWatcher, UWATCHER_AT_EVENT */
#include "watcher/uwatcher_install.h"  /* install_at_event_runtime */
#include "gc/ugc_incremental.h"        /* UGC_HAS_SLOT_CHANGE_EVENT */
#include "value/uintern.h"                   /* ustr_intern */
#include "chunk/uchunk.h"                   /* USymbol, UClosure, UProto */
#include "runtime/uclosure.h"                  /* UClosure layout */
#include "urbi/object.h"               /* URBI_ATOM_OBJECT */

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helper: trivial OP_RET closure (same pattern as test_event_emit_async)
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

/* ===================================================================
 * Test 1: fast path — bit 7 clear, no dispatch
 * =================================================================== */

UTEST(emit_skips_when_bit7_clear)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (o == NULL) { urbi_vm_destroy(&vm); return; }

    /* No subscriber → bit 7 must be clear at alloc. */
    UASSERT_EQ(0, (int)(((UCell *)o)->gc_byte & UGC_HAS_SLOT_CHANGE_EVENT));

    USymbol *x = (USymbol *)ustr_intern(&vm, "x", 1);
    UValue v; v.kind = UVAL_INT; v.v.i = 42;

    /* Must not crash or alter any state. */
    urbi_emit_slot_change_if_subscribed(&vm, o, x, v);

    /* Bit 7 still clear — no chain was created. */
    UASSERT_EQ(0, (int)(((UCell *)o)->gc_byte & UGC_HAS_SLOT_CHANGE_EVENT));
    /* Deferred ring still empty. */
    UASSERT_EQ((int)vm.deferred_slot_changes_head,
               (int)vm.deferred_slot_changes_tail);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 2: slow path dispatches to event when subscriber present
 * =================================================================== */

UTEST(emit_dispatches_when_subscriber_present)
{
    UVM vm;
    UStrand s;
    uint32_t instr_buf[1];
    UProto   proto;
    UClosure body;

    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    if (r == NULL) { urbi_vm_destroy(&vm); return; }

    ustrand_init(&s, &vm);
    s.realm = r;

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (o == NULL) {
        ustrand_destroy(&s, &vm);
        urbi_realm_destroy(&vm, r);
        urbi_vm_destroy(&vm);
        return;
    }

    USymbol *x = (USymbol *)ustr_intern(&vm, "x", 1);

    /* Create the slot-change event (sets bit 7 on o). */
    UEvent *e = urbi_object_get_or_create_change_event(&vm, o, x);
    UASSERT(e != NULL);
    UASSERT((((UCell *)o)->gc_byte & UGC_HAS_SLOT_CHANGE_EVENT) != 0);
    if (e == NULL) {
        ustrand_destroy(&s, &vm);
        urbi_realm_destroy(&vm, r);
        urbi_vm_destroy(&vm);
        return;
    }

    /* Install an AT_EVENT watcher with a trivial body closure. */
    make_trivial_closure(&body, &proto, instr_buf);
    UWatcherInstallResult ir =
        install_at_event_runtime(&vm, &s, UWATCHER_AT_EVENT, e, &body, NULL);
    UASSERT_EQ((int)URBI_INSTALL_OK, (int)ir);

    uint32_t runnable_before = vm.strand_runnable_count;

    /* Emit the slot change — should call c_event_emit_sync → spawn strand. */
    UValue v; v.kind = UVAL_INT; v.v.i = 7;
    urbi_emit_slot_change_if_subscribed(&vm, o, x, v);

    /* do_spawn_body_coroutine must have spawned the body strand. */
    UASSERT(vm.strand_runnable_count > runnable_before ||
            e->at_watchers_head->body_strand != NULL);

    /* Clean up watcher before destroying strand/realm. */
    if (e->at_watchers_head != NULL) {
        urbi_watcher_unregister_internal(&vm, e->at_watchers_head);
    }
    ustrand_destroy(&s, &vm);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 3: re-entrancy — routes to deferred ring, one-shot warn
 * =================================================================== */

UTEST(emit_defers_when_in_scratch_context)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (o == NULL) { urbi_vm_destroy(&vm); return; }

    USymbol *x = (USymbol *)ustr_intern(&vm, "x", 1);
    UEvent *e = urbi_object_get_or_create_change_event(&vm, o, x);
    UASSERT(e != NULL);
    if (e == NULL) { urbi_vm_destroy(&vm); return; }

    uint16_t head_before = vm.deferred_slot_changes_head;
    (void)head_before;

    /* Simulate being inside a sync scratch context. */
    vm.in_watcher_scratch = 1;

    UValue v; v.kind = UVAL_INT; v.v.i = 99;
    urbi_emit_slot_change_if_subscribed(&vm, o, x, v);

    /* Entry must have been pushed to the deferred ring (tail advanced). */
    UASSERT(vm.deferred_slot_changes_tail != vm.deferred_slot_changes_head);
    /* One-shot warn armed. */
    UASSERT_EQ(1, (int)vm.slot_change_reentrancy_warned);

    /* Second call — tail advances again; warn stays at 1. */
    uint16_t tail_after_first = vm.deferred_slot_changes_tail;
    urbi_emit_slot_change_if_subscribed(&vm, o, x, v);
    UASSERT(vm.deferred_slot_changes_tail != tail_after_first);
    UASSERT_EQ(1, (int)vm.slot_change_reentrancy_warned);

    vm.in_watcher_scratch = 0;
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry point
 * =================================================================== */

void
test_slot_change_emit_suite(void)
{
    printf("test_slot_change_emit\n");
    utest_run("emit_skips_when_bit7_clear",
              emit_skips_when_bit7_clear);
    utest_run("emit_dispatches_when_subscriber_present",
              emit_dispatches_when_subscriber_present);
    utest_run("emit_defers_when_in_scratch_context",
              emit_defers_when_in_scratch_context);
}
