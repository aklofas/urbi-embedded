/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: Event prototype native methods (spec #3 §7.3).
 *
 * Phase 7 (M6 stdlib): Event proto slots are UVAL_CLOSURE values whose
 * native_fn pointer routes OP_CALL through the Phase-3 native-method ABI
 * (vm, self, args, nargs, out).  Pre-Phase-7 they were UVAL_HOST_FN slots
 * carrying a (strand, argc, argv) host-fn — that legacy ABI is retired.
 *
 * Tests drive the natives directly through the closure->native_fn pointer
 * (mirroring what OP_CALL does); scripted-level coverage is in
 * test_event_new_scripted.c (Phase 7).
 *
 *   1. event_new_creates_uevent:
 *      vm->event_proto holds a "new" slot of kind UVAL_CLOSURE whose
 *      native_fn allocates a fresh UEvent and writes it into *out.
 *
 *   2. event_emit_method_dispatches_to_async:
 *      Calling Event.emit's native fn with self=UVAL_EVENT, args=[42] wakes
 *      a parked waiter and deposits the payload.
 *
 *   3. event_proto_has_all_four_slots:
 *      new / emit / syncEmit / waituntil all exist as UVAL_CLOSURE slots
 *      with native_fn != NULL.
 *
 *   4. event_new_raises_on_oom (EVENT-011 carry-forward):
 *      When urbi_event_create fails (alloc-spy injected OOM), Event.new
 *      returns UEXEC_THROW; *out is left as canonical urbi_make_nil(). */

#include "utest.h"

#include "vm/uvm.h"
#include "event/uevent_native.h"
#include "event/uevent.h"
#include "object/uobject.h"
#include "value/uintern.h"
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"  /* sched_strand_block (waiter park) */
#include "chunk/uchunk.h"
#include "runtime/uclosure.h"
#include "urbi/urbi.h"
#include "urbi/types.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* === Helper: pull the Phase-3 native_fn out of a UVAL_CLOSURE slot. === */
static urbi_native_method_fn
fetch_native_fn(UVM *vm, UObject *proto, const char *name, size_t name_len)
{
    USymbol *sym = (USymbol *)ustr_intern(vm, name, name_len);
    if (sym == NULL) return NULL;

    UValue slot;
    slot.kind = (uint8_t)UVAL_NIL;
    if (urbi_object_lookup(vm, proto, sym, &slot) != 0) return NULL;
    if (slot.kind != (uint8_t)UVAL_CLOSURE || slot.v.p == NULL) return NULL;

    UClosure *cl = (UClosure *)slot.v.p;
    return cl->native_fn;
}

/* ===================================================================
 * Test 1: event_new_creates_uevent
 * =================================================================== */

UTEST(event_new_creates_uevent)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_native_protos_init(&vm);

    UASSERT(vm.event_proto != NULL);
    if (vm.event_proto == NULL) { urbi_vm_destroy(&vm); return; }

    /* "new" slot must exist as UVAL_CLOSURE with a native_fn. */
    USymbol *sym_new = (USymbol *)ustr_intern(&vm, "new", 3);
    UASSERT(sym_new != NULL);
    if (sym_new == NULL) { urbi_vm_destroy(&vm); return; }

    UValue slot_val;
    slot_val.kind = (uint8_t)UVAL_NIL;
    UASSERT(urbi_object_lookup(&vm, vm.event_proto, sym_new, &slot_val) == 0);
    UASSERT_EQ((int)slot_val.kind, (int)UVAL_CLOSURE);
    if (slot_val.kind != (uint8_t)UVAL_CLOSURE) { urbi_vm_destroy(&vm); return; }

    UClosure *cl = (UClosure *)slot_val.v.p;
    UASSERT(cl != NULL && cl->native_fn != NULL);
    if (cl == NULL || cl->native_fn == NULL) { urbi_vm_destroy(&vm); return; }

    /* Receiver = Event proto; nargs = 0. */
    UValue self;
    self.kind = (uint8_t)UVAL_OBJECT;
    self.v.p  = (void *)vm.event_proto;

    UValue out = urbi_make_nil();
    int rc = cl->native_fn(&vm, self, NULL, 0, &out);
    UASSERT_EQ(rc, UEXEC_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_EVENT);
    if (out.kind == (uint8_t)UVAL_EVENT) {
        UEvent *e = uvalue_as_event(out);
        UASSERT(e != NULL);
        if (e != NULL) {
            UASSERT_EQ((int)e->type_tag, (int)UTYPE_EVENT);
            UASSERT(e->at_watchers_head == NULL);
            UASSERT(e->waiters_head     == NULL);
        }
    }

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 2: event_emit_method_dispatches_to_async
 * =================================================================== */

UTEST(event_emit_method_dispatches_to_async)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_native_protos_init(&vm);

    UASSERT(vm.event_proto != NULL);
    if (vm.event_proto == NULL) { urbi_vm_destroy(&vm); return; }

    urbi_native_method_fn fn = fetch_native_fn(&vm, vm.event_proto, "emit", 4);
    UASSERT(fn != NULL);
    if (fn == NULL) { urbi_vm_destroy(&vm); return; }

    /* Create an event and a waiter strand. */
    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);
    if (e == NULL) { urbi_vm_destroy(&vm); return; }

    /* Park through the real transition (v0.13.3 / SCHED-13: a raw
     * WAIT_EVENT stamp would bypass the strand_waiting_count increment
     * and trip the wake's no-saturation decrement). */
    UStrand waiter;
    ustrand_init(&waiter, &vm);
    waiter.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count++;     /* satisfy block's RUNNING-decrement */
    sched_strand_block(&waiter, USTRAND_REASON_EVENT, (uint64_t)(uintptr_t)e);
    waiter.wait_event_target   = e;
    waiter.next_event_waiter   = NULL;
    waiter.last_event_payload.kind = (uint8_t)UVAL_NIL;
    waiter.last_event_payload.v.i  = 0;
    e->waiters_head            = &waiter;

    /* Phase-3 ABI: self = UEvent, args = [42], nargs = 1. */
    UValue self = uvalue_from_event(e);
    UValue args[1];
    args[0].kind = (uint8_t)UVAL_INT;
    args[0].v.i  = 42;

    UValue out = urbi_make_nil();
    int rc = fn(&vm, self, args, 1, &out);
    UASSERT_EQ(rc, UEXEC_OK);

    /* emit returns NIL. */
    UASSERT_EQ((int)out.kind, (int)UVAL_NIL);

    /* Waiter should have been woken: state → READY, payload deposited. */
    UASSERT_EQ((int)waiter.state, (int)USTRAND_STATE_READY);
    UASSERT_EQ((int)waiter.last_event_payload.kind, (int)UVAL_INT);
    UASSERT_EQ((long long)waiter.last_event_payload.v.i, (long long)42);
    UASSERT(e->waiters_head == NULL);

    ustrand_destroy(&waiter, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 3: all four slots are installed
 * =================================================================== */

UTEST(event_proto_has_all_four_slots)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_native_protos_init(&vm);

    UASSERT(vm.event_proto != NULL);
    if (vm.event_proto == NULL) { urbi_vm_destroy(&vm); return; }

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
            UASSERT_EQ((int)v.kind, (int)UVAL_CLOSURE);
            if (v.kind == (uint8_t)UVAL_CLOSURE) {
                UClosure *cl = (UClosure *)v.v.p;
                UASSERT(cl != NULL && cl->native_fn != NULL);
            }
        }
    }

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 4 (EVENT-011 carry-forward): event_new_raises_on_oom
 *
 * Pre-Phase-7 the host-fn ABI returned a NIL UValue on OOM (silent failure
 * masquerading as success).  Phase 7's Phase-3 ABI surfaces OOM as a
 * UEXEC_THROW return; *out is left at canonical urbi_make_nil().
 * =================================================================== */

