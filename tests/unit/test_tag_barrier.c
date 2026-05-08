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
 *                actual abort fires only with URBI_DEBUG on (Gate G1 stretch). */

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
}
