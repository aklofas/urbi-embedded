/* SPDX-License-Identifier: BSD-3-Clause */
/* Phase 7 regression tests for v0.5.7-fixes:
 *   T36 / GC-009 — gc_shade_gray's silent NULL-sidecar return is now
 *                  documented (DOCUMENT-only resolution): three legitimate
 *                  cell-allocation regimes coexist at v0.5.x and only one
 *                  enrolls a sidecar.  Tests pin both expected paths.
 *   T37 / GC-015 — sweep gc_surviving_bytes accumulator must not include
 *                  cells allocated mid-cycle (between sweep slices).
 *
 * Both tests exercise the GC state machine directly via urbi_gc_slice and
 * urbi_gc_force_full; they don't go through the VM dispatch loop.  The same
 * sidecar mirror trick used in test_ugc_state_machine.c is used here to
 * count cells on vm->all_cells_head without coupling to the private
 * UAllCellsNode struct. */

#include "utest.h"
#include "urbi/gc.h"
#include "gc/ugc_incremental.h"
#include "vm/uvm.h"
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* Mirror of UAllCellsNode for sidecar-list counting (matches the private
 * layout in src/gc/ugc_incremental.c — cell, size, next, next_gray). */
typedef struct MirrorNode {
    void   *cell;
    size_t  size;
    struct MirrorNode *next;
    struct MirrorNode *next_gray;
} MirrorNode;

static int count_all_cells(UVM *vm) {
    MirrorNode *n = (MirrorNode *)(void *)vm->all_cells_head;
    int count = 0;
    while (n != NULL) {
        count++;
        n = n->next;
    }
    return count;
}

/* ===================================================================
 * T36 / GC-009 → refactor-3 GC-15 — gc_shade_gray NULL-sidecar contract
 *
 * Test 1 (tracked cell): gc_shade_gray on an urbi_gc_alloc'd cell pushes
 *   it onto the gray work-list normally.  Positive regression.
 *
 * Test 2 (FIXED pool cell): gc_shade_gray on a hand-built FIXED cell (no
 *   sidecar in vm->all_cells_head) silently no-ops the work-list push
 *   without aborting or crashing.  Mirrors the UWatcher pool slots —
 *   the ONLY cells legitimately absent from all_cells_head.
 *
 * The former Test 3 ("regime 3" — a non-FIXED non-enrolled UClosure-like
 * cell silently no-ops) was retired by refactor-3 GC-15: UClosure /
 * UUpvalCell are enrolled via urbi_gc_alloc since v0.8.4 Step C-2, so a
 * NULL sidecar on a non-FIXED cell is an enrollment bug and gc_shade_gray
 * now asserts on it.  The inverted pin (debug-build abort) lives in
 * test_ugc_state_machine.c.
 * =================================================================== */

UTEST(gc_shade_gray_walks_alloced_cell)
{
    /* Positive regression: normal alloc + shade + force_full path. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UCell *c = urbi_gc_alloc(&vm, 64U, UTYPE_OBJECT);
    UASSERT(c != NULL);

    /* Pin so it survives sweep — exercises the sidecar lookup + work-list
     * push path inside gc_shade_gray indirectly via the cycle. */
    c->gc_byte |= UGC_IS_PINNED;

    urbi_gc_force_full(&vm);

    /* Cell survived (pinned), and we finished a clean cycle. */
    UASSERT_EQ(count_all_cells(&vm), 1);
    UASSERT_EQ(urbi_gc_phase(&vm), (uint8_t)GC_PHASE_IDLE);

    urbi_vm_destroy(&vm);
}

UTEST(gc_shade_gray_silent_on_fixed_cell_without_sidecar)
{
    /* Build a FIXED cell on the stack — no sidecar, never inserted into
     * vm->all_cells_head.  Mirrors the UWatcher pool-slot pattern (the
     * pool slab is alloc_fn-managed; slots carry UGC_IS_FIXED). */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UCell standalone;
    standalone.type_tag = UTYPE_OBJECT;
    standalone.gc_byte  = (uint8_t)(vm.current_white | UGC_IS_FIXED);

    /* Per the GC-15 contract documented in gc_shade_gray, the NULL-return
     * from find_sidecar_for_cell is an expected path for FIXED cells: the
     * function silently no-ops the work-list push and just leaves the
     * color flag set as an idempotency marker. */
    gc_shade_gray(&vm, &standalone);

    /* Color advanced to GRAY (idempotency marker for re-entry). */
    UASSERT_EQ((standalone.gc_byte & UGC_COLOR_MASK), (uint8_t)UGC_COLOR_GRAY);

    /* All-cells list never had this cell — count stays 0. */
    UASSERT_EQ(count_all_cells(&vm), 0);

    urbi_vm_destroy(&vm);
}

/* gc_shade_gray_silent_on_uclosure_regime_cell retired (refactor-3 GC-15):
 * the "regime 3" silent no-op it pinned is now an assert-failure path —
 * see the banner above and test_ugc_state_machine.c. */