typedef struct {
    int alloc_calls;
    int fail_at;  /* -1: never; otherwise fail when alloc_calls > fail_at */
} EventNativeAllocSpy;

static void *
event_native_spy_alloc(void *ptr, size_t n, void *ud)
{
    EventNativeAllocSpy *spy = (EventNativeAllocSpy *)ud;
    if (n > 0 && ptr == NULL) {
        spy->alloc_calls++;
        if (spy->fail_at >= 0 && spy->alloc_calls > spy->fail_at) {
            return NULL;
        }
    }
    if (n == 0) { free(ptr); return NULL; }
    return realloc(ptr, n);
}

UTEST(event_new_raises_on_oom)
{
    EventNativeAllocSpy spy = { 0, -1 };

    UVM vm;
    urbi_vm_init(&vm, event_native_spy_alloc, &spy);
    urbi_native_protos_init(&vm);

    UASSERT(vm.event_proto != NULL);
    if (vm.event_proto == NULL) { urbi_vm_destroy(&vm); return; }

    urbi_native_method_fn fn = fetch_native_fn(&vm, vm.event_proto, "new", 3);
    UASSERT(fn != NULL);
    if (fn == NULL) { urbi_vm_destroy(&vm); return; }

    /* Arm the spy to fail the next allocation — urbi_event_create then
     * returns NULL and the native must take the OOM raise branch. */
    spy.fail_at = spy.alloc_calls;

    UValue self;
    self.kind = (uint8_t)UVAL_OBJECT;
    self.v.p  = (void *)vm.event_proto;

    UValue out = urbi_make_nil();
    int rc = fn(&vm, self, NULL, 0, &out);

    UASSERT_EQ(rc, UEXEC_THROW);

    /* *out must be bit-identical to canonical urbi_make_nil(). */
    UValue canonical = urbi_make_nil();
    UASSERT_EQ((int)out.kind, (int)canonical.kind);
    UASSERT(memcmp(&out, &canonical, sizeof(UValue)) == 0);

    /* Disarm spy before teardown. */
    spy.fail_at = -1;
    urbi_vm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void
test_event_native_suite(void)
{
    printf("test_event_native\n");
    utest_run("event_new_creates_uevent",              event_new_creates_uevent);
    utest_run("event_emit_method_dispatches_to_async", event_emit_method_dispatches_to_async);
    utest_run("event_proto_has_all_four_slots",        event_proto_has_all_four_slots);
    utest_run("event_new_raises_on_oom",               event_new_raises_on_oom);
}
