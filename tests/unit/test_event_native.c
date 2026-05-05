/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: Event prototype native methods (spec #3 §7.3).
 *
 * Source-level tests (e.g. `Event.new()`) are blocked by globals exposure
 * (T59) and class-decl infrastructure.  These tests drive via C-API:
 *
 *   1. event_new_creates_uevent:
 *      After urbi_native_protos_init, vm->event_proto is non-NULL and has a
 *      "new" slot holding a UVAL_HOST_FN.  Calling the native fn directly
 *      returns a UVAL_EVENT wrapping a fresh UEvent.
 *
 *   2. event_emit_method_dispatches_to_async:
 *      The "emit" slot exists.  Calling the native fn with an event receiver
 *      and an int payload wakes a parked waiter and deposits the payload.
 *
 *   3. event_proto_has_all_four_slots:
 *      new / emit / syncEmit / waituntil all exist as UVAL_HOST_FN slots. */

#include "utest.h"

#include "uvm.h"
#include "event_native.h"
#include "uevent.h"
#include "object/uobject.h"
#include "uintern.h"
#include "ustrand.h"
#include "umodule.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Test 1: event_new_creates_uevent
 * =================================================================== */

UTEST(event_new_creates_uevent)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    urbi_native_protos_init(&vm);

    UASSERT(vm.event_proto != NULL);
    if (vm.event_proto == NULL) { uvm_destroy(&vm); return; }

    /* "new" slot must exist and be UVAL_HOST_FN. */
    USymbol *sym_new = (USymbol *)ustr_intern(&vm, "new", 3);
    UASSERT(sym_new != NULL);
    if (sym_new == NULL) { uvm_destroy(&vm); return; }

    UValue slot_val;
    slot_val.kind = (uint8_t)UVAL_NIL;
    UASSERT(urbi_object_lookup(&vm, vm.event_proto, sym_new, &slot_val) == 0);
    UASSERT_EQ((int)slot_val.kind, (int)UVAL_HOST_FN);
    if (slot_val.kind != (uint8_t)UVAL_HOST_FN) { uvm_destroy(&vm); return; }

    /* Create a minimal stack-local strand so s->vm is valid. */
    UStrand s;
    ustrand_init(&s, &vm);

    /* argv[0] = proto receiver; nargs = 1. */
    UValue argv[1];
    argv[0].kind = (uint8_t)UVAL_OBJECT;
    argv[0].v.p  = (void *)vm.event_proto;

    UHostFn fn  = (UHostFn)(uintptr_t)slot_val.v.p;
    UValue result = fn(&s, 1, argv);

    /* Returned value must be UVAL_EVENT. */
    UASSERT_EQ((int)result.kind, (int)UVAL_EVENT);
    if (result.kind == (uint8_t)UVAL_EVENT) {
        UEvent *e = uvalue_as_event(result);
        UASSERT(e != NULL);
        if (e != NULL) {
            UASSERT_EQ((int)e->type_tag, (int)UTYPE_EVENT);
            UASSERT(e->at_watchers_head == NULL);
            UASSERT(e->waiters_head     == NULL);
        }
    }

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ===================================================================
 * Test 2: event_emit_method_dispatches_to_async
 * =================================================================== */

UTEST(event_emit_method_dispatches_to_async)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    urbi_native_protos_init(&vm);

    UASSERT(vm.event_proto != NULL);
    if (vm.event_proto == NULL) { uvm_destroy(&vm); return; }

    /* Locate the "emit" slot. */
    USymbol *sym_emit = (USymbol *)ustr_intern(&vm, "emit", 4);
    UASSERT(sym_emit != NULL);
    if (sym_emit == NULL) { uvm_destroy(&vm); return; }

    UValue slot_val;
    slot_val.kind = (uint8_t)UVAL_NIL;
    UASSERT(urbi_object_lookup(&vm, vm.event_proto, sym_emit, &slot_val) == 0);
    UASSERT_EQ((int)slot_val.kind, (int)UVAL_HOST_FN);
    if (slot_val.kind != (uint8_t)UVAL_HOST_FN) { uvm_destroy(&vm); return; }

    /* Create an event and a waiter strand. */
    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);
    if (e == NULL) { uvm_destroy(&vm); return; }

    UStrand waiter;
    ustrand_init(&waiter, &vm);
    waiter.state               = USTRAND_WAIT_EVENT;
    waiter.wait_event_target   = e;
    waiter.next_event_waiter   = NULL;
    waiter.last_event_payload.kind = (uint8_t)UVAL_NIL;
    waiter.last_event_payload.v.i  = 0;
    e->waiters_head            = &waiter;

    /* Call the native emit fn: argv = [event, payload=42]. */
    UStrand caller;
    ustrand_init(&caller, &vm);

    UValue argv[2];
    argv[0] = uvalue_from_event(e);
    argv[1].kind = (uint8_t)UVAL_INT;
    argv[1].v.i  = 42;

    UHostFn fn  = (UHostFn)(uintptr_t)slot_val.v.p;
    UValue ret  = fn(&caller, 2, argv);

    /* emit returns NIL. */
    UASSERT_EQ((int)ret.kind, (int)UVAL_NIL);

    /* Waiter should have been woken: state → READY, payload deposited. */
    UASSERT_EQ((int)waiter.state, (int)USTRAND_STATE_READY);
    UASSERT_EQ((int)waiter.last_event_payload.kind, (int)UVAL_INT);
    UASSERT_EQ((long long)waiter.last_event_payload.v.i, (long long)42);
    /* waiters_head cleared. */
    UASSERT(e->waiters_head == NULL);

    ustrand_destroy(&caller, &vm);
    ustrand_destroy(&waiter, &vm);
    uvm_destroy(&vm);
}

/* ===================================================================
 * Test 3: all four slots are installed
 * =================================================================== */

UTEST(event_proto_has_all_four_slots)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    urbi_native_protos_init(&vm);

    UASSERT(vm.event_proto != NULL);
    if (vm.event_proto == NULL) { uvm_destroy(&vm); return; }

    struct { const char *name; size_t len; } slots[] = {
        { "new",       3 },
        { "emit",      4 },
        { "syncEmit",  8 },
        { "waituntil", 9 },
    };
    int i;
    for (i = 0; i < 4; i++) {
        USymbol *sym = (USymbol *)ustr_intern(&vm, slots[i].name, slots[i].len);
        UASSERT(sym != NULL);
        if (sym == NULL) continue;

        UValue v;
        v.kind = (uint8_t)UVAL_NIL;
        int hit = (urbi_object_lookup(&vm, vm.event_proto, sym, &v) == 0);
        UASSERT(hit);
        if (hit) {
            UASSERT_EQ((int)v.kind, (int)UVAL_HOST_FN);
        }
    }

    uvm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void
test_event_native_suite(void)
{
    printf("test_event_native\n");
    utest_run("event_new_creates_uevent",              event_new_creates_uevent);
    utest_run("event_emit_method_dispatches_to_async", event_emit_method_dispatches_to_async);
    utest_run("event_proto_has_all_four_slots",        event_proto_has_all_four_slots);
}
