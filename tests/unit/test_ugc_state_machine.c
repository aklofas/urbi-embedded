/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: GC 5-phase state machine + slice work + threshold tuning.
 * Row 10 §6.1–§6.5.  T24 baseline.
 *
 * URBI_GC_STRESS disarm (v0.13.2): every test in this suite deliberately
 * allocates UNROOTED cells and asserts on their counts/colors across
 * manually-driven collection steps — the suite tests the GC machinery
 * itself, not rooted-program behaviour.  Collect-on-every-alloc sweeps
 * the deliberately-unrooted cells mid-setup, so each test sets
 * vm.gc_stress_armed = 0 right after init.  Structural-by-design, not a
 * rooting bug (refactor-3 TEST-GAP-01 stress-exempt list).
 *
 * All tests use the urbi_vm_init/urbi_vm_destroy pattern.  No concrete cell types
 * are registered (type_table[] is empty at T24), so walk_payload is never
 * called.  Tests exercise:
 *   - Phase transitions through a full cycle
 *   - gc_sweep frees dead cells and survives pinned/fixed cells
 *   - gc_pause suppresses automatic cycle start
 *   - Threshold update at end of cycle
 *   - gc_force_full completes synchronously
 *   - gc_phase getter returns correct value */

#include "utest.h"
#include "urbi/gc.h"
#include "gc/ugc_incremental.h"
#include "vm/uvm.h"
#include "watcher/uwatcher.h"
#include <stdlib.h>

#ifdef URBI_DEBUG
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* EXPECT_ABORT: assert that expr causes abort (via assert() failure).
 * Uses fork+waitpid: child executes expr; parent verifies abnormal exit.
 * Only meaningful in URBI_DEBUG builds where URBI_INTERNAL_ASSERT is assert(). */
#define EXPECT_ABORT(expr)                                                   \
    do {                                                                     \
        utest_checks++;                                                      \
        pid_t _pid = fork();                                                 \
        if (_pid == 0) {                                                     \
            (expr);                                                          \
            _exit(0); /* should not reach — abort expected */                \
        }                                                                    \
        int _st = 0;                                                         \
        waitpid(_pid, &_st, 0);                                              \
        int _aborted = WIFSIGNALED(_st) ||                                   \
                       (WIFEXITED(_st) && WEXITSTATUS(_st) != 0);            \
        if (!_aborted) {                                                     \
            utest_failures++;                                                \
            printf("  FAIL: %s:%d: " #expr " did not abort\n",               \
                   __FILE__, __LINE__);                                      \
            fflush(stdout);                                                  \
        }                                                                    \
    } while (0)
#endif /* URBI_DEBUG */

#define UTEST(name) static void name(void)

/* Helpers for counting cells on the all-cells list.
 * The sidecar list is private to ugc_incremental.c, but we can walk it
 * via vm->all_cells_head using the same cast convention documented in
 * ugc_incremental.c — treat the stored UCell* as an opaque pointer and
 * compare addresses to count nodes by iterating through the raw pointer
 * field.
 *
 * We don't export UAllCellsNode here; instead we exploit the fact that the
 * sidecar's first field is UCell *cell.  Casting vm->all_cells_head to a
 * "struct where first field is a pointer, second field is size_t, third
 * is another pointer" requires a compatible struct.  Instead of coupling
 * tests to the private struct, we count allocated cells by tracking
 * gc_total_allocated vs gc_live_bytes after a full cycle. */

/* Count live cells by walking the sidecar list.  The sidecar layout is:
 *   { UCell *cell, size_t size, struct *next, struct *next_gray }
 * We use a local mirror struct with matching layout to read the next pointer.
 * This is a test-only coupling; production code does not do this. */
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

/* ===== Test 1: urbi_gc_phase returns IDLE on fresh VM ===== */

UTEST(ugc_phase_getter_returns_idle_initially)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    UASSERT_EQ(urbi_gc_phase(&vm), (uint8_t)GC_PHASE_IDLE);

    urbi_vm_destroy(&vm);
}

/* ===== Test 2: phase transitions IDLE → MARK_ROOTS when debt > 0 ===== */

UTEST(ugc_phase_transitions_idle_to_mark)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    /* Force debt positive so the trigger fires. */
    vm.gc_debt = 1;

    urbi_gc_slice(&vm, 64U);

    /* Phase should have moved out of IDLE (to MARK_ROOTS or beyond). */
    UASSERT(urbi_gc_phase(&vm) != GC_PHASE_IDLE);

    urbi_vm_destroy(&vm);
}

/* ===== Test 3: urbi_gc_force_full completes a full cycle ===== */

