/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: three GC write-barrier surfaces — forward Dijkstra barrier
 * and observer_dirty hook.  Row 10 §4.  T25.
 *
 * URBI_GC_STRESS disarm (v0.13.2): these tests allocate UNROOTED cells and
 * hand-paint their colors to exercise the barrier state table — the suite
 * tests barrier mechanics, not rooted-program behaviour.  Collect-on-
 * every-alloc sweeps the deliberately-unrooted cells between the paired
 * allocations, so each test sets vm.gc_stress_armed = 0 right after init.
 * Structural-by-design, not a rooting bug (refactor-3 TEST-GAP-01
 * stress-exempt list). */

#include "utest.h"
#include "urbi/gc.h"
#include "gc/ugc_incremental.h"
#include "runtime/uclosure.h"
#include "vm/uvm.h"
#include <stddef.h>
#include <stdlib.h>

/* === Test helper: construct a UValue from a UCell* (test-only) ===
 *
 * Tags the value as UVAL_CLOSURE and stores the cell pointer in v.v.p.
 * At M4 UClosure embeds UCell at offset 0, so uvalue_as_cell is
 * well-defined for real closure values in production code; these unit
 * tests still use synthetic UCell objects allocated via urbi_gc_alloc to
 * isolate barrier behavior from closure construction.
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
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    UCell *parent = urbi_gc_alloc(&vm, sizeof(UCell) + 32U, UTYPE_OBJECT);
    UCell *child  = urbi_gc_alloc(&vm, sizeof(UCell) + 32U, UTYPE_OBJECT);

    /* Force parent BLACK, child WHITE (current_white). */
    parent->gc_byte = (uint8_t)((parent->gc_byte & ~UGC_COLOR_MASK) | UGC_COLOR_BLACK);
    child->gc_byte  = (uint8_t)((child->gc_byte  & ~UGC_COLOR_MASK) | vm.current_white);

    urbi_gc_slot_pre_store(&vm, parent, 0U, uvalue_from_test_cell(child));

    /* Child must be gray now — shaded by the forward barrier. */
    UASSERT(IS_GRAY(child));
    /* Parent remains black. */
    UASSERT(IS_BLACK(parent));

    urbi_vm_destroy(&vm);
}

/* ===== Test 2: gray parent + white child → child NOT shaded ===== */

/* The barrier only fires on black parents.  A gray parent has not yet been
 * fully scanned, so adding an edge to a white child is safe — the mark phase
 * will scan the parent's payload and discover the child. */
UTEST(barrier_gray_stores_white_no_shade)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    UCell *parent = urbi_gc_alloc(&vm, sizeof(UCell) + 32U, UTYPE_OBJECT);
    UCell *child  = urbi_gc_alloc(&vm, sizeof(UCell) + 32U, UTYPE_OBJECT);

    /* Force parent GRAY, child WHITE. */
    parent->gc_byte = (uint8_t)((parent->gc_byte & ~UGC_COLOR_MASK) | UGC_COLOR_GRAY);
    child->gc_byte  = (uint8_t)((child->gc_byte  & ~UGC_COLOR_MASK) | vm.current_white);

    urbi_gc_slot_pre_store(&vm, parent, 0U, uvalue_from_test_cell(child));

    /* Child must remain white — gray parent does not trigger the barrier. */
    UASSERT(IS_WHITE(child));

    urbi_vm_destroy(&vm);
}

/* ===== Test 3: white parent + white child → child NOT shaded ===== */

/* Both parent and child are white (unvisited).  No barrier action needed. */
UTEST(barrier_white_stores_white_no_shade)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    UCell *parent = urbi_gc_alloc(&vm, sizeof(UCell) + 32U, UTYPE_OBJECT);
    UCell *child  = urbi_gc_alloc(&vm, sizeof(UCell) + 32U, UTYPE_OBJECT);

    /* Both born current_white — no forced color change needed. */
    UASSERT(IS_WHITE(parent));
    UASSERT(IS_WHITE(child));

    urbi_gc_slot_pre_store(&vm, parent, 0U, uvalue_from_test_cell(child));

    /* Child must remain white. */
    UASSERT(IS_WHITE(child));

    urbi_vm_destroy(&vm);
}

/* ===== Test 4: register_write is a no-op — GC state unchanged ===== */

/* VM registers are roots walked at every mark phase; no barrier is needed.
 * The call must compile and not mutate any GC state. */
UTEST(barrier_register_write_no_op)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    UCell *child = urbi_gc_alloc(&vm, sizeof(UCell) + 32U, UTYPE_OBJECT);

    /* Record color before the call. */
    uint8_t color_before = (uint8_t)(child->gc_byte & UGC_COLOR_MASK);

    /* urbi_gc_register_write has no strand to act on; pass NULL — the
     * implementation does nothing with the strand or child. */
    urbi_gc_register_write(&vm, NULL, 0U, uvalue_from_test_cell(child));

    /* Color unchanged; gray work-list still empty. */
    UASSERT_EQ((child->gc_byte & UGC_COLOR_MASK), color_before);
    UASSERT(vm.gray_work_head == NULL);

    urbi_vm_destroy(&vm);
}

