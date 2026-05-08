/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: tag enter/leave + GC barrier fixes from v0.5.7-fixes Phase 12
 * (T53-T56).
 *
 * T53 TAGCH-001: tag_enter_getter / tag_leave_getter route the field write of
 *                the freshly-allocated UEvent through the Dijkstra forward
 *                barrier.  Without the barrier, a black UTag storing a white
 *                UEvent breaks the tri-color invariant: the new event would
 *                not be reached by the mark phase and could be swept while
 *                still reachable.
 *
 * T54 TAGCH-002: getter stubs (tag_enter_getter_stub / tag_leave_getter_stub)
 *                are unreachable at M5 baseline (OP_CALL wiring for tag-typed
 *                receivers lands at M6).  Their unsafe receiver casts are
 *                replaced by URBI_INTERNAL_ASSERT(0 && "unreachable: stub"),
 *                no-op in release.  Test asserts the slots remain installed
 *                on vm.tag_proto so the proto is correctly populated; the
 *                actual abort fires only with URBI_DEBUG on (Gate G1 stretch).
 *
 * T55 TAGCH-004: tag_native_register propagates urbi_register_fn failures
 *                instead of dropping them.  When the slot installer OOMs on
 *                any of the four slots, vm->tag_proto is reset to NULL and
 *                the function returns UVM_OOM.  Mirrors the Phase-11 T50
 *                pattern for event_native_register.
 *
 * T56 TAGCH-016: URBI_ASSERT_NOT_ISR is asserted at every gc_alloc-bearing
 *                callsite in utag_native.c (tag_enter_getter, tag_leave_getter,
 *                tag_native_register).  Mirrors src/changed/uchanged.c:32.
 *                Test asserts the happy path under no ISR — the assertion
 *                fires only when an ISR-check function reports true (Gate G1
 *                stretch, URBI_DEBUG-only). */

#include "utest.h"

#include "vm/uvm.h"
#include "tag/utag.h"
#include "tag/utag_native.h"
#include "event/uevent.h"
#include "event/uevent_native.h"
#include "object/uobject.h"
#include "value/uintern.h"
#include "module/umodule.h"
#include "gc/ugc_incremental.h"   /* IS_BLACK / IS_GRAY / UGC_COLOR_* */
#include "urbi/urbi.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * T53: tag_enter_event_creation_triggers_dijkstra_barrier
 *
 * Force the UTag cell to BLACK before calling the lazy-alloc getter so
 * the Dijkstra forward barrier on the field write must shade the
 * freshly-allocated UEvent gray (or re-gray the parent).  Mirrors the
 * pattern in test_ugc_barrier.c::barrier_black_stores_white_shades.
 * =================================================================== */

UTEST(tag_enter_event_creation_triggers_dijkstra_barrier)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UTag *t = utag_create(&vm);
    UASSERT(t != NULL);
    if (t == NULL) { urbi_vm_destroy(&vm); return; }

    /* Force the UTag BLACK.  At this point enter_event is still NULL —
     * the lazy-alloc getter is about to allocate a fresh white UEvent
     * and assign it into the BLACK parent.  Without the barrier the
     * new white child would be swept through. */
    UCell *parent = (UCell *)t;
    parent->gc_byte = (uint8_t)((parent->gc_byte & ~UGC_COLOR_MASK)
                                | UGC_COLOR_BLACK);
    UASSERT(IS_BLACK(parent));
    UASSERT(t->enter_event == NULL);

    UValue r = tag_enter_getter(&vm, t);
    UASSERT_EQ((int)r.kind, (int)UVAL_EVENT);
    UASSERT(t->enter_event != NULL);
    if (t->enter_event == NULL) { urbi_vm_destroy(&vm); return; }

    /* The Dijkstra forward barrier shades the new event gray to maintain
     * the no-black-to-white invariant.  Parent stays black. */
    UCell *child = (UCell *)t->enter_event;
    UASSERT(IS_GRAY(child));
    UASSERT(IS_BLACK(parent));

    urbi_vm_destroy(&vm);
}

/* Same shape but for tag_leave_getter. */
UTEST(tag_leave_event_creation_triggers_dijkstra_barrier)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UTag *t = utag_create(&vm);
    UASSERT(t != NULL);
    if (t == NULL) { urbi_vm_destroy(&vm); return; }

    UCell *parent = (UCell *)t;
    parent->gc_byte = (uint8_t)((parent->gc_byte & ~UGC_COLOR_MASK)
                                | UGC_COLOR_BLACK);
    UASSERT(IS_BLACK(parent));
    UASSERT(t->leave_event == NULL);

    UValue r = tag_leave_getter(&vm, t);
    UASSERT_EQ((int)r.kind, (int)UVAL_EVENT);
    UASSERT(t->leave_event != NULL);
    if (t->leave_event == NULL) { urbi_vm_destroy(&vm); return; }

    UCell *child = (UCell *)t->leave_event;
    UASSERT(IS_GRAY(child));
    UASSERT(IS_BLACK(parent));

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T54: stubs_assert_unreachable_in_debug
 *
 * After T54 the two getter stubs are URBI_INTERNAL_ASSERT(0)-then-NIL.
 * In release builds the assertion collapses to a no-op; the slots remain
 * installed on the proto.  We exercise the happy path: vm.tag_proto has
 * "enter" and "leave" UVAL_HOST_FN slots after urbi_native_protos_init
 * regardless of build flavor.  Gate G1 (URBI_DEBUG) actually triggers
 * the abort if anything calls into the stubs.
 * =================================================================== */