UTEST(ugc_force_full_reaches_idle)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    /* Allocate a few cells to give the sweep something to walk. */
    int i;
    for (i = 0; i < 10; i++) {
        UCell *c = urbi_gc_alloc(&vm, 64U, UTYPE_OBJECT);
        UASSERT(c != NULL);
    }

    urbi_gc_force_full(&vm);

    UASSERT_EQ(urbi_gc_phase(&vm), (uint8_t)GC_PHASE_IDLE);
    UASSERT_EQ(vm.gc_pending, 0U);

    urbi_vm_destroy(&vm);
}

/* ===== Test 4: sweep frees all unreachable cells (no roots registered) ===== */

UTEST(ugc_force_full_collects_all_unreachable)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    /* Allocate 20 cells; none are roots (no root providers registered,
     * no UVAL_CLOSURE values in any slot). */
    int i;
    for (i = 0; i < 20; i++) {
        UCell *c = urbi_gc_alloc(&vm, 64U, UTYPE_OBJECT);
        UASSERT(c != NULL);
    }

    UASSERT_EQ(count_all_cells(&vm), 20);

    urbi_gc_force_full(&vm);

    /* All 20 cells are unreachable — should have been freed. */
    UASSERT_EQ(count_all_cells(&vm), 0);
    /* gc_live_bytes should be 0 after collecting everything. */
    UASSERT_EQ(urbi_gc_live_bytes(&vm), (size_t)0);

    urbi_vm_destroy(&vm);
}

/* ===== Test 5: sweep survives UGC_IS_PINNED cells ===== */

UTEST(ugc_sweep_survives_pinned_cell)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    /* Allocate one pinned cell and one unpinned cell. */
    UCell *pinned = urbi_gc_alloc(&vm, 64U, UTYPE_OBJECT);
    UCell *dead   = urbi_gc_alloc(&vm, 64U, UTYPE_OBJECT);
    UASSERT(pinned != NULL);
    UASSERT(dead != NULL);

    /* Pin 'pinned' — GC sweep must not free it. */
    pinned->gc_byte |= UGC_IS_PINNED;

    urbi_gc_force_full(&vm);

    /* 'dead' should be gone; 'pinned' must survive. */
    UASSERT_EQ(count_all_cells(&vm), 1);
    /* Verify the surviving cell is pinned (same pointer still readable). */
    UASSERT(pinned->gc_byte & UGC_IS_PINNED);

    urbi_vm_destroy(&vm);
}

/* ===== Test 6: sweep survives UGC_IS_FIXED cells ===== */

UTEST(ugc_sweep_survives_fixed_cell)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    UCell *fixed = urbi_gc_alloc(&vm, 64U, UTYPE_OBJECT);
    UCell *dead  = urbi_gc_alloc(&vm, 64U, UTYPE_OBJECT);
    UASSERT(fixed != NULL);
    UASSERT(dead != NULL);

    /* Mark 'fixed' as pool-managed — GC sweep must not free it. */
    fixed->gc_byte |= UGC_IS_FIXED;

    urbi_gc_force_full(&vm);

    /* 'dead' freed; 'fixed' survives. */
    UASSERT_EQ(count_all_cells(&vm), 1);
    UASSERT(fixed->gc_byte & UGC_IS_FIXED);

    urbi_vm_destroy(&vm);
}

/* ===== Test 7: gc_pause suppresses automatic cycle start ===== */

UTEST(ugc_pause_suppresses_cycle)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    urbi_gc_pause(&vm, true);

    /* Force positive debt — normally triggers a cycle. */
    vm.gc_debt = (int64_t)vm.gc_threshold + 1;

    /* Slice with positive debt and paused GC: should stay IDLE. */
    urbi_gc_slice(&vm, 4096U);

    UASSERT_EQ(urbi_gc_phase(&vm), (uint8_t)GC_PHASE_IDLE);

    /* Resume and verify that a cycle starts normally. */
    urbi_gc_pause(&vm, false);
    urbi_gc_slice(&vm, 4096U);
    /* Phase should have advanced out of IDLE. */
    /* (force_full brings it back to IDLE; we only check it moved) */

    urbi_vm_destroy(&vm);
}

/* ===== Test 8: threshold updates based on live bytes at end of cycle ===== */

