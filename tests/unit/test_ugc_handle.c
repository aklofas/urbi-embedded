/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: host-handle table (urbi_handle_create/get/release),
 * host_handle_walk_roots, urbi_pin/unpin, and UGC_IS_FIXED survival.
 * Row 10 §5.6 + §8.  T27. */

#include "utest.h"
#include "urbi/gc.h"
#include "gc/ugc_incremental.h"
#include "runtime/uhandle.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* === Test helper: build a UValue tagged UVAL_CLOSURE pointing to c ===
 * Reuses the synthetic pattern from T25/T26 barrier tests. */
static UValue
make_handle_cell_value(UCell *c)
{
    UValue v = {0};
    v.kind = UVAL_CLOSURE;
    v.v.p = (void *)c;
    return v;
}

/* === Test helper: callback for handle_walk_roots_active_only ===
 * Counts non-NIL slots visited by host_handle_walk_roots. */
static void
handle_walk_count_cb(struct UVM *vm, UValue *slot, void *ctx)
{
    (void)vm; (void)slot;
    int *count = (int *)ctx;
    (*count)++;
}

/* ===== Handle table tests ===== */

/* Basic create → get → release round-trip. */
UTEST(handle_create_get_release)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UCell *c = urbi_gc_alloc(&vm, sizeof(UCell) + 16U, UTYPE_OBJECT);
    UASSERT(c != NULL);

    UValue v = make_handle_cell_value(c);

    UHandle h = urbi_handle_create(&vm, v);
    UASSERT(h != URBI_HANDLE_INVALID);

    UValue got = urbi_handle_get(&vm, h);
    UASSERT_EQ(got.kind, UVAL_CLOSURE);
    UASSERT(got.v.p == (void *)c);

    urbi_handle_release(&vm, h);
    UValue after = urbi_handle_get(&vm, h);
    UASSERT_EQ(after.kind, UVAL_NIL);

    urbi_vm_destroy(&vm);
}

/* URBI_HANDLE_INVALID (0) and out-of-range get return nil. */
UTEST(handle_invalid_returns_nil)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue v1 = urbi_handle_get(&vm, URBI_HANDLE_INVALID);
    UASSERT_EQ(v1.kind, UVAL_NIL);

    UValue v2 = urbi_handle_get(&vm, 9999U);
    UASSERT_EQ(v2.kind, UVAL_NIL);

    /* Release of invalid handle is a no-op (no crash). */
    urbi_handle_release(&vm, URBI_HANDLE_INVALID);
    urbi_handle_release(&vm, 9999U);

    urbi_vm_destroy(&vm);
}

/* Allocate more handles than INITIAL_CAP (16) to exercise table growth. */
UTEST(handle_grow_beyond_initial_cap)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Allocate 20 handles to force at least one growth. */
#define N_HANDLES 20
    UHandle handles[N_HANDLES];
    UCell   *cells[N_HANDLES];
    int      i;

    for (i = 0; i < N_HANDLES; i++) {
        cells[i] = urbi_gc_alloc(&vm, sizeof(UCell) + 8U, UTYPE_OBJECT);
        UASSERT(cells[i] != NULL);
        UValue v = make_handle_cell_value(cells[i]);
        handles[i] = urbi_handle_create(&vm, v);
        UASSERT(handles[i] != URBI_HANDLE_INVALID);
    }

    /* Verify all handles still resolve correctly. */
    for (i = 0; i < N_HANDLES; i++) {
        UValue got = urbi_handle_get(&vm, handles[i]);
        UASSERT_EQ(got.kind, UVAL_CLOSURE);
        UASSERT(got.v.p == (void *)cells[i]);
    }

    /* Handles are 1-indexed and must be distinct. */
    for (i = 0; i < N_HANDLES; i++) {
        UASSERT_EQ(handles[i], (UHandle)(i + 1));
    }
#undef N_HANDLES

    urbi_vm_destroy(&vm);
}

/* host_handle_walk_roots calls back for active (non-nil) slots only. */
UTEST(handle_walk_roots_active_only)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UCell *c1 = urbi_gc_alloc(&vm, sizeof(UCell) + 8U, UTYPE_OBJECT);
    UCell *c2 = urbi_gc_alloc(&vm, sizeof(UCell) + 8U, UTYPE_OBJECT);
    UASSERT(c1 != NULL);
    UASSERT(c2 != NULL);

    UHandle h1 = urbi_handle_create(&vm, make_handle_cell_value(c1));
    UHandle h2 = urbi_handle_create(&vm, make_handle_cell_value(c2));
    UASSERT(h1 != URBI_HANDLE_INVALID);
    UASSERT(h2 != URBI_HANDLE_INVALID);

    /* Release h1; only h2 should be walked. */
    urbi_handle_release(&vm, h1);

    /* Count callbacks via a small static counter. */
    static int walk_count;
    walk_count = 0;

    /* Call host_handle_walk_roots with the file-scope callback. */
    host_handle_walk_roots(&vm, handle_walk_count_cb, &walk_count);

    /* After creating h1 (released) and h2 (active): 1 active slot visited. */
    UASSERT_EQ(walk_count, 1);

    urbi_vm_destroy(&vm);
}

/* ===== pin/unpin tests ===== */

