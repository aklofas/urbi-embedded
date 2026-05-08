/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: tag enter/leave + GC barrier fixes from v0.5.7-fixes Phase 12
 * (T53-T56).
 *
 * T53 TAGCH-001: tag_enter_getter / tag_leave_getter route the field write of
 *                the freshly-allocated UEvent through the Dijkstra forward
 *                barrier.  Without the barrier, a black UTag storing a white
 *                UEvent breaks the tri-color invariant: the new event would
 *                not be reached by the mark phase and could be swept while
 *                still reachable. */

#include "utest.h"

#include "vm/uvm.h"
#include "tag/utag.h"
#include "tag/utag_native.h"
#include "event/uevent.h"
#include "event/uevent_native.h"
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

/* ===== Suite entry point ===== */

void
test_tag_barrier_suite(void)
{
    printf("test_tag_barrier\n");
    utest_run("tag_enter_event_creation_triggers_dijkstra_barrier",
              tag_enter_event_creation_triggers_dijkstra_barrier);
    utest_run("tag_leave_event_creation_triggers_dijkstra_barrier",
              tag_leave_event_creation_triggers_dijkstra_barrier);
}
