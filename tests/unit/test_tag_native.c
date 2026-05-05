/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: Tag.enter / Tag.leave native getters with lazy alloc
 * (spec #3 §8.2).
 *
 * Source-level tests (e.g. `myTag.enter`) are blocked by globals exposure
 * (T59) and UVAL_TAG register-binding (deferred).  These tests drive via
 * the typed C helpers directly:
 *
 *   1. tag_enter_is_lazy_allocated:
 *      tag->enter_event is NULL at create.  tag_enter_getter lazy-allocates
 *      on first call and is idempotent on second call (same UEvent returned).
 *
 *   2. tag_leave_is_lazy_allocated:
 *      Same contract for tag->leave_event.
 *
 *   3. tag_proto_has_enter_and_leave_slots:
 *      After urbi_native_protos_init, vm->tag_proto is non-NULL and has
 *      "enter" and "leave" slots as UVAL_HOST_FN stubs.
 *
 *   4. tag_enter_setter_throws_protected_slot:
 *      Calling the _enter_set stub throws URBI_ERR_PROTECTED_SLOT (via
 *      urbi_throw, which sets pending_unwind on the strand). */

#include "utest.h"

#include "uvm.h"
#include "tag_native.h"
#include "event_native.h"
#include "utag.h"
#include "uevent.h"
#include "object/uobject.h"
#include "uintern.h"
#include "ustrand.h"
#include "uunwind.h"
#include "umodule.h"
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
    uvm_init(&vm, NULL, NULL);

    UTag *t = utag_create(&vm);
    UASSERT(t != NULL);
    if (t == NULL) { uvm_destroy(&vm); return; }

    /* enter_event is NULL at create. */
    UASSERT(t->enter_event == NULL);

    /* First call: lazy-allocates. */
    UValue r1 = tag_enter_getter(&vm, t);
    UASSERT_EQ((int)r1.kind, (int)UVAL_EVENT);
    UASSERT(t->enter_event != NULL);
    if (r1.kind == (uint8_t)UVAL_EVENT) {
        UEvent *e1 = uvalue_as_event(r1);
        UASSERT(e1 == t->enter_event);
    }

    /* Second call: idempotent — same UEvent returned. */
    UValue r2 = tag_enter_getter(&vm, t);
    UASSERT_EQ((int)r2.kind, (int)UVAL_EVENT);
    if (r2.kind == (uint8_t)UVAL_EVENT && r1.kind == (uint8_t)UVAL_EVENT) {
        UASSERT(uvalue_as_event(r2) == uvalue_as_event(r1));
    }

    uvm_destroy(&vm);
}

/* ===================================================================
 * Test 2: tag_leave_is_lazy_allocated
 * =================================================================== */

UTEST(tag_leave_is_lazy_allocated)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UTag *t = utag_create(&vm);
    UASSERT(t != NULL);
    if (t == NULL) { uvm_destroy(&vm); return; }

    UASSERT(t->leave_event == NULL);

    UValue r1 = tag_leave_getter(&vm, t);
    UASSERT_EQ((int)r1.kind, (int)UVAL_EVENT);
    UASSERT(t->leave_event != NULL);
    if (r1.kind == (uint8_t)UVAL_EVENT) {
        UASSERT(uvalue_as_event(r1) == t->leave_event);
    }

    /* Idempotent. */
    UValue r2 = tag_leave_getter(&vm, t);
    UASSERT_EQ((int)r2.kind, (int)UVAL_EVENT);
    if (r2.kind == (uint8_t)UVAL_EVENT && r1.kind == (uint8_t)UVAL_EVENT) {
        UASSERT(uvalue_as_event(r2) == uvalue_as_event(r1));
    }

    uvm_destroy(&vm);
}

/* ===================================================================
 * Test 3: tag_proto_has_enter_and_leave_slots
 * =================================================================== */

UTEST(tag_proto_has_enter_and_leave_slots)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    urbi_native_protos_init(&vm);

    UASSERT(vm.tag_proto != NULL);
    if (vm.tag_proto == NULL) { uvm_destroy(&vm); return; }

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
        int hit = (urbi_object_lookup(&vm, vm.tag_proto, sym, &v) == 0);
        UASSERT(hit);
        if (hit) {
            UASSERT_EQ((int)v.kind, (int)UVAL_HOST_FN);
        }
    }

    uvm_destroy(&vm);
}

/* ===================================================================
 * Test 4: tag_enter_setter_throws_protected_slot
 *
 * The _enter_set stub calls urbi_throw with URBI_ERR_PROTECTED_SLOT.
 * urbi_throw deposits a THROW unwind event on the strand; we verify
 * the strand's pending_unwind.status becomes UEXEC_THROW after the call.
 * =================================================================== */

UTEST(tag_enter_setter_throws_protected_slot)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    urbi_native_protos_init(&vm);

    UASSERT(vm.tag_proto != NULL);
    if (vm.tag_proto == NULL) { uvm_destroy(&vm); return; }

    /* Locate _enter_set stub. */
    USymbol *sym_set = (USymbol *)ustr_intern(&vm, "_enter_set", 10);
    UASSERT(sym_set != NULL);
    if (sym_set == NULL) { uvm_destroy(&vm); return; }

    UValue slot_val;
    slot_val.kind = (uint8_t)UVAL_NIL;
    UASSERT(urbi_object_lookup(&vm, vm.tag_proto, sym_set, &slot_val) == 0);
    UASSERT_EQ((int)slot_val.kind, (int)UVAL_HOST_FN);
    if (slot_val.kind != (uint8_t)UVAL_HOST_FN) { uvm_destroy(&vm); return; }

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
    UExecStatus status = urbi_strand_unwind_status(&s);
    UASSERT_EQ((int)status, (int)UEXEC_THROW);

    /* The thrown value should be an int == URBI_ERR_PROTECTED_SLOT. */
    if (status == UEXEC_THROW) {
        UValue thrown = s.unwind_value;
        UASSERT_EQ((int)thrown.kind, (int)UVAL_INT);
        if (thrown.kind == (uint8_t)UVAL_INT) {
            UASSERT_EQ((long long)thrown.v.i, (long long)URBI_ERR_PROTECTED_SLOT);
        }
    }

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void
test_tag_native_suite(void)
{
    printf("test_tag_native\n");
    utest_run("tag_enter_is_lazy_allocated",          tag_enter_is_lazy_allocated);
    utest_run("tag_leave_is_lazy_allocated",          tag_leave_is_lazy_allocated);
    utest_run("tag_proto_has_enter_and_leave_slots",  tag_proto_has_enter_and_leave_slots);
    utest_run("tag_enter_setter_throws_protected_slot", tag_enter_setter_throws_protected_slot);
}