/* urbi_pin sets UGC_IS_PINNED; urbi_unpin clears it. */
UTEST(pin_sets_bit_unpin_clears)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UCell *c = urbi_gc_alloc(&vm, sizeof(UCell) + 16U, UTYPE_OBJECT);
    UASSERT(c != NULL);

    UValue v = make_handle_cell_value(c);

    /* Initially not pinned. */
    UASSERT(!(c->gc_byte & UGC_IS_PINNED));

    urbi_pin(&vm, v);
    UASSERT(c->gc_byte & UGC_IS_PINNED);

    urbi_unpin(&vm, v);
    UASSERT(!(c->gc_byte & UGC_IS_PINNED));

    urbi_vm_destroy(&vm);
}

/* A pinned cell must survive urbi_gc_force_full (not freed). */
UTEST(pin_skips_sweep)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UCell *c = urbi_gc_alloc(&vm, sizeof(UCell) + 16U, UTYPE_OBJECT);
    UASSERT(c != NULL);

    UValue v = make_handle_cell_value(c);
    urbi_pin(&vm, v);
    UASSERT(c->gc_byte & UGC_IS_PINNED);

    /* Force a full GC cycle — cell is unreachable via roots but pinned. */
    urbi_gc_force_full(&vm);

    /* Cell must still have the pinned flag set (not freed/zeroed). */
    UASSERT(c->gc_byte & UGC_IS_PINNED);

    /* Clean up: unpin so the next GC cycle can collect it. */
    urbi_unpin(&vm, v);

    urbi_vm_destroy(&vm);
}

/* pin/unpin are no-ops for non-heap UValues (no crash). */
UTEST(pin_unpin_nop_for_non_heap)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue nil_val = {0};
    nil_val.kind = UVAL_NIL;
    urbi_pin(&vm, nil_val);   /* no crash */
    urbi_unpin(&vm, nil_val); /* no crash */

    UValue int_val = {0};
    int_val.kind = UVAL_INT;
    int_val.v.i = 42;
    urbi_pin(&vm, int_val);
    urbi_unpin(&vm, int_val);

    urbi_vm_destroy(&vm);
}

/* ===== UGC_IS_FIXED survival ===== */

/* A cell with UGC_IS_FIXED must survive a full GC sweep (pool-managed semantics). */
UTEST(fixed_cell_survives_sweep)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UCell *c = urbi_gc_alloc(&vm, sizeof(UCell) + 16U, UTYPE_OBJECT);
    UASSERT(c != NULL);

    c->gc_byte |= UGC_IS_FIXED;

    /* Force full GC — cell is not reachable via roots but is FIXED. */
    urbi_gc_force_full(&vm);

    /* FIXED cells must survive: verify the flag is still set. */
    UASSERT(c->gc_byte & UGC_IS_FIXED);

    urbi_vm_destroy(&vm);
}

/* GC-007: a FIXED cell's color tracks vm->current_white after every sweep.
 *
 * The sweep loop in ugc_incremental.c paints FIXED cells with current_white
 * regardless of whether the mark phase reached them.  This is REQUIRED, not
 * redundant: the FIXED cell may not have a heap root walking to it (e.g. a
 * UWatcher reached only through watcher_table_walk_roots, which traverses
 * a side list rather than the all-cells sidecar).  Without the re-paint,
 * the cell would carry a stale OTHER_WHITE color into the next mark and
 * appear "already marked" — breaking the tri-color invariant.
 *
 * The test forces two consecutive full GC cycles (which flips current_white
 * each cycle) and asserts that the FIXED cell's color matches the
 * post-sweep current_white at every cycle boundary. */
UTEST(fixed_cell_color_tracks_current_white_after_sweep)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UCell *c = urbi_gc_alloc(&vm, sizeof(UCell) + 16U, UTYPE_OBJECT);
    UASSERT(c != NULL);
    c->gc_byte |= UGC_IS_FIXED;

    /* Cycle 1. */
    urbi_gc_force_full(&vm);
    UASSERT(c->gc_byte & UGC_IS_FIXED);
    UASSERT_EQ((int)(c->gc_byte & UGC_COLOR_MASK), (int)vm.current_white);

    /* Cycle 2: current_white flips; the sweep must re-paint to the new value. */
    urbi_gc_force_full(&vm);
    UASSERT(c->gc_byte & UGC_IS_FIXED);
    UASSERT_EQ((int)(c->gc_byte & UGC_COLOR_MASK), (int)vm.current_white);

    /* Cycle 3 for good measure — the contract holds across many cycles. */
    urbi_gc_force_full(&vm);
    UASSERT(c->gc_byte & UGC_IS_FIXED);
    UASSERT_EQ((int)(c->gc_byte & UGC_COLOR_MASK), (int)vm.current_white);

    urbi_vm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void test_ugc_handle_suite(void)
{
    utest_run("handle_create_get_release",
              handle_create_get_release);
    utest_run("handle_invalid_returns_nil",
              handle_invalid_returns_nil);
    utest_run("handle_grow_beyond_initial_cap",
              handle_grow_beyond_initial_cap);
    utest_run("handle_walk_roots_active_only",
              handle_walk_roots_active_only);
    utest_run("pin_sets_bit_unpin_clears",
              pin_sets_bit_unpin_clears);
    utest_run("pin_skips_sweep",
              pin_skips_sweep);
    utest_run("pin_unpin_nop_for_non_heap",
              pin_unpin_nop_for_non_heap);
    utest_run("fixed_cell_survives_sweep",
              fixed_cell_survives_sweep);
    utest_run("fixed_cell_color_tracks_current_white_after_sweep",
              fixed_cell_color_tracks_current_white_after_sweep);
}