/* ===== Test 5: observer bit set + stub observer_dirty (T25 no-op) ===== */

/* When UGC_HAS_WATCHER_OBSERVER is set on the parent, the barrier calls
 * observer_dirty.  At T25 the stub is a no-op; watcher_dirty_count stays zero.
 * T34 will replace the stub with a real implementation. */
UTEST(barrier_observer_bit_calls_stub)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    UCell *parent = urbi_gc_alloc(&vm, sizeof(UCell) + 32U, UTYPE_OBJECT);
    UCell *child  = urbi_gc_alloc(&vm, sizeof(UCell) + 32U, UTYPE_OBJECT);

    /* Mark parent as having a watcher observer and force it BLACK. */
    parent->gc_byte |= UGC_HAS_WATCHER_OBSERVER;
    parent->gc_byte  = (uint8_t)((parent->gc_byte & ~UGC_COLOR_MASK) | UGC_COLOR_BLACK);
    child->gc_byte   = (uint8_t)((child->gc_byte  & ~UGC_COLOR_MASK) | vm.current_white);

    uint32_t dirty_before = vm.watchers->dirty_count;

    urbi_gc_slot_pre_store(&vm, parent, 42U, uvalue_from_test_cell(child));

    /* T25: stub does nothing — watcher_dirty_count unchanged.
     * T34: UASSERT_EQ(vm.watchers->dirty_count, dirty_before + 1U); */
    (void)dirty_before;

    /* GC barrier still fires: child was white + parent was black → child gray. */
    UASSERT(IS_GRAY(child));

    urbi_vm_destroy(&vm);
}

/* ===== Test 6: upvalue_write black parent + white child → child shaded ===== */

/* urbi_gc_upvalue_pre_store applies the same forward Dijkstra barrier as
 * slot_write.  Task 9c: the barrier parent is the UUpvalCell's UCell
 * header (the cell is shared between sibling closures, so a closure's
 * color is the wrong check); a synthetic UCell stands in for it. */
UTEST(barrier_upvalue_black_stores_white_shades)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    /* parent_cell stands in for a heapified UUpvalCell's header (T25
     * synthetic; Task 9c cell-parent signature). */
    UCell *parent_cell = urbi_gc_alloc(&vm, sizeof(UCell) + 32U, UTYPE_OBJECT);
    UCell *child       = urbi_gc_alloc(&vm, sizeof(UCell) + 32U, UTYPE_OBJECT);

    parent_cell->gc_byte = (uint8_t)((parent_cell->gc_byte & ~UGC_COLOR_MASK) | UGC_COLOR_BLACK);
    child->gc_byte       = (uint8_t)((child->gc_byte       & ~UGC_COLOR_MASK) | vm.current_white);

    urbi_gc_upvalue_pre_store(&vm, parent_cell,
                          uvalue_from_test_cell(child));

    /* Child must be gray — forward barrier fired. */
    UASSERT(IS_GRAY(child));
    UASSERT(IS_BLACK(parent_cell));

    urbi_vm_destroy(&vm);
}

/* ===== Test 7: UClosure embeds UCell at offset 0 ===== */

/* M4 closes the M3 deferral: UClosure embeds UCell as its first member, so
 * the UClosure* → UCell* casts in uvalue_as_cell (UVAL_CLOSURE mark-path
 * values) and the walker/root shading sites (walk_uclosure,
 * strand_walk_roots) are well-defined.  Verifies the structural invariant
 * those casts rely on.  (Task 9c: urbi_gc_upvalue_pre_store no longer
 * performs this cast — its parent is the UUpvalCell header — but the
 * value-tagging casts above keep the pin load-bearing.) */
UTEST(barrier_upvalue_uclosure_embeds_ucell_at_offset_zero)
{
    /* The cast UClosure* → UCell* is well-defined only when UCell sits at
     * offset 0 inside UClosure.  The two-byte common header (type_tag,
     * gc_byte) must alias the first two bytes of the closure. */
    UASSERT_EQ(offsetof(UClosure, cell), (size_t)0);
    UASSERT_EQ(offsetof(UClosure, cell.type_tag), (size_t)0);
    UASSERT_EQ(offsetof(UClosure, cell.gc_byte),  (size_t)1);
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
    utest_run("barrier_upvalue_uclosure_embeds_ucell_at_offset_zero",
              barrier_upvalue_uclosure_embeds_ucell_at_offset_zero);
}
