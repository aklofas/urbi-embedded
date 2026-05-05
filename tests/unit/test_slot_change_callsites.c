/* SPDX-License-Identifier: BSD-3-Clause */
/* Integration tests: slot-change emit at write callsites (T65, spec #4 §5.2).
 *
 * Tests the three C-API callsites via direct calls (no bytecode pipeline
 * needed — globals exposure is not yet wired at R6):
 *
 *   1. slot_change_fires_via_uslothandle_write:
 *      urbi_slothandle_write_value fires the slot-change event.
 *   2. slot_change_fires_via_set_local_slot_inplace:
 *      urbi_object_set_local_slot case 1 (in-place) fires on second write.
 *   3. slot_change_fires_via_set_local_slot_cow:
 *      urbi_object_set_local_slot case 2 (COW / new slot) fires on first write.
 *   4. slot_change_no_fire_when_no_subscriber:
 *      Without a subscriber, none of the callsites fires (fast-path passes).
 *
 * "Fired" is detected by checking vm.strand_runnable_count increments after
 * the emit: c_event_emit_sync calls do_spawn_body_coroutine which enqueues a
 * body strand.  A URealm is required for strand spawning.
 */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "uvm.h"
#include "ustrand.h"                   /* ustrand_init/destroy */
#include "realm/urealm.h"              /* urbi_realm_create/destroy */
#include "object/uobject.h"            /* urbi_object_alloc, urbi_object_set_local_slot */
#include "object/uslothandle.h"        /* urbi_object_get_slot, urbi_slothandle_write_value */
#include "uchanged_node.h"             /* urbi_object_get_or_create_change_event */
#include "watcher/uwatcher.h"          /* UWatcher, UWATCHER_AT_EVENT */
#include "watcher/uwatcher_install.h"  /* install_at_event_runtime */
#include "uintern.h"                   /* ustr_intern */
#include "umodule.h"                   /* USymbol, UClosure, UProto */
#include "uclosure.h"                  /* UClosure layout */
#include "urbi/object.h"               /* URBI_ATOM_OBJECT */
#include "gc/ugc_incremental.h"        /* UGC_HAS_SLOT_CHANGE_EVENT */

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

/* Set up: vm + realm + strand (with realm set) + object o + event on slot s.
 * Installs an AT_EVENT watcher with a trivial body on the slot-change event.
 * Returns the event pointer; sets *out_strand / *out_realm.
 * Caller must clean up (urbi_watcher_unregister_internal, ustrand_destroy,
 * urbi_realm_destroy, uvm_destroy). */
static UEvent *
setup_subscriber(UVM *vm_out, URealm **realm_out, UStrand *s_out,
                 UObject *obj, USymbol *sym,
                 UClosure *cl_out, UProto *proto_out, uint32_t *instr_out)
{
    URealm *r = urbi_realm_create(vm_out);
    if (!r) return NULL;
    *realm_out = r;

    ustrand_init(s_out, vm_out);
    s_out->realm = r;

    UEvent *e = urbi_object_get_or_create_change_event(vm_out, obj, sym);
    if (!e) return NULL;

    make_trivial_closure(cl_out, proto_out, instr_out);
    UWatcherInstallResult ir =
        install_at_event_runtime(vm_out, s_out, UWATCHER_AT_EVENT, e, cl_out, NULL);
    if (ir != URBI_INSTALL_OK) return NULL;

    return e;
}

static void
cleanup_subscriber(UVM *vm, UStrand *s, URealm *r, UEvent *e)
{
    if (e && e->at_watchers_head)
        urbi_watcher_unregister_internal(vm, e->at_watchers_head);
    ustrand_destroy(s, vm);
    urbi_realm_destroy(vm, r);
}

/* ===================================================================
 * Test 1: urbi_slothandle_write_value callsite
 * =================================================================== */

UTEST(slot_change_fires_via_uslothandle_write)
{
    UVM vm;
    UStrand s;
    URealm *r;
    uint32_t instr[1]; UProto proto; UClosure cl;

    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (!o) { uvm_destroy(&vm); return; }

    USymbol *sym = (USymbol *)ustr_intern(&vm, "y", 1);

    /* Install a local slot first so USlotHandle can resolve it. */
    UValue init; init.kind = UVAL_INT; init.v.i = 0;
    int rc = urbi_object_set_local_slot(&vm, o, sym, init);
    UASSERT_EQ(0, rc);

    UEvent *e = setup_subscriber(&vm, &r, &s, o, sym, &cl, &proto, instr);
    UASSERT(e != NULL);
    if (!e) { ustrand_destroy(&s, &vm); urbi_realm_destroy(&vm, r); uvm_destroy(&vm); return; }

    USlotHandle *h = urbi_object_get_slot(&vm, o, sym);
    UASSERT(h != NULL);
    if (!h) { cleanup_subscriber(&vm, &s, r, e); uvm_destroy(&vm); return; }

    uint32_t runnable_before = vm.strand_runnable_count;

    UValue v; v.kind = UVAL_INT; v.v.i = 99;
    int wr = urbi_slothandle_write_value(&vm, h, v);
    UASSERT_EQ(0, wr);

    /* Strand must have been spawned (runnable count increased). */
    UASSERT(vm.strand_runnable_count > runnable_before);

    cleanup_subscriber(&vm, &s, r, e);
    uvm_destroy(&vm);
}

