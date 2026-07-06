/* SPDX-License-Identifier: BSD-3-Clause */
/* URBI_GC_STRESS disarm (v0.13.2): C-API scaffolding suite — GC cells
 * (tags/events/objects/closures) held in bare C locals and/or synthetic
 * strands outside the realm graph, by design, to drive one primitive in
 * isolation.  Collect-on-every-alloc sweeps them between paired
 * allocations; fine on normal builds where host-C call sequences cannot
 * be interrupted by a collection.  Each test sets vm.gc_stress_armed = 0
 * after init.  Structural-by-design, not a runtime rooting bug
 * (refactor-3 TEST-GAP-01 stress-exempt list). */
/* Unit tests: Tag.enter / Tag.leave native getters with lazy alloc
 * (spec #3 §8.2).
 *
 * Source-level tests (e.g. `myTag.enter`) are blocked by globals exposure
 * (T59) and UVAL_TAG register-binding (deferred).  These tests drive via
 * the typed C helpers directly:
 *
 *   1. tag_enter_is_lazy_allocated:
 *      tag->enter_event is NULL at create.  urbi_tag_enter_getter lazy-allocates
 *      on first call and is idempotent on second call (same UEvent returned).
 *
 *   2. tag_leave_is_lazy_allocated:
 *      Same contract for tag->leave_event.
 *
 *   3. tag_proto_has_enter_and_leave_native_slots (W4/v0.10.2 update):
 *      Phase 7 (M6 stdlib) removed the unreachable enter/leave getter
 *      stubs.  Lookup for "enter"/"leave" on vm->tag_proto must now miss.
 *
 *   4. tag_enter_setter_throws_protected_slot:
 *      Calling the _enter_set stub throws URBI_ERR_PROTECTED_SLOT (via
 *      urbi_throw, which sets pending_unwind on the strand). */

#include "utest.h"

#include "vm/uvm.h"
#include "tag/utag_native.h"
#include "event/uevent_native.h"
#include "tag/utag.h"
#include "event/uevent.h"
#include "object/uobject.h"
#include "value/uintern.h"
#include "sched/ustrand.h"
#include "runtime/uunwind.h"
#include "chunk/uchunk.h"
#include "urbi/urbi.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Test 1: tag_enter_is_lazy_allocated
 * =================================================================== */

UTEST(tag_enter_is_lazy_allocated)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    UTag *t = utag_create(&vm);
    UASSERT(t != NULL);
    if (t == NULL) { urbi_vm_destroy(&vm); return; }

    /* enter_event is NULL at create. */
    UASSERT(t->enter_event == NULL);

    /* First call: lazy-allocates. */
    UValue r1 = urbi_tag_enter_getter(&vm, t);
    UASSERT_EQ((int)r1.kind, (int)UVAL_EVENT);
    UASSERT(t->enter_event != NULL);
    if (r1.kind == (uint8_t)UVAL_EVENT) {
        UEvent *e1 = uvalue_as_event(r1);
        UASSERT(e1 == t->enter_event);
    }

    /* Second call: idempotent — same UEvent returned. */
    UValue r2 = urbi_tag_enter_getter(&vm, t);
    UASSERT_EQ((int)r2.kind, (int)UVAL_EVENT);
    if (r2.kind == (uint8_t)UVAL_EVENT && r1.kind == (uint8_t)UVAL_EVENT) {
        UASSERT(uvalue_as_event(r2) == uvalue_as_event(r1));
    }

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 2: tag_leave_is_lazy_allocated
 * =================================================================== */

UTEST(tag_leave_is_lazy_allocated)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    UTag *t = utag_create(&vm);
    UASSERT(t != NULL);
    if (t == NULL) { urbi_vm_destroy(&vm); return; }

    UASSERT(t->leave_event == NULL);

    UValue r1 = urbi_tag_leave_getter(&vm, t);
    UASSERT_EQ((int)r1.kind, (int)UVAL_EVENT);
    UASSERT(t->leave_event != NULL);
    if (r1.kind == (uint8_t)UVAL_EVENT) {
        UASSERT(uvalue_as_event(r1) == t->leave_event);
    }

    /* Idempotent. */
    UValue r2 = urbi_tag_leave_getter(&vm, t);
    UASSERT_EQ((int)r2.kind, (int)UVAL_EVENT);
    if (r2.kind == (uint8_t)UVAL_EVENT && r1.kind == (uint8_t)UVAL_EVENT) {
        UASSERT(uvalue_as_event(r2) == uvalue_as_event(r1));
    }

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 3: tag_proto_has_enter_and_leave_native_slots (W4/v0.10.2 update)
 *
 * W4 (v0.10.2 reactive) installs tag_enter_native / tag_leave_native as
 * UVAL_CLOSURE native methods on tag_proto.  The old assertion (miss)
 * was correct for the Phase-7 baseline (TAGCH-013 closure) when UVAL_TAG
 * didn't exist in UValKind.  With UVAL_TAG = 12 and scripted tag.stop()
 * / .enter / .leave live, the lookup must now HIT.
 *
 * Pre-W4 rationale (preserved for history): the Phase 7 cleanup removed
 * unreachable getter stubs.  W4 reinstates enter/leave as reachable
 * UVAL_CLOSURE native methods (tag_enter_native / tag_leave_native).
 * =================================================================== */

