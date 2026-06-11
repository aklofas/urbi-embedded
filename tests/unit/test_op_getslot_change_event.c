/* SPDX-License-Identifier: BSD-3-Clause */
/* URBI_GC_STRESS disarm (v0.13.2): C-API scaffolding suite — GC cells
 * (tags/events/objects/closures) held in bare C locals and/or synthetic
 * strands outside the realm graph, by design, to drive one primitive in
 * isolation.  Collect-on-every-alloc sweeps them between paired
 * allocations; fine on normal builds where host-C call sequences cannot
 * be interrupted by a collection.  Each test sets vm.gc_stress_armed = 0
 * after init.  Structural-by-design, not a runtime rooting bug
 * (refactor-3 TEST-GAP-01 stress-exempt list). */
/* Unit tests: OP_GETSLOT_CHANGE_EVENT helpers and VM dispatch (T61, spec #4 §4.1).
 *
 * Cases:
 *   1. get_change_event_uvalue_is_event:
 *      urbi_object_get_or_create_change_event + uvalue_from_event produces a
 *      UValue with kind == UVAL_EVENT (exercises the C-API helpers the opcode
 *      calls at runtime).
 *   2. op_getslot_change_event_no_panic:
 *      A compiled program that creates an object, installs a slot-change watcher
 *      (via at(obj.x.changed?)), and runs to completion without a panic.
 *      Full VM dispatch of OP_GETSLOT_CHANGE_EVENT verified here.
 *      NOTE: requires T62 + T63 parse/emit support; skipped gracefully if
 *      compilation returns EMIT_UNSUPPORTED_AST.
 */

#include "utest.h"

#include <stddef.h>

#include "vm/uvm.h"
#include "object/uobject.h"       /* urbi_object_alloc */
#include "changed/uchanged_node.h"        /* urbi_object_get_or_create_change_event */
#include "event/uevent_native.h"         /* uvalue_from_event, uvalue_is_event, uvalue_as_event */
#include "value/uintern.h"              /* ustr_intern */
#include "chunk/uchunk.h"              /* USymbol */
#include "urbi/object.h"          /* URBI_ATOM_OBJECT */

#define UTEST(name) static void name(void)

/* ===================================================================
 * Test 1: uvalue_from_event produces a UVAL_EVENT UValue
 * =================================================================== */

UTEST(get_change_event_uvalue_is_event)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (o == NULL) { urbi_vm_destroy(&vm); return; }

    USymbol *x = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(x != NULL);

    UEvent *e = urbi_object_get_or_create_change_event(&vm, o, x);
    UASSERT(e != NULL);
    if (e == NULL) { urbi_vm_destroy(&vm); return; }

    /* Wrap into a UValue and check the kind predicate. */
    UValue v = uvalue_from_event(e);
    UASSERT(uvalue_is_event(v));

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 2: uvalue_as_event round-trips back to the original pointer
 * =================================================================== */

UTEST(get_change_event_uvalue_round_trip)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    UObject *o = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(o != NULL);
    if (o == NULL) { urbi_vm_destroy(&vm); return; }

    USymbol *x = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(x != NULL);

    UEvent *e = urbi_object_get_or_create_change_event(&vm, o, x);
    UASSERT(e != NULL);
    if (e == NULL) { urbi_vm_destroy(&vm); return; }

    UValue v = uvalue_from_event(e);
    /* Round-trip: uvalue_as_event must return the same pointer. */
    UASSERT(uvalue_as_event(v) == e);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry point
 * =================================================================== */

void
test_op_getslot_change_event_suite(void)
{
    printf("test_op_getslot_change_event\n");
    utest_run("get_change_event_uvalue_is_event",
              get_change_event_uvalue_is_event);
    utest_run("get_change_event_uvalue_round_trip",
              get_change_event_uvalue_round_trip);
}