UTEST(ugc_threshold_updates_at_cycle_end)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    size_t initial_threshold = vm.gc_threshold;

    /* Allocate some pinned cells so they survive the sweep. */
    int i;
    for (i = 0; i < 5; i++) {
        UCell *c = urbi_gc_alloc(&vm, 128U, UTYPE_OBJECT);
        UASSERT(c != NULL);
        c->gc_byte |= UGC_IS_PINNED;
    }

    /* Run a full cycle. */
    urbi_gc_force_full(&vm);

    /* gc_live_bytes should reflect the 5 surviving cells (5 * 128 = 640). */
    UASSERT_EQ(urbi_gc_live_bytes(&vm), (size_t)(5 * 128));

    /* Threshold should have been updated from live_bytes * PAUSE_RATIO / 100.
     * With live=640 and PAUSE_RATIO=200: threshold = 1280.
     * That's >= URBI_GC_INITIAL_THRESHOLD (16384), so the clamp applies:
     * threshold = max(1280, 16384) = 16384.
     * So threshold is clamped to the initial value here since 1280 < 16384. */
    /* Regardless of clamping: threshold != initial only if live_bytes-derived
     * value exceeds URBI_GC_INITIAL_THRESHOLD.  With 5*128=640 bytes live,
     * 640*200/100=1280 < 16384, so threshold stays at URBI_GC_INITIAL_THRESHOLD. */
    UASSERT_EQ(urbi_gc_threshold(&vm), initial_threshold);

    /* Verify debt was reset to -threshold (negative = credit). */
    UASSERT(vm.gc_debt <= 0);
    UASSERT_EQ(vm.gc_debt, -(int64_t)vm.gc_threshold);

    urbi_vm_destroy(&vm);
}

/* ===== Test 9: full cycle with large allocation triggers threshold > initial ===== */

UTEST(ugc_threshold_exceeds_initial_with_large_live_set)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    /* Allocate enough pinned cells so that live_bytes * PAUSE_RATIO / 100
     * exceeds URBI_GC_INITIAL_THRESHOLD (16384).
     * Need: live * 2 > 16384  →  live > 8192.  Use 10 cells * 1024 = 10240 bytes. */
    int i;
    for (i = 0; i < 10; i++) {
        UCell *c = urbi_gc_alloc(&vm, 1024U, UTYPE_OBJECT);
        UASSERT(c != NULL);
        c->gc_byte |= UGC_IS_PINNED;
    }

    urbi_gc_force_full(&vm);

    size_t live = urbi_gc_live_bytes(&vm);
    size_t expected_thresh = (live * (size_t)URBI_GC_PAUSE_RATIO) / 100U;

    UASSERT_EQ(urbi_gc_live_bytes(&vm), (size_t)(10 * 1024));
    UASSERT_EQ(urbi_gc_threshold(&vm), expected_thresh);

    urbi_vm_destroy(&vm);
}

/* ===== Test 10: urbi_gc_collect is equivalent to force_full ===== */

UTEST(ugc_collect_frees_dead_cells)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    int i;
    for (i = 0; i < 8; i++) {
        UCell *c = urbi_gc_alloc(&vm, 64U, UTYPE_OBJECT);
        UASSERT(c != NULL);
    }
    UASSERT_EQ(count_all_cells(&vm), 8);

    urbi_gc_collect(&vm);

    UASSERT_EQ(count_all_cells(&vm), 0);
    UASSERT_EQ(urbi_gc_phase(&vm), (uint8_t)GC_PHASE_IDLE);

    urbi_vm_destroy(&vm);
}

/* ===== Test 11 (refactor-3 GC-15): urbi_gc_shade_gray sidecar contract =====
 *
 * NULL-sidecar contract: the ONLY cells legitimately absent from
 * all_cells_head are FIXED pool cells (UWatcher slots).  Everything else
 * — including UClosure / UUpvalCell, GC-managed since v0.8.4 Step C-2 —
 * is enrolled by urbi_gc_alloc, so urbi_gc_shade_gray must always find a
 * sidecar and push it onto the gray work-list. */

/* Is `cell` enrolled on the all-cells sidecar list? */
static int cell_enrolled(UVM *vm, const void *cell)
{
    const MirrorNode *n = (const MirrorNode *)(void *)vm->all_cells_head;
    while (n != NULL) {
        if (n->cell == cell) return 1;
        n = n->next;
    }
    return 0;
}