/* ===================================================================
 * Test 2: urbi_object_set_local_slot case 1 (in-place update)
 * =================================================================== */

UTEST(slot_change_fires_via_set_local_slot_inplace)
{
    UVM vm;
    UStrand s;
    URealm *r;
    uint32_t instr[1]; UProto proto; UClosure cl;

    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (!o) { uvm_destroy(&vm); return; }

    USymbol *sym = (USymbol *)ustr_intern(&vm, "x", 1);

    /* First call: creates the slot (COW transition). */
    UValue v1; v1.kind = UVAL_INT; v1.v.i = 1;
    urbi_object_set_local_slot(&vm, o, sym, v1);

    UEvent *e = setup_subscriber(&vm, &r, &s, o, sym, &cl, &proto, instr);
    UASSERT(e != NULL);
    if (!e) { ustrand_destroy(&s, &vm); urbi_realm_destroy(&vm, r); uvm_destroy(&vm); return; }

    uint32_t runnable_before = vm.strand_runnable_count;

    /* Second call: in-place update (case 1). */
    UValue v2; v2.kind = UVAL_INT; v2.v.i = 2;
    int rc = urbi_object_set_local_slot(&vm, o, sym, v2);
    UASSERT_EQ(0, rc);

    UASSERT(vm.strand_runnable_count > runnable_before);

    cleanup_subscriber(&vm, &s, r, e);
    uvm_destroy(&vm);
}

/* ===================================================================
 * Test 3: urbi_object_set_local_slot case 2 (COW / new slot)
 * =================================================================== */

UTEST(slot_change_fires_via_set_local_slot_cow)
{
    UVM vm;
    UStrand s;
    URealm *r;
    uint32_t instr[1]; UProto proto; UClosure cl;

    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (!o) { uvm_destroy(&vm); return; }

    /* Use a fresh symbol "z" — no slot installed yet. */
    USymbol *sym = (USymbol *)ustr_intern(&vm, "z", 1);

    UEvent *e = setup_subscriber(&vm, &r, &s, o, sym, &cl, &proto, instr);
    UASSERT(e != NULL);
    if (!e) { ustrand_destroy(&s, &vm); urbi_realm_destroy(&vm, r); uvm_destroy(&vm); return; }

    uint32_t runnable_before = vm.strand_runnable_count;

    /* First call for "z": COW shape transition (case 2). */
    UValue v; v.kind = UVAL_INT; v.v.i = 3;
    int rc = urbi_object_set_local_slot(&vm, o, sym, v);
    UASSERT_EQ(0, rc);

    UASSERT(vm.strand_runnable_count > runnable_before);

    cleanup_subscriber(&vm, &s, r, e);
    uvm_destroy(&vm);
}

/* ===================================================================
 * Test 4: no fire when no subscriber (fast-path check)
 * =================================================================== */

UTEST(slot_change_no_fire_when_no_subscriber)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (!o) { uvm_destroy(&vm); return; }

    USymbol *sym = (USymbol *)ustr_intern(&vm, "w", 1);
    UValue v; v.kind = UVAL_INT; v.v.i = 5;

    uint32_t runnable_before = vm.strand_runnable_count;
    uint16_t ring_tail_before = vm.deferred_slot_changes_tail;

    /* All three callsites: no subscriber installed. */
    urbi_object_set_local_slot(&vm, o, sym, v);  /* COW */
    urbi_object_set_local_slot(&vm, o, sym, v);  /* in-place */

    /* No runnable strands spawned, no deferred ring entries. */
    UASSERT_EQ((int)runnable_before, (int)vm.strand_runnable_count);
    UASSERT_EQ((int)ring_tail_before, (int)vm.deferred_slot_changes_tail);

    uvm_destroy(&vm);
}

/* ===================================================================
 * Suite entry point
 * =================================================================== */

void
test_slot_change_callsites_suite(void)
{
    printf("test_slot_change_callsites\n");
    utest_run("slot_change_fires_via_uslothandle_write",
              slot_change_fires_via_uslothandle_write);
    utest_run("slot_change_fires_via_set_local_slot_inplace",
              slot_change_fires_via_set_local_slot_inplace);
    utest_run("slot_change_fires_via_set_local_slot_cow",
              slot_change_fires_via_set_local_slot_cow);
    utest_run("slot_change_no_fire_when_no_subscriber",
              slot_change_no_fire_when_no_subscriber);
}