/* ===================================================================
 * T37 / GC-015 — sweep surviving-bytes excludes intra-slice allocations
 *
 * Setup: pin five 64-byte cells (320 B survivors), then drive the GC
 * state machine through MARK with a large budget, then run a *single
 * tiny-budget sweep slice* so the sweep is deliberately incomplete.
 * Allocate one new 64-byte cell mid-cycle — it prepends to all_cells_head
 * with current_white color and IS NOT visited by the sweep cursor walk.
 * Complete the sweep with force_full.
 *
 * Assertion: gc_live_bytes == 320 (the five pinned cells).  The mid-cycle
 * 64-byte allocation must NOT be counted.  Before T37 the head→cursor
 * re-walk at the start of the next slice would have summed it in.
 * =================================================================== */

UTEST(sweep_surviving_bytes_excludes_intra_slice_allocations)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Allocate five 64 B cells; pin all so the sweep keeps them. */
    UCell *cells[5];
    for (int i = 0; i < 5; i++) {
        cells[i] = urbi_gc_alloc(&vm, 64U, UTYPE_OBJECT);
        UASSERT(cells[i] != NULL);
        cells[i]->gc_byte |= UGC_IS_PINNED;
    }

    UASSERT_EQ(count_all_cells(&vm), 5);

    /* Drive MARK to completion + transition into SWEEP.  We do this by
     * forcing positive debt + slicing with a generous budget that handles
     * MARK_ROOTS, MARK_INCREMENTAL (empty for these no-roots cells), and
     * ATOMIC_FINISH (which transitions to SWEEP). */
    vm.gc_debt = 1;
    /* Run slices until we land in SWEEP (or finish, which we don't expect
     * for 5 pinned cells with no atomic-finish drain to do).  Cap at a
     * few iterations to avoid infinite loop on regression. */
    for (int i = 0; i < 10 && vm.gc_phase != GC_PHASE_SWEEP; i++) {
        urbi_gc_slice(&vm, 64U);
    }
    UASSERT_EQ(urbi_gc_phase(&vm), (uint8_t)GC_PHASE_SWEEP);

    /* Run ONE tiny-budget sweep slice that processes some-but-not-all
     * cells.  budget=64 lets exactly one 64-B cell complete the slice
     * (consumed >= budget after one iteration). */
    urbi_gc_slice(&vm, 1U);
    /* Sweep should still be in progress (cursor mid-list). */
    UASSERT_EQ(urbi_gc_phase(&vm), (uint8_t)GC_PHASE_SWEEP);

    /* Mid-cycle allocation: prepends to all_cells_head; current_white;
     * no sweep-cursor visit because cursor is past the head. */
    UCell *intra = urbi_gc_alloc(&vm, 64U, UTYPE_OBJECT);
    UASSERT(intra != NULL);
    /* Pin it so it would survive even if the cursor did visit it — we
     * want the bug signal (over-counting) to come purely from the
     * accounting path, not from a free.  In any case the cursor will
     * NEVER reach it because intra is now BEFORE the cursor in the list. */
    intra->gc_byte |= UGC_IS_PINNED;

    /* Now complete the cycle.  After this gc_phase == IDLE and
     * gc_live_bytes is set. */
    urbi_gc_force_full(&vm);
    UASSERT_EQ(urbi_gc_phase(&vm), (uint8_t)GC_PHASE_IDLE);

    /* The five originally-pinned cells = 320 B.  The intra cell is
     * pinned + alive but was never visited by sweep, so it doesn't
     * contribute to gc_surviving_bytes (the closed bug).
     *
     * Pre-T37 shape would have re-walked head→cursor and added intra's
     * 64 B → expected_pre_fix == 384.
     * Post-T37 shape: gc_live_bytes == 320. */
    UASSERT_EQ(urbi_gc_live_bytes(&vm), (size_t)(5 * 64));

    /* Sanity: all six cells are still on the all-cells list. */
    UASSERT_EQ(count_all_cells(&vm), 6);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T37 corollary — sweep_surviving_bytes is reset between cycles.
 *
 * Run two back-to-back force_full cycles; assert gc_live_bytes after the
 * second cycle is the same as after the first (no double-counting from
 * the persistent accumulator across cycles).
 * =================================================================== */

UTEST(sweep_surviving_bytes_resets_between_cycles)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UCell *a = urbi_gc_alloc(&vm, 128U, UTYPE_OBJECT);
    UASSERT(a != NULL);
    a->gc_byte |= UGC_IS_PINNED;

    urbi_gc_force_full(&vm);
    size_t live_after_1 = urbi_gc_live_bytes(&vm);
    UASSERT_EQ(live_after_1, (size_t)128);

    /* Second cycle on the same VM with the same single pinned cell. */
    urbi_gc_force_full(&vm);
    size_t live_after_2 = urbi_gc_live_bytes(&vm);
    UASSERT_EQ(live_after_2, (size_t)128);

    urbi_vm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void test_gc_sweep_accounting_suite(void)
{
    utest_run("gc_shade_gray_walks_alloced_cell",
              gc_shade_gray_walks_alloced_cell);
    utest_run("gc_shade_gray_silent_on_fixed_cell_without_sidecar",
              gc_shade_gray_silent_on_fixed_cell_without_sidecar);
    utest_run("sweep_surviving_bytes_excludes_intra_slice_allocations",
              sweep_surviving_bytes_excludes_intra_slice_allocations);
    utest_run("sweep_surviving_bytes_resets_between_cycles",
              sweep_surviving_bytes_resets_between_cycles);
}
