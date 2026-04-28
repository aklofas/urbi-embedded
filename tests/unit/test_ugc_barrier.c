/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: three GC write-barrier surfaces — forward Dijkstra barrier
 * and observer_dirty hook.  Row 10 §4.  T25. */

#include "utest.h"
#include "ugc_capi.h"
#include "ugc_incremental.h"
#include "uvm.h"
#include <stdlib.h>

/* === Test helper: construct a UValue from a UCell* (test-only) ===
 *
 * Tags the value as UVAL_CLOSURE and stores the cell pointer in v.v.p.
 * At T25 no real UClosure objects have a UCell header, so barrier tests
 * use synthetic cells allocated via urbi_gc_alloc.  Production code will
 * use proper UClosure-bearing values once UClosure embeds UCell (M4).
 *
 * This helper is not exported — it exists only to build test UValues. */
static UValue
uvalue_from_test_cell(UCell *c)
{
    UValue v = {0};
    v.kind = UVAL_CLOSURE;
    v.v.p = (void *)c;
    return v;
}

#define UTEST(name) static void name(void)

/* ===== Test 1: black parent + white child → child shaded gray ===== */

/* Forward barrier (Dijkstra): when a black parent stores a white heap child,
 * the child must be painted gray so it will be scanned before sweep.
 * This is the core tri-color invariant maintenance step. */
UTEST(barrier_black_stores_white_shades)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UCell *parent = urbi_gc_alloc(&vm, sizeof(UCell) + 32u, UTYPE_OBJECT);
    UCell *child  = urbi_gc_alloc(&vm, sizeof(UCell) + 32u, UTYPE_OBJECT);

    /* Force parent BLACK, child WHITE (current_white). */
    parent->gc_byte = (uint8_t)((parent->gc_byte & ~UGC_COLOR_MASK) | UGC_COLOR_BLACK);
    child->gc_byte  = (uint8_t)((child->gc_byte  & ~UGC_COLOR_MASK) | vm.current_white);

    urbi_gc_slot_write(&vm, parent, 0u, uvalue_from_test_cell(child));

    /* Child must be gray now — shaded by the forward barrier. */
    UASSERT(IS_GRAY(child));
    /* Parent remains black. */
    UASSERT(IS_BLACK(parent));

    uvm_destroy(&vm);
}

/* ===== Test 2: gray parent + white child → child NOT shaded ===== */

/* The barrier only fires on black parents.  A gray parent has not yet been
 * fully scanned, so adding an edge to a white child is safe — the mark phase
 * will scan the parent's payload and discover the child. */
UTEST(barrier_gray_stores_white_no_shade)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UCell *parent = urbi_gc_alloc(&vm, sizeof(UCell) + 32u, UTYPE_OBJECT);
    UCell *child  = urbi_gc_alloc(&vm, sizeof(UCell) + 32u, UTYPE_OBJECT);

    /* Force parent GRAY, child WHITE. */
    parent->gc_byte = (uint8_t)((parent->gc_byte & ~UGC_COLOR_MASK) | UGC_COLOR_GRAY);
    child->gc_byte  = (uint8_t)((child->gc_byte  & ~UGC_COLOR_MASK) | vm.current_white);

    urbi_gc_slot_write(&vm, parent, 0u, uvalue_from_test_cell(child));

    /* Child must remain white — gray parent does not trigger the barrier. */
    UASSERT(IS_WHITE(child));

    uvm_destroy(&vm);
}

/* ===== Test 3: white parent + white child → child NOT shaded ===== */

/* Both parent and child are white (unvisited).  No barrier action needed. */
UTEST(barrier_white_stores_white_no_shade)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UCell *parent = urbi_gc_alloc(&vm, sizeof(UCell) + 32u, UTYPE_OBJECT);
    UCell *child  = urbi_gc_alloc(&vm, sizeof(UCell) + 32u, UTYPE_OBJECT);

    /* Both born current_white — no forced color change needed. */
    UASSERT(IS_WHITE(parent));
    UASSERT(IS_WHITE(child));

    urbi_gc_slot_write(&vm, parent, 0u, uvalue_from_test_cell(child));

    /* Child must remain white. */
    UASSERT(IS_WHITE(child));

    uvm_destroy(&vm);
}

/* ===== Test 4: register_write is a no-op — GC state unchanged ===== */