UTEST(stubs_assert_unreachable_in_debug)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_native_protos_init(&vm);

    UASSERT(vm.tag_proto != NULL);
    if (vm.tag_proto == NULL) { urbi_vm_destroy(&vm); return; }

    /* Slots are installed (they'll only assert if invoked). */
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
        UASSERT(urbi_object_lookup(&vm, vm.tag_proto, sym, &v) == 0);
        UASSERT_EQ((int)v.kind, (int)UVAL_HOST_FN);
        UASSERT(v.v.p != NULL);
    }

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T55: tag_native_register_propagates_failures
 *
 * Run with a failing allocator that succeeds long enough for the proto
 * object to allocate but OOMs on a later urbi_register_fn slot install.
 * After T55, tag_native_register clears vm->tag_proto on any failure
 * and returns UVM_OOM.  Mirrors the Phase-11
 * event_native_register_propagates_register_oom test pattern.
 * =================================================================== */

typedef struct {
    int alloc_calls;
    int fail_at;  /* -1 means never fail; trigger NULL when alloc_calls > fail_at */
} TagAllocSpy;

static void *tag_spy_alloc(void *ptr, size_t n, void *ud)
{
    TagAllocSpy *spy = (TagAllocSpy *)ud;
    if (n == 0) {
        free(ptr);
        return NULL;
    }
    if (ptr == NULL) {
        spy->alloc_calls++;
        if (spy->fail_at >= 0 && spy->alloc_calls > spy->fail_at) {
            return NULL;
        }
    }
    return realloc(ptr, n);
}

UTEST(tag_native_register_propagates_failures)
{
    /* Step 1: count clean allocations.  We need an upper bound on the
     * proto + first slot install. */
    TagAllocSpy probe = { 0, -1 };
    UVM probe_vm;
    urbi_vm_init(&probe_vm, tag_spy_alloc, &probe);

    UVMError ok = tag_native_register(&probe_vm);
    UASSERT_EQ((int)ok, (int)UVM_OK);
    UASSERT(probe_vm.tag_proto != NULL);
    int total_clean_calls = probe.alloc_calls;
    UASSERT(total_clean_calls > 2);  /* proto + at least one slot */
    urbi_vm_destroy(&probe_vm);

    /* Step 2: re-run with fail_at set partway through, after the proto
     * lands but before all four slot installs succeed.  After T55,
     * tag_proto must be NULL on return and the error must be UVM_OOM. */
    TagAllocSpy spy = { 0, total_clean_calls - 2 };
    UVM vm;
    urbi_vm_init(&vm, tag_spy_alloc, &spy);

    UVMError err = tag_native_register(&vm);
    UASSERT_EQ((int)err, (int)UVM_OOM);
    UASSERT(vm.tag_proto == NULL);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T56: tag_native_calls_assert_not_isr
 *
 * After T56 every gc_alloc-bearing path in utag_native.c is prefixed by
 * URBI_ASSERT_NOT_ISR(vm).  In release builds the macro collapses to
 * ((void)0) so all three callers behave normally.  Gate G1 (URBI_DEBUG
 * with an isr_check_fn that returns true) would trip the assertion;
 * we cover the happy path here.
 * =================================================================== */

UTEST(tag_native_calls_assert_not_isr)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* tag_native_register: must complete without tripping the macro
     * because no ISR check function is registered (urbi_in_isr returns
     * false in that case). */
    UVMError err = tag_native_register(&vm);
    UASSERT_EQ((int)err, (int)UVM_OK);
    UASSERT(vm.tag_proto != NULL);
    if (vm.tag_proto == NULL) { urbi_vm_destroy(&vm); return; }

    /* tag_enter_getter / tag_leave_getter: lazy-alloc paths must also
     * complete without panic in the no-ISR case. */
    UTag *t = utag_create(&vm);
    UASSERT(t != NULL);
    if (t == NULL) { urbi_vm_destroy(&vm); return; }

    UValue re = tag_enter_getter(&vm, t);
    UASSERT_EQ((int)re.kind, (int)UVAL_EVENT);
    UASSERT(t->enter_event != NULL);

    UValue rl = tag_leave_getter(&vm, t);
    UASSERT_EQ((int)rl.kind, (int)UVAL_EVENT);
    UASSERT(t->leave_event != NULL);

    urbi_vm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void
test_tag_barrier_suite(void)
{
    printf("test_tag_barrier\n");
    utest_run("tag_enter_event_creation_triggers_dijkstra_barrier",
              tag_enter_event_creation_triggers_dijkstra_barrier);
    utest_run("tag_leave_event_creation_triggers_dijkstra_barrier",
              tag_leave_event_creation_triggers_dijkstra_barrier);
    utest_run("stubs_assert_unreachable_in_debug",
              stubs_assert_unreachable_in_debug);
    utest_run("tag_native_register_propagates_failures",
              tag_native_register_propagates_failures);
    utest_run("tag_native_calls_assert_not_isr",
              tag_native_calls_assert_not_isr);
}