UTEST(tag_proto_has_enter_and_leave_native_slots)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */
    urbi_native_protos_init(&vm);

    UASSERT(vm.tag_proto != NULL);
    if (vm.tag_proto == NULL) { urbi_vm_destroy(&vm); return; }

    struct { const char *name; size_t len; } slots[] = {
        { "enter", 5 },
        { "leave", 5 },
    };
    int i;
    for (i = 0; i < 2; i++) {
        USymbol *sym = (USymbol *)ustr_intern(&vm, slots[i].name, slots[i].len);
        UASSERT(sym != NULL);
        if (sym == NULL) continue;

        UValue v;
        v.kind = (uint8_t)UVAL_NIL;
        /* W4: urbi_object_lookup returns 0 on hit — enter/leave are now
         * UVAL_CLOSURE native methods installed by urbi_tag_native_register. */
        int hit = (urbi_object_lookup(&vm, vm.tag_proto, sym, &v) == 0);
        UASSERT(hit);
        if (hit) {
            /* Verify the slot is a native UVAL_CLOSURE. */
            UASSERT_EQ((int)v.kind, (int)UVAL_CLOSURE);
        }
    }

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 4: tag_enter_setter_throws_protected_slot
 *
 * The _enter_set stub calls urbi_throw with URBI_ERR_PROTECTED_SLOT.
 * urbi_throw deposits a THROW unwind event on the strand; we verify
 * the strand's pending_unwind.status becomes UEXEC_THROW after the call.
 *
 * Closes audit ID TAGCH-018 [cov, defer:M6]:
 * 'tag_enter_leave_setter_protected — no test coverage for protected-
 *  slot-write throw path'.  This test exercises that path end-to-end:
 *  resolve _enter_set on tag_proto, invoke as a UHostFn with a tag_proto
 *  receiver and a NIL value argument, assert the strand's unwind state
 *  carries UEXEC_THROW + thrown int == URBI_ERR_PROTECTED_SLOT.
 *  Cross-reference: TAGCH-013 closure (Phase 7 / v0.6.0 stdlib scaffold)
 *  removed the unreachable enter/leave getter stubs but kept the setter
 *  stubs to surface protected-slot semantics — this test pins that
 *  semantics at v0.6.1.
 * =================================================================== */

UTEST(tag_enter_setter_throws_protected_slot)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */
    urbi_native_protos_init(&vm);

    UASSERT(vm.tag_proto != NULL);
    if (vm.tag_proto == NULL) { urbi_vm_destroy(&vm); return; }

    /* Locate _enter_set stub. */
    USymbol *sym_set = (USymbol *)ustr_intern(&vm, "_enter_set", 10);
    UASSERT(sym_set != NULL);
    if (sym_set == NULL) { urbi_vm_destroy(&vm); return; }

    UValue slot_val;
    slot_val.kind = (uint8_t)UVAL_NIL;
    UASSERT(urbi_object_lookup(&vm, vm.tag_proto, sym_set, &slot_val) == 0);
    UASSERT_EQ((int)slot_val.kind, (int)UVAL_HOST_FN);
    if (slot_val.kind != (uint8_t)UVAL_HOST_FN) { urbi_vm_destroy(&vm); return; }

    /* Create a strand so urbi_throw has somewhere to deposit. */
    UStrand s;
    ustrand_init(&s, &vm);

    /* Build argv: [tag_proto, some_value]. */
    UValue argv[2];
    argv[0].kind = (uint8_t)UVAL_OBJECT;
    argv[0].v.p  = (void *)vm.tag_proto;
    argv[1].kind = (uint8_t)UVAL_NIL;

    UHostFn fn = (UHostFn)(uintptr_t)slot_val.v.p;
    fn(&s, 2, argv);

    /* Strand should now have a THROW unwind pending. */
    UStrandUnwind status = urbi_strand_unwind_status(&vm, &s);
    UASSERT_EQ((int)status, (int)URBI_UNWIND_THROW);

    /* The thrown value should be an int == URBI_ERR_PROTECTED_SLOT. */
    if (status == URBI_UNWIND_THROW) {
        UValue thrown = s.unwind_value;
        UASSERT_EQ((int)thrown.kind, (int)UVAL_INT);
        if (thrown.kind == (uint8_t)UVAL_INT) {
            UASSERT_EQ((long long)thrown.v.i, (long long)URBI_ERR_PROTECTED_SLOT);
        }
    }

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void
test_tag_native_suite(void)
{
    printf("test_tag_native\n");
    utest_run("tag_enter_is_lazy_allocated",          tag_enter_is_lazy_allocated);
    utest_run("tag_leave_is_lazy_allocated",          tag_leave_is_lazy_allocated);
    utest_run("tag_proto_has_enter_and_leave_native_slots", tag_proto_has_enter_and_leave_native_slots);
    utest_run("tag_enter_setter_throws_protected_slot", tag_enter_setter_throws_protected_slot);
}