/* VM registers are roots walked at every mark phase; no barrier is needed.
 * The call must compile and not mutate any GC state. */
UTEST(barrier_register_write_no_op)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UCell *child = urbi_gc_alloc(&vm, sizeof(UCell) + 32u, UTYPE_OBJECT);

    /* Record color before the call. */
    uint8_t color_before = (uint8_t)(child->gc_byte & UGC_COLOR_MASK);

    /* urbi_gc_register_write has no strand to act on; pass NULL — the
     * implementation does nothing with the strand or child. */
    urbi_gc_register_write(&vm, NULL, 0u, uvalue_from_test_cell(child));

    /* Color unchanged; gray work-list still empty. */
    UASSERT_EQ((child->gc_byte & UGC_COLOR_MASK), color_before);
    UASSERT(vm.gray_work_head == NULL);

    uvm_destroy(&vm);
}

/* ===== Test 5: observer bit set + stub observer_dirty (T25 no-op) ===== */

/* When UGC_HAS_WATCHER_OBSERVER is set on the parent, the barrier calls
 * observer_dirty.  At T25 the stub is a no-op; watcher_dirty_count stays zero.
 * T34 will replace the stub with a real implementation. */
UTEST(barrier_observer_bit_calls_stub)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UCell *parent = urbi_gc_alloc(&vm, sizeof(UCell) + 32u, UTYPE_OBJECT);
    UCell *child  = urbi_gc_alloc(&vm, sizeof(UCell) + 32u, UTYPE_OBJECT);

    /* Mark parent as having a watcher observer and force it BLACK. */
    parent->gc_byte |= UGC_HAS_WATCHER_OBSERVER;
    parent->gc_byte  = (uint8_t)((parent->gc_byte & ~UGC_COLOR_MASK) | UGC_COLOR_BLACK);
    child->gc_byte   = (uint8_t)((child->gc_byte  & ~UGC_COLOR_MASK) | vm.current_white);

    uint32_t dirty_before = vm.watcher_dirty_count;

    urbi_gc_slot_write(&vm, parent, 42u, uvalue_from_test_cell(child));

    /* T25: stub does nothing — watcher_dirty_count unchanged.
     * T34: UASSERT_EQ(vm.watcher_dirty_count, dirty_before + 1u); */
    (void)dirty_before;

    /* GC barrier still fires: child was white + parent was black → child gray. */
    UASSERT(IS_GRAY(child));

    uvm_destroy(&vm);
}

/* ===== Test 6: upvalue_write black parent + white child → child shaded ===== */

/* urbi_gc_upvalue_write applies the same forward Dijkstra barrier as
 * slot_write.  Uses a synthetic UCell cast as UClosure* (T25 test-only pattern;
 * production use deferred to M4 when UClosure embeds UCell). */
UTEST(barrier_upvalue_black_stores_white_shades)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* parent_cell stands in for a UClosure (T25 synthetic). */
    UCell *parent_cell = urbi_gc_alloc(&vm, sizeof(UCell) + 32u, UTYPE_OBJECT);
    UCell *child       = urbi_gc_alloc(&vm, sizeof(UCell) + 32u, UTYPE_OBJECT);

    parent_cell->gc_byte = (uint8_t)((parent_cell->gc_byte & ~UGC_COLOR_MASK) | UGC_COLOR_BLACK);
    child->gc_byte       = (uint8_t)((child->gc_byte       & ~UGC_COLOR_MASK) | vm.current_white);

    urbi_gc_upvalue_write(&vm, (struct UClosure *)parent_cell, 0u,
                          uvalue_from_test_cell(child));

    /* Child must be gray — forward barrier fired. */
    UASSERT(IS_GRAY(child));
    UASSERT(IS_BLACK(parent_cell));

    uvm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void test_ugc_barrier_suite(void)
{
    utest_run("barrier_black_stores_white_shades",
              barrier_black_stores_white_shades);
    utest_run("barrier_gray_stores_white_no_shade",
              barrier_gray_stores_white_no_shade);
    utest_run("barrier_white_stores_white_no_shade",
              barrier_white_stores_white_no_shade);
    utest_run("barrier_register_write_no_op",
              barrier_register_write_no_op);
    utest_run("barrier_observer_bit_calls_stub",
              barrier_observer_bit_calls_stub);
    utest_run("barrier_upvalue_black_stores_white_shades",
              barrier_upvalue_black_stores_white_shades);
}