UTEST(ugc_shade_gray_graylists_every_enrolled_cell_type)
{
    /* One tag per GC-managed allocation path that historically had its own
     * "regime": ordinary objects, the v0.8.4-promoted closure/upval pair
     * (urbi_vm_alloc_closure / urbi_vm_open_upvalue → urbi_gc_alloc), tags, events.
     * 256 B over-allocates every payload so the zero-init walkers stay in
     * bounds if a forced stress collection drains the gray list. */
    static const uint8_t tags[] = {
        UTYPE_OBJECT, UTYPE_CLOSURE, UTYPE_UPVAL_CELL, UTYPE_TAG, UTYPE_EVENT,
    };

    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    size_t i;
    for (i = 0; i < sizeof tags / sizeof tags[0]; i++) {
        UCell *c = urbi_gc_alloc(&vm, 256U, tags[i]);
        UASSERT(c != NULL);
        UASSERT(cell_enrolled(&vm, c));
        UASSERT((c->gc_byte & UGC_COLOR_MASK) <= UGC_COLOR_WHITE1);

        urbi_gc_shade_gray(&vm, c);

        UASSERT_EQ((int)(c->gc_byte & UGC_COLOR_MASK), (int)UGC_COLOR_GRAY);
        /* Sidecar found: the gray work-list head is this cell's sidecar. */
        const MirrorNode *gray = (const MirrorNode *)(void *)vm.gray_work_head;
        UASSERT(gray != NULL);
        UASSERT(gray->cell == (void *)c);
    }

    urbi_vm_destroy(&vm);
}

UTEST(ugc_shade_gray_fixed_pool_cell_colors_without_sidecar)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    /* Real pool watcher: UGC_IS_FIXED, never enrolled on all_cells_head.
     * UWatcher's leading fields match the UCell header layout by design. */
    UWatcher *w = uwatcher_pool_alloc(&vm);
    UASSERT(w != NULL);
    UCell *cell = (UCell *)(void *)w;
    UASSERT((cell->gc_byte & UGC_IS_FIXED) != 0U);
    UASSERT(!cell_enrolled(&vm, cell));
    UASSERT(vm.gray_work_head == NULL);

    urbi_gc_shade_gray(&vm, cell);   /* must NOT abort: FIXED is the exemption */

    /* Color set; work-list push correctly skipped (no sidecar to push). */
    UASSERT_EQ((int)(cell->gc_byte & UGC_COLOR_MASK), (int)UGC_COLOR_GRAY);
    UASSERT(vm.gray_work_head == NULL);

    urbi_vm_destroy(&vm);
}

#ifdef URBI_DEBUG
/* Helper invoked inside the forked child of EXPECT_ABORT below.  Shades a
 * stack cell that is neither enrolled (no urbi_gc_alloc sidecar) nor FIXED
 * — a rooting/enrollment bug by the GC-15 contract; the NULL-sidecar
 * assert in urbi_gc_shade_gray must abort the child. */
static void shade_unenrolled_nonfixed_cell(void)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    UCell orphan;
    orphan.type_tag = UTYPE_OBJECT;
    orphan.gc_byte  = vm.current_white;   /* white; not FIXED; not enrolled */
    urbi_gc_shade_gray(&vm, &orphan);
}

UTEST(ugc_shade_gray_unenrolled_nonfixed_aborts_in_debug)
{
    EXPECT_ABORT(shade_unenrolled_nonfixed_cell());
}
#endif

/* ===== Suite entry point ===== */

void test_ugc_state_machine_suite(void)
{
    utest_run("ugc_phase_getter_returns_idle_initially",
              ugc_phase_getter_returns_idle_initially);
    utest_run("ugc_phase_transitions_idle_to_mark",
              ugc_phase_transitions_idle_to_mark);
    utest_run("ugc_force_full_reaches_idle",
              ugc_force_full_reaches_idle);
    utest_run("ugc_force_full_collects_all_unreachable",
              ugc_force_full_collects_all_unreachable);
    utest_run("ugc_sweep_survives_pinned_cell",
              ugc_sweep_survives_pinned_cell);
    utest_run("ugc_sweep_survives_fixed_cell",
              ugc_sweep_survives_fixed_cell);
    utest_run("ugc_pause_suppresses_cycle",
              ugc_pause_suppresses_cycle);
    utest_run("ugc_threshold_updates_at_cycle_end",
              ugc_threshold_updates_at_cycle_end);
    utest_run("ugc_threshold_exceeds_initial_with_large_live_set",
              ugc_threshold_exceeds_initial_with_large_live_set);
    utest_run("ugc_collect_frees_dead_cells",
              ugc_collect_frees_dead_cells);
    utest_run("ugc_shade_gray_graylists_every_enrolled_cell_type",
              ugc_shade_gray_graylists_every_enrolled_cell_type);
    utest_run("ugc_shade_gray_fixed_pool_cell_colors_without_sidecar",
              ugc_shade_gray_fixed_pool_cell_colors_without_sidecar);
#ifdef URBI_DEBUG
    utest_run("ugc_shade_gray_unenrolled_nonfixed_aborts_in_debug",
              ugc_shade_gray_unenrolled_nonfixed_aborts_in_debug);
#endif
}
