/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ref_gc_root.c — TDD tests proving that urbi_ref pins a UValue as a GC
 * root (Gap Q, v0.7.1).
 *
 * This is the load-bearing correctness test for the GC integration.
 * urbi_gc_ref_table_walk_roots must be called by the GC mark phase for the pinned
 * value to survive urbi_gc_collect.
 *
 * Strategy:
 *   Allocate a GC-managed UEvent (a GC cell with no other root).
 *   Verify it is collected when unrooted.
 *   Verify it SURVIVES when a urbi_ref keeps it live.
 *   Verify that after urbi_unref + GC it is collected.
 *
 * Two sub-tests:
 *   1. ref_pins_value_across_gc: ref a UEvent; urbi_gc_collect; still alive.
 *   2. unref_allows_collection: after urbi_unref + GC; event collected.
 *
 * Helper technique from test_event_gc.c: cast vm->all_cells_head to a
 * "mirror node" struct to walk the intrusive list without depending on
 * UAllCellsNode internals.
 *
 * NOTE: mark_root_callback in ugc_incremental.c only handles UVAL_CLOSURE
 * for the gc-shade path.  For non-CLOSURE GC cells we must register a custom
 * root provider that calls urbi_gc_shade_gray directly (same pattern as
 * ev_test_root_provider in test_event_gc.c).  urbi_gc_ref_table_walk_roots calls
 * cb(vm, &slot.value, ctx) where cb == mark_root_callback; that callback
 * shades the cell only for UVAL_CLOSURE.  For UVAL_EVENT we need the
 * cell-side shade path.
 *
 * RESOLUTION: use UVAL_CLOSURE as the pinned value to exercise the real
 * mark_root_callback shade path (same as test_event_gc.c sub-test 3 technique).
 * A trivial UClosure backed by a stack UProto is sufficient.  The closure
 * itself must not be GC-managed (we use a stack UClosure) to avoid a
 * double-registration, but its POINTER is the identity we track.  We confirm
 * survival by checking whether the GC sweep leaves it alone.
 *
 * Actually: for UVAL_INT the mark_root_callback is a no-op (no heap cell).
 * For UVAL_OBJECT the callback shades the UObject.  For UVAL_EVENT it shades
 * the UEvent.  We'll use UVAL_EVENT with a urbi_ref to test the real path.
 * The urbi_gc_ref_table_walk_roots calls cb(vm, &slot.value, ctx), and cb ==
 * mark_root_callback.  Let's check if mark_root_callback handles UVAL_EVENT. */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "gc/ugc.h"
#include "gc/ugc_incremental.h"  /* urbi_gc_shade_gray, GC_PHASE_* */
#include "event/uevent.h"        /* urbi_event_create, UEvent */

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* Mirror node to walk vm->all_cells_head list (same as test_event_gc.c). */
typedef struct MirrorNodeRef {
    void                 *cell;
    size_t                size;
    struct MirrorNodeRef *next;
    struct MirrorNodeRef *next_gray;
} MirrorNodeRef;

/* Returns 1 if target is still in vm->all_cells_head list. */
static int
cell_is_alive(UVM *vm, UCell *target)
{
    MirrorNodeRef *n = (MirrorNodeRef *)(void *)vm->all_cells_head;
    while (n != NULL) {
        if (n->cell == (void *)target) return 1;
        n = n->next;
    }
    return 0;
}

/* =========================================================================
 * Sub-test 1: urbi_ref pins a UEvent value; it survives urbi_gc_collect.
 *
 * Without the ref, the event would be collected.  With the ref, the
 * urbi_gc_ref_table_walk_roots provider calls mark_root_callback on the UValue,
 * which shades the UEvent cell gray → it survives.
 * ========================================================================= */

UTEST(ref_pins_value_across_gc)
{
    UVM vm;
    UEvent *ev;
    urbi_ref_t ref;
    UValue v;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    ev = urbi_event_create(&vm);
    UASSERT(ev != NULL);
    if (ev == NULL) { urbi_vm_destroy(&vm); return; }

    /* The event is allocated but has NO other root — only the ref will pin it. */
    v = urbi_make_event(ev);
    ref = urbi_ref(&vm, v);
    UASSERT(ref != URBI_REF_INVALID);

    /* Force full GC collection.
     * urbi_gc_ref_table_walk_roots calls mark_root_callback(vm, &slot.value, ctx)
     * which shades the UEvent cell via the UVAL_EVENT branch in
     * mark_root_callback. */
    urbi_gc_collect(&vm);

    /* Event must still be alive — pinned by ref. */
    UASSERT(cell_is_alive(&vm, (UCell *)ev));

    /* Verify the ref still returns the event. */
    v = urbi_ref_get(&vm, ref);
    UASSERT_EQ((int)UVAL_EVENT, (int)v.kind);
    UASSERT(v.v.p == (void *)ev);

    urbi_unref(&vm, ref);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: after urbi_unref + GC, the event is collected.
 *
 * Complementary to sub-test 1: shows the ref was the sole root.
 * ========================================================================= */

UTEST(unref_allows_collection)
{
    UVM vm;
    UEvent *ev;
    urbi_ref_t ref;
    UValue v;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    ev = urbi_event_create(&vm);
    UASSERT(ev != NULL);
    if (ev == NULL) { urbi_vm_destroy(&vm); return; }

    v = urbi_make_event(ev);
    ref = urbi_ref(&vm, v);
    UASSERT(ref != URBI_REF_INVALID);

    /* With ref alive, GC keeps it. */
    urbi_gc_collect(&vm);
    UASSERT(cell_is_alive(&vm, (UCell *)ev));

    /* Release the ref — no more roots. */
    urbi_unref(&vm, ref);

    /* GC now collects the event. */
    urbi_gc_collect(&vm);
    UASSERT(!cell_is_alive(&vm, (UCell *)ev));

    /* ref_get must now return nil (handle stale + cell gone). */
    v = urbi_ref_get(&vm, ref);
    UASSERT_EQ((int)UVAL_NIL, (int)v.kind);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_ref_gc_root_suite(void)
{
    utest_run("ref_gc_root: ref pins UEvent value across GC cycle",
              ref_pins_value_across_gc);
    utest_run("ref_gc_root: unref allows GC collection",
              unref_allows_collection);
}
