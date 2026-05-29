/* SPDX-License-Identifier: BSD-3-Clause */
/* Incremental GC strategy: cell allocation, two-white tri-color marking,
 * allocation-debt trigger.  Spec: row 10 §3–§6 of the pre-M3 GC design doc.
 * T24 lands the 5-phase state machine; T25 lands the real write barriers;
 * T26 lands the root-provider registry walk. */

/* === All-cells linkage design choice: Option B (sidecar nodes) ===
 *
 * Spec §3.4 wants the all-cells next-pointer at the end of each cell's
 * payload, at a type-specific offset given by cell_size(type_tag).  At T23,
 * no concrete cell types exist and no type_table entries are registered
 * (that lands at T27).  A per-type cell_size lookup is therefore impossible.
 *
 * Option B (chosen here): each urbi_gc_alloc call also allocates a small
 * UAllCellsNode sidecar { cell*, size, next* }.  The sidecar list is the
 * all-cells linked list for T23/T24/T25/T26.  UCell itself stays exactly
 * 2 bytes (uint8_t type_tag + uint8_t gc_byte) per spec §2.5 — no fields
 * are added.
 *
 * vm->all_cells_head stores the UAllCellsNode* head cast to UCell*.  All
 * code inside this file uses the gc_node_head() accessor to recover it.
 * T24/T25/T26 sweep code must use the same accessor.  vm->sweep_cursor and
 * vm->sweep_cursor_prev follow the same cast convention.
 *
 * T27 collapses the sidecar into the trailing-payload-pointer scheme from
 * spec §3.4 once concrete types with registered UType entries exist.
 * At that point vm->all_cells_head reverts to a plain UCell* list threaded
 * through the trailing pointer at offset (cell_size - sizeof(UCell*)).
 *
 * Option A (size stored inside UCell padding) was rejected: it would add a
 * uint16_t at offset 2 inside UCell, violating the 2-byte header invariant
 * from spec §2.5, and requiring a schema break when T27 lands.
 * Option C (hash table) is overkill for the M3 bootstrap. */

/* === Gray work-list design choice: Option A (cast pointer) ===
 *
 * vm->gray_work_head is typed struct UCell* in uvm.h (per T4).  At T24 the
 * gray work-list is implemented by threading UAllCellsNode* through each
 * sidecar's next_gray field.  We store the sidecar head in vm->gray_work_head
 * via a UCell* cast, and recover UAllCellsNode* via gc_gray_head() below.
 *
 * This is ugly but safe: we write and read through the same cast convention
 * within this TU.  No code outside ugc_incremental.c touches gray_work_head
 * directly at T24.
 *
 * T27: when the sidecar disappears and trailing payload pointers are used,
 * vm->gray_work_head will hold plain UCell* directly.  Every cast site is
 * annotated with a T27 comment.
 *
 * Option B (a parallel vm->gc_gray_head of type UAllCellsNode*) would avoid
 * the cast but adds a UVM field.  We prefer not to change uvm.h at T24. */

#include "ugc_incremental.h"
#include "urbi/gc.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"
#include "runtime/umacros.h"
#include "gc/ugc.h"
#include "runtime/umemdebug.h"
#if URBI_MEM_DEBUG
#include "sched/ustrand.h"   /* full UStrand def: owner capture reads cur_strand->pc */
#include "chunk/uchunk.h"    /* uinstr_op */
#endif
#include <stddef.h>
#include <stdint.h>

#if URBI_MEM_DEBUG && (URBI_GC != URBI_GC_INCREMENTAL)
#  error "URBI_MEM_DEBUG requires URBI_GC_INCREMENTAL (the all-cells sidecar lives here)"
#endif

/* No stdlib.h or string.h — freestanding-strict like every other src/c file.
 * Memory operations go through vm->alloc_fn.  Zero-init uses a byte loop. */

/* === Sidecar node (strategy-private; not exported) ===
 *
 * One node per allocated GC cell.  Provides:
 *   - the pointer to the cell (so destroy can free it)
 *   - the allocation size (so destroy can call alloc_fn(cell, 0, ud) correctly,
 *     and so T24 can track live-byte accounting during sweep)
 *   - next pointer for the all-cells linked list
 *
 * T27: collapse into trailing-payload-pointer per spec §3.4 once UType
 * cell_size lookups are available. */
typedef struct UAllCellsNode {
    UCell              *cell;
    size_t              size;        /* USER size (redzone excluded under URBI_MEM_DEBUG) */
    struct UAllCellsNode *next;
    struct UAllCellsNode *next_gray;  /* T24: gray work-list link; NULL when not on gray queue */
#if URBI_MEM_DEBUG
    uint64_t              seq;        /* v0.11.3 owner tag: monotonic alloc sequence */
    const uint32_t       *owner_pc;   /* vm->cur_strand->pc at alloc, or NULL */
    void                 *owner_ret;  /* __builtin_return_address(0): the C caller */
    uint16_t              owner_op;   /* decoded opcode at owner_pc, else 0xFFFF */
    uint16_t              strand_id;  /* cur_strand low-16, else 0 */
#endif
} UAllCellsNode;

/* === Sidecar accessor convention (GC-019) ===
 *
 * Each `gc_<role>` accessor below recovers the embedded UAllCellsNode
 * sidecar stored in the corresponding UCell* field of the UVM struct
 * (all_cells_head, gray_work_head, sweep_cursor, sweep_cursor_prev).
 *
 * The cast convention is identical across all six (one-line cast through
 * `void *` to satisfy strict-aliasing).  The functions are kept as
 * separate inlines for type safety — a single parameterized helper would
 * defeat the type-safety value the audit cited.  See the T27 comment
 * threads above for what changes when the sidecar layout collapses
 * (each accessor body switches to a direct UCell* cast; signatures are
 * load-bearing across the file and stay). */
static UAllCellsNode *gc_node_head(UVM *vm) {
    /* Intentional sidecar type laundering; UCell* and UAllCellsNode* alias
     * the same storage by design.  See file-header on T27 collapse. */
    /* NOLINTNEXTLINE(bugprone-casting-through-void) */
    return (UAllCellsNode *)(void *)vm->all_cells_head;
}

/* Accessor: recover the gray-list head from vm->gray_work_head.
 * T27: when sidecar disappears, vm->gray_work_head holds UCell* directly. */
static UAllCellsNode *gc_gray_head(UVM *vm) {
    /* See gc_node_head — sidecar laundering. */
    /* NOLINTNEXTLINE(bugprone-casting-through-void) */
    return (UAllCellsNode *)(void *)vm->gray_work_head;
}

/* Set the gray-list head in vm->gray_work_head.
 * T27: when sidecar disappears, store UCell* directly. */
static void gc_set_gray_head(UVM *vm, UAllCellsNode *node) {
    /* NOLINTNEXTLINE(bugprone-casting-through-void) — see gc_node_head. */
    vm->gray_work_head = (UCell *)(void *)node;
}

/* Accessor: recover sidecar from vm->sweep_cursor.
 * T27: when sidecar disappears, vm->sweep_cursor holds UCell* directly. */
static UAllCellsNode *gc_sweep_node(UVM *vm) {
    /* See gc_node_head — sidecar laundering. */
    /* NOLINTNEXTLINE(bugprone-casting-through-void) */
    return (UAllCellsNode *)(void *)vm->sweep_cursor;
}

/* Accessor: recover prev-sidecar from vm->sweep_cursor_prev.
 * T27: when sidecar disappears, vm->sweep_cursor_prev holds UCell* directly. */
static UAllCellsNode *gc_sweep_node_prev(UVM *vm) {
    /* See gc_node_head — sidecar laundering. */
    /* NOLINTNEXTLINE(bugprone-casting-through-void) */
    return (UAllCellsNode *)(void *)vm->sweep_cursor_prev;
}

/* Set vm->sweep_cursor to a sidecar node (or NULL).
 * T27: when sidecar disappears, store UCell* directly. */
static void gc_set_sweep_cursor(UVM *vm, UAllCellsNode *node) {
    /* NOLINTNEXTLINE(bugprone-casting-through-void) — see gc_node_head. */
    vm->sweep_cursor = (UCell *)(void *)node;
}

/* Set vm->sweep_cursor_prev to a sidecar node (or NULL).
 * T27: when sidecar disappears, store UCell* directly. */
static void gc_set_sweep_cursor_prev(UVM *vm, UAllCellsNode *node) {
    /* NOLINTNEXTLINE(bugprone-casting-through-void) — see gc_node_head. */
    vm->sweep_cursor_prev = (UCell *)(void *)node;
}

/* === Static helpers for gray work-list and all-cells traversal ===
 *
 * find_sidecar_for_cell: linear scan of the all-cells sidecar list to find
 * the sidecar node whose cell pointer matches the target cell.  O(N) at T24;
 * T27 collapse adds a back-pointer that makes this O(1).
 *
 * Returns NULL if not found (shouldn't happen in correct use). */
static UAllCellsNode *
find_sidecar_for_cell(UVM *vm, const UCell *target)
{
    UAllCellsNode *node = gc_node_head(vm);
    while (node != NULL) {
        if (node->cell == target) return node;
        node = node->next;
    }
    return NULL;
}

/* === mark_root_callback ===
 *
 * Called by the mark phase for each UValue root slot.  If the value is
 * heap-bearing and its cell is current_white, paints the cell gray and
 * pushes it onto the gray work-list via its sidecar node.
 *
 * At M3 baseline the only heap-bearing UValKind is UVAL_CLOSURE (carries
 * a UClosure*, which is a UCell-based object).  All other kinds (NIL, INT,
 * FLOAT, BOOL, STR, VOID) either have no cell pointer or have cells managed
 * outside the GC heap (interned strings, v1 strong roots).
 *
 * T25: uvalue_is_heap() + uvalue_as_cell() are now defined as static inline
 * in ugc_incremental.h.  This callback keeps its own inline kind check for
 * the sidecar-based mark path; the barrier surfaces use the helpers. */
static void
mark_root_callback(UVM *vm, UValue *slot, void *ctx)
{
    (void)ctx;  /* ctx == vm; not needed separately */

    /* Shade any heap-bearing value (UVAL_CLOSURE, UVAL_OBJECT, UVAL_EVENT).
     * M3 baseline only handled UVAL_CLOSURE; M4 added UVAL_OBJECT and M5
     * UVAL_EVENT as heap-managed cell types. */
    if (!uvalue_is_heap(*slot)) return;
    if (slot->v.p == NULL) return;

    UCell *cell = (UCell *)(slot->v.p);

    /* Only shade if cell is white (not yet gray or black).
     * Skip already-grayed/blackened cells for idempotency. */
    if (IS_GRAY(cell) || IS_BLACK(cell)) return;

    /* Paint gray and push onto work-list via sidecar. */
    gc_shade_gray(vm, cell);
}

/* === drain_gray: shared gray work-list drainer (GC-027) ===
 *
 * Pops cells from the gray work-list, walks their payload (if any), and
 * paints each cell black.  Stops when the gray list empties or when
 * `consumed` reaches `budget` (pass SIZE_MAX to drain completely).
 *
 * Returns bytes of work consumed.  The gray list state in vm is updated
 * in-place; callers check gc_gray_head(vm) == NULL to detect completion. */
static size_t
drain_gray(UVM *vm, size_t budget)
{
    size_t consumed = 0U;
    while (gc_gray_head(vm) != NULL && consumed < budget) {
        /* T27: when sidecar disappears, vm->gray_work_head holds UCell* directly. */
        UAllCellsNode *node = gc_gray_head(vm);
        UCell *cell = node->cell;

        /* Pop from gray work-list. */
        gc_set_gray_head(vm, node->next_gray);
        node->next_gray = NULL;

        /* Walk payload if a type walker is registered. */
        const UType *t = vm->type_table[cell->type_tag];
        if (t != NULL && t->walk_payload != NULL) {
            /* Payload starts immediately after UCell header.
             * At M3 no concrete types exist, so this is unreachable in practice. */
            t->walk_payload(vm, (void *)(cell + 1), mark_root_callback, vm);
        }

        /* Paint cell black — fully scanned. */
        urbi_gc_set_color(cell, UGC_COLOR_BLACK);

        consumed += node->size;
    }
    return consumed;
}

/* === gc_mark_roots_step ===
 *
 * MARK_ROOTS phase: enumerate all roots (VM globals + registered providers),
 * invoke mark_root_callback for each root slot.  Transitions to
 * MARK_INCREMENTAL.  This is a stop-the-world step (single slice, bounded
 * by root surface).
 *
 * Returns approximate work units consumed (1024 = one root-scan slice). */
static size_t
gc_mark_roots_step(UVM *vm)
{
    /* Walk all registered root providers.
     * VM-level UValue globals (e.g. fatal_handler_value, prototypes[]) are
     * reached via root providers — no separate VM-globals walk needed. */
    uint8_t i;
    for (i = 0U; i < vm->root_provider_count; i++) {
        vm->root_providers[i](vm, mark_root_callback, vm);
    }

    vm->gc_phase = GC_PHASE_MARK_INCREMENTAL;
    URBI_TP(vm, URBI_TRACE_GC, URBI_LOG_DEBUG, URBI_TP_GC_PHASE, (uint32_t)vm->gc_phase, 0);
    return 1024U;  /* approximate work units for root scanning */
}

/* === gc_mark_incremental_step ===
 *
 * MARK_INCREMENTAL phase: drain the gray work-list up to `budget` bytes of
 * work.  For each gray cell, walk its payload (if any UType walker is
 * registered), then paint it black.  If the work-list empties, transition
 * to ATOMIC_FINISH.
 *
 * At M3 baseline no concrete cell types exist and no type_table entries are
 * registered, so walk_payload is never called.  The consumed accounting
 * uses the sidecar's `size` field (correct per the sidecar design).
 *
 * Returns bytes of work consumed. */
static size_t
gc_mark_incremental_step(UVM *vm, size_t budget)
{
    size_t consumed = drain_gray(vm, budget);

    if (gc_gray_head(vm) == NULL) {
        vm->gc_phase = GC_PHASE_ATOMIC_FINISH;
        URBI_TP(vm, URBI_TRACE_GC, URBI_LOG_DEBUG, URBI_TP_GC_PHASE, (uint32_t)vm->gc_phase, 0);
    }

    return consumed;
}

/* === gc_atomic_finish_step ===
 *
 * ATOMIC_FINISH phase: stop-the-world re-scan of mutator-touched roots, then
 * drain any residual gray work-list.  At completion, all live cells are black
 * and everything else is dead.  Transitions to SWEEP.
 *
 * Per-strand re-scan is deferred: vm->cur_strand does not exist on UVM per
 * T19 design choice (adding it would create a new invariant hazard).
 * TODO(T26): when the scheduler tracks the actively-dispatching strand,
 * re-scan its registers here.  M3 baseline has no cur_strand pointer per
 * T19 design choice.
 *
 * Returns bytes of work consumed (accumulated from gray-list drain). */
static size_t
gc_atomic_finish_step(UVM *vm)
{
    /* TODO(T26): re-scan currently-running strand's register window here
     * once vm->cur_strand (or equivalent) is available.  M3 baseline
     * skips per-strand re-scan — see T19 design notes. */

    /* Drain residual gray work-list (fully in-slice — bounded by remaining
     * gray set after MARK_INCREMENTAL, which converges because no mutator
     * runs during ATOMIC_FINISH). */
    size_t consumed = drain_gray(vm, (size_t)-1U);

    /* Gray work-list should be fully drained before SWEEP. */
    URBI_INTERNAL_ASSERT(gc_gray_head(vm) == NULL);

    /* Transition to SWEEP; initialise sweep cursor to start of all-cells list.
     * Reset gc_surviving_bytes accumulator (closes GC-015): each gc_sweep_step
     * slice now adds only the cells it processed in that slice.  Mid-sweep
     * allocations prepend to all_cells_head with current_white color and are
     * never visited by the cursor walk, so they don't contribute. */
    vm->gc_phase = GC_PHASE_SWEEP;
    URBI_TP(vm, URBI_TRACE_GC, URBI_LOG_DEBUG, URBI_TP_GC_PHASE, (uint32_t)vm->gc_phase, 0);
    vm->gc_surviving_bytes = 0U;
    gc_set_sweep_cursor(vm, gc_node_head(vm));
    gc_set_sweep_cursor_prev(vm, NULL);

    /* Return value contract:
     *   >0 — bytes-of-gray-work consumed in this atomic-finish step.  The
     *        slice scheduler subtracts this from the slice budget; if the
     *        budget remains >0 the SWEEP phase begins immediately within
     *        the same slice (see urbi_gc_slice loop call site at line ~760).
     *   64u — sentinel returned when the gray list was already empty.  We
     *        cannot return 0 because the slice scheduler reads 0 as "GC is
     *        idle, no atomic work pending" and would wedge the loop on
     *        successive 0-returns when the slice budget hasn't refilled.
     *        64u is a small constant chosen to (a) make forward progress
     *        through the SWEEP phase on the same slice, (b) be small enough
     *        that downstream sweep_step bounds dominate the slice budget
     *        accounting, and (c) match the granularity of `gc_mark_roots_step`
     *        which also uses small constants for empty-work shapes.
     *
     * The 64u sentinel exists only because callers count "0" as "GC done".
     * If the slice scheduler ever distinguishes "no work this slice, retry
     * next safepoint" from "GC is idle", this sentinel can collapse to 0. */
    return consumed > 0U ? consumed : 64U;
}

/* === end_of_cycle_threshold_update ===
 *
 * Called at SWEEP → IDLE transition.  Updates gc_threshold based on the
 * number of bytes that survived the sweep (gc_live_bytes was set by
 * gc_sweep_step before calling this).  Resets gc_debt to -threshold so the
 * mutator has a full "threshold bytes" of credit before the next cycle.
 * Clears gc_pending.
 *
 * Threshold formula: threshold = live_bytes * URBI_GC_PAUSE_RATIO / 100.
 * Guard: if live_bytes is zero (all cells collected), clamp to
 * URBI_GC_INITIAL_THRESHOLD to avoid gc_debt staying at 0 (starvation). */
static void
end_of_cycle_threshold_update(UVM *vm)
{
    size_t live = vm->gc_live_bytes;
    size_t threshold;

    if (live == 0U) {
        threshold = (size_t)URBI_GC_INITIAL_THRESHOLD;
    } else {
        threshold = (live * (size_t)URBI_GC_PAUSE_RATIO) / 100U;
        if (threshold < (size_t)URBI_GC_INITIAL_THRESHOLD) {
            threshold = (size_t)URBI_GC_INITIAL_THRESHOLD;
        }
    }

    vm->gc_threshold = threshold;
    vm->gc_debt      = -(int64_t)threshold;
    vm->gc_pending   = 0U;
}

/* === gc_sweep_step ===
 *
 * SWEEP phase: walk the all-cells sidecar list starting from the current
 * sweep cursor.  For each cell:
 *   - IS_DEAD (color == OTHER_WHITE): unlink sidecar, run finalizer if set,
 *     free cell + sidecar.  Increment consumed.
 *   - UGC_IS_FIXED (pool-managed): re-paint to current_white; advance;
 *     accumulate to vm->gc_surviving_bytes.
 *   - UGC_IS_PINNED (host-pinned): re-paint to current_white; advance;
 *     accumulate to vm->gc_surviving_bytes.
 *   - All others: re-paint to current_white; advance; accumulate.
 *
 * Surviving-bytes accumulation persists across slices in vm->gc_surviving_bytes
 * (initialised to 0 at SWEEP entry by gc_atomic_finish_step).  Each slice adds
 * only the cells it processed in that slice; mid-slice allocations prepend to
 * all_cells_head with current_white color and are never visited by the cursor
 * walk, so they're correctly excluded from the survivor count (closes GC-015).
 *
 * The previous shape re-derived surviving_bytes by walking from head to
 * sweep_cursor at every slice entry; that walk over-counted intra-slice
 * allocations because they prepended to head between slices and ended up
 * "before sweep_cursor" in the list.
 *
 * vm->gc_surviving_bytes is written to vm->gc_live_bytes only when the sweep
 * completes (cursor reaches end).
 *
 * Returns bytes of work consumed (used by urbi_gc_slice budget tracking). */
static size_t
gc_sweep_step(UVM *vm, size_t budget)
{
    size_t consumed = 0U;

    UAllCellsNode *prev = gc_sweep_node_prev(vm);
    UAllCellsNode *cur  = gc_sweep_node(vm);

    while (cur != NULL && consumed < budget) {
        UAllCellsNode *next = cur->next;
        UCell         *cell = cur->cell;

        /* FIXED and PINNED cells are exempt from collection per spec §3.6.
         * Check these flags BEFORE IS_DEAD: even if a fixed/pinned cell's
         * color didn't get updated by the mark phase (because no root
         * registered it as reachable), it must not be freed.  Re-paint it
         * to current_white so it survives further cycles too.
         *
         * GC-007: the re-paint is REQUIRED, not redundant.  FIXED means
         * "don't free"; it does NOT mean "skip color update".  A FIXED
         * cell's color must track current_white at every sweep boundary
         * so the next mark phase observes it as not-yet-marked.  Without
         * the re-paint, a FIXED cell that survived two cycles in a row
         * (without being re-walked by its specialised root walker, e.g.
         * watcher_table_walk_roots for UWatcher cells) would carry stale
         * non-current-white color into the next mark, breaking the
         * tri-color invariant.  Do not "optimise" this branch by
         * dropping the urbi_gc_set_color call without a separate root
         * walker that paints the cell every cycle.
         *
         * Note that some FIXED cells (UWatcher) are walked through a
         * dedicated root walker (watcher_table_walk_roots) that
         * traverses active_watchers_head independently of the all-cells
         * sidecar list — so the cells exist in the sweep iteration even
         * when no ordinary heap reference reaches them. */
        if ((cell->gc_byte & (UGC_IS_FIXED | UGC_IS_PINNED)) != 0U) {
            urbi_gc_set_color(cell, vm->current_white);
            URBI_INTERNAL_ASSERT(
                (cell->gc_byte & UGC_COLOR_MASK) == vm->current_white);
            vm->gc_surviving_bytes += cur->size;
            consumed              += cur->size;
            prev = cur;
            cur  = next;

        } else if (IS_DEAD(vm, cell)) {
            /* Dead cell: unlink from all-cells list and free. */

            /* Unlink sidecar. */
            if (prev == NULL) {
                /* cur was the head.  Sidecar pattern, see file-header. */
                /* NOLINTNEXTLINE(bugprone-casting-through-void) */
                vm->all_cells_head = (UCell *)(void *)next;
            } else {
                prev->next = next;
            }

            /* Run finalizer if registered. */
            if ((cell->gc_byte & UGC_HAS_FINALIZER) != 0U) {
                const UType *t = vm->type_table[cell->type_tag];
                if (t != NULL && t->destroy != NULL) {
                    vm->in_destroy_callback = 1U;
                    t->destroy(vm, (void *)(cell + 1));
                    vm->in_destroy_callback = 0U;
                }
            }

            consumed += cur->size;

            /* Free cell, then sidecar.  Under MEM_DEBUG the cell is poisoned
             * and quarantined (UAF detection); read owner off `cur` first. */
#if URBI_MEM_DEBUG
            umemdbg_release_cell(vm, cell, cur->size, cur->seq, cur->owner_pc, cur->owner_ret);
#else
            vm->alloc_fn(cell, 0U, vm->alloc_ud);
#endif
            vm->alloc_fn(cur,  0U, vm->alloc_ud);

            /* prev stays unchanged; cur moves to next. */
            cur = next;

        } else {
            /* Live cell (marked black): re-paint to current_white. */
            urbi_gc_set_color(cell, vm->current_white);
            vm->gc_surviving_bytes += cur->size;
            consumed              += cur->size;
            prev = cur;
            cur  = next;
        }
    }

    /* Save updated cursors. */
    gc_set_sweep_cursor_prev(vm, prev);
    gc_set_sweep_cursor(vm, cur);

    if (cur == NULL) {
        /* Sweep complete: publish surviving total and trigger threshold update. */
        vm->gc_live_bytes = vm->gc_surviving_bytes;
        vm->gc_phase      = GC_PHASE_IDLE;
        /* v0.11.1: close out the cycle (always-on). Bump the cycle count and
         * turn the start-timestamp held in last_gc_us into a duration. */
        vm->gc_cycles++;
        if (vm->host_time_us && vm->last_gc_us != 0) {
            uint64_t now = vm->host_time_us(vm->host_time_ud);
            uint64_t dur = (now >= vm->last_gc_us) ? (now - vm->last_gc_us) : 0;
            vm->last_gc_us = dur;        /* repurpose: now holds the DURATION */
            vm->total_gc_us += dur;
        } else {
            vm->last_gc_us = 0;
        }
        URBI_TP(vm, URBI_TRACE_GC, URBI_LOG_DEBUG, URBI_TP_GC_PHASE, (uint32_t)vm->gc_phase, 0);
        end_of_cycle_threshold_update(vm);
    }

    return consumed;
}

/* === urbi_gc_init ===
 *
 * urbi_vm_init() already zero-initialises every GC field added at T4/T22, so
 * urbi_gc_init only needs to set fields whose correct initial value is NOT
 * zero: gc_threshold and gc_debt.  All pointer and flag fields are already
 * NULL / 0 after urbi_vm_init's zero pass.
 *
 * Called from urbi_vm_init() after all other field zero-init. */
void
urbi_gc_init(UVM *vm)
{
    URBI_ASSERT_NOT_ISR(vm);

    /* Fields already zero-init by urbi_vm_init:
     *   gc_phase, current_white, gc_paused, in_destroy_callback,
     *   gc_live_bytes, gc_total_allocated,
     *   all_cells_head, gray_work_head, sweep_cursor, sweep_cursor_prev,
     *   root_providers[], root_provider_count, type_table[], gc_pending,
     *   watcher_dirty_count.
     * Only non-zero initial values need explicit assignment here. */
    vm->gc_threshold = (size_t)URBI_GC_INITIAL_THRESHOLD;
    vm->gc_debt      = -(int64_t)URBI_GC_INITIAL_THRESHOLD;
}

/* === urbi_gc_destroy ===
 *
 * Walks the all-cells sidecar list and frees every cell via the VM allocator,
 * regardless of color or UGC_IS_FIXED / UGC_IS_PINNED flags.  Spec §3.6
 * "FIXED cells are never freed during SWEEP" applies only to normal
 * incremental collection — at VM teardown every cell is freed unconditionally.
 *
 * If the type_table entry for a cell has a destroy callback and the cell has
 * UGC_HAS_FINALIZER set, the finalizer is called before freeing.
 *
 * vm->in_destroy_callback is set around finalizer calls so that any
 * re-entrant urbi_gc_alloc (which would be a bug, but guard anyway) can
 * assert or no-op.
 *
 * Called from urbi_vm_destroy() as the last subsystem teardown step, after
 * urealm_teardown_all() and all other subsystems that might still hold
 * cells (at M3 no subsystem allocates via urbi_gc_alloc yet, so ordering
 * is loose — placing gc_destroy last is safe and correct for all futures). */
void
urbi_gc_destroy(UVM *vm)
{
    URBI_ASSERT_NOT_ISR(vm);

    UAllCellsNode *node = gc_node_head(vm);
    vm->all_cells_head = NULL;

    while (node != NULL) {
        UAllCellsNode *next_node = node->next;
        UCell         *cell      = node->cell;

        /* Call finalizer if registered. */
        if ((cell->gc_byte & UGC_HAS_FINALIZER) != 0U) {
            const UType *t = vm->type_table[cell->type_tag];
            if (t != NULL && t->destroy != NULL) {
                vm->in_destroy_callback = 1U;
                /* Payload starts immediately after UCell header.
                 * At M3 no concrete cell types exist, so this path is
                 * unreachable in practice; provided for correctness at T27+. */
                t->destroy(vm, (void *)(cell + 1));
                vm->in_destroy_callback = 0U;
            }
        }

        /* Free the cell.  Under MEM_DEBUG the cell is poisoned and quarantined;
         * umemdbg_destroy (called from urbi_vm_destroy after this) flushes +
         * poison-verifies the whole quarantine.  Read owner off `node` first. */
#if URBI_MEM_DEBUG
        umemdbg_release_cell(vm, cell, node->size, node->seq, node->owner_pc, node->owner_ret);
#else
        vm->alloc_fn(cell, 0U, vm->alloc_ud);
#endif

        /* Free the sidecar node itself. */
        vm->alloc_fn(node, 0U, vm->alloc_ud);

        node = next_node;
    }

    /* Null out sweep cursors so stale pointers don't linger. */
    gc_set_sweep_cursor(vm, NULL);
    gc_set_sweep_cursor_prev(vm, NULL);
    gc_set_gray_head(vm, NULL);
}

/* === urbi_gc_alloc ===
 *
 * Allocates a new GC-managed cell of the given size and type_tag.
 * Returns NULL on OOM (either the cell alloc or the sidecar alloc).
 *
 * The cell is born current_white per spec §3.5 (survives the current cycle;
 * ATOMIC_FINISH re-scan picks it up if it's reachable).
 *
 * The sidecar node is prepended to the all-cells list (O(1) insert;
 * sweep order is unspecified by the spec).
 *
 * gc_debt is incremented by size; if debt crosses zero and GC is not
 * paused, gc_pending is set to request a gc_slice at the next safepoint. */
UCell *
urbi_gc_alloc(UVM *vm, size_t size, uint8_t type_tag)
{
    URBI_ASSERT_NOT_ISR(vm);

    /* Phase 13 / T145: urbi_lock_heap one-way latch.  Once locked,
     * decline new allocations — caller observes NULL (the standard
     * OOM-shaped failure mode the rest of the runtime already handles
     * via urbi_raise_oom on the script surface). */
    if (UNLIKELY(vm->heap_locked)) {
        URBI_TP(vm, URBI_TRACE_GC, URBI_LOG_WARN, URBI_TP_GC_ALLOC_DENIED,
                (uint32_t)size, 0);
#if URBI_MEM_DEBUG
        if (vm->memdbg == NULL) umemdbg_init(vm);
        umemdbg_note_heaplock(vm, size,
            vm->cur_strand ? vm->cur_strand->pc : NULL,
            (vm->cur_strand && vm->cur_strand->pc)
                ? (uint16_t)uinstr_op(*vm->cur_strand->pc) : 0xFFFFu,
            (uint16_t)(uintptr_t)vm->cur_strand, __builtin_return_address(0));
#endif
        return NULL;
    }

    /* Allocate the cell (+ trailing redzone in MEM_DEBUG builds). */
#if URBI_MEM_DEBUG
    size_t alloc_size = size + URBI_MEM_REDZONE_BYTES;
#else
    size_t alloc_size = size;
#endif
    UCell *cell = (UCell *)vm->alloc_fn(NULL, alloc_size, vm->alloc_ud);
    if (UNLIKELY(cell == NULL)) return NULL;

    /* Zero-init the USER bytes (no memset — freestanding). */
    urbi_zero(cell, size);

    /* Allocate the sidecar node. */
    UAllCellsNode *node = (UAllCellsNode *)vm->alloc_fn(
            NULL, sizeof(UAllCellsNode), vm->alloc_ud);
    if (UNLIKELY(node == NULL)) {
        vm->alloc_fn(cell, 0U, vm->alloc_ud);
        return NULL;
    }

    /* Initialize cell header. */
    cell->type_tag = type_tag;
    cell->gc_byte  = vm->current_white;   /* born current_white per spec §3.5 */

    /* v0.8.4: mirror the type's TYPE_HAS_FINALIZER flag onto the cell's
     * gc_byte so that gc_sweep_step and urbi_gc_destroy will call the
     * finalizer.  Without this, types registered with a non-NULL destroy
     * (UTYPE_CLOSURE since Step B) would have their finalizer silently
     * skipped — every test prior to v0.8.4 had flags == 0 on every type
     * registration, so this code path was latent and untested. */
    {
        const UType *t = vm->type_table[type_tag];
        if (t != NULL && (t->flags & TYPE_HAS_FINALIZER) != 0U) {
            cell->gc_byte |= UGC_HAS_FINALIZER;
        }
    }

    /* Initialize sidecar node and prepend to all-cells list. */
    node->cell = cell;
    node->size = size;
    node->next = gc_node_head(vm);
    node->next_gray = NULL;
    /* Store sidecar head as UCell* (cast convention documented at top of
     * file).  Sidecar pattern, see file-header. */
    /* NOLINTNEXTLINE(bugprone-casting-through-void) */
    vm->all_cells_head = (UCell *)(void *)node;

#if URBI_MEM_DEBUG
    /* v0.11.3 owner tag + trailing redzone. memdbg is lazy (may stay NULL on
     * OOM — owner/seq then degrade; the redzone still works). */
    if (vm->memdbg == NULL) umemdbg_init(vm);
    node->seq       = (vm->memdbg != NULL) ? ++vm->memdbg->alloc_seq : 0u;
    node->owner_pc  = (vm->cur_strand != NULL) ? vm->cur_strand->pc : NULL;
    node->owner_op  = (node->owner_pc != NULL) ? (uint16_t)uinstr_op(*node->owner_pc) : 0xFFFFu;
    node->owner_ret = __builtin_return_address(0);
    node->strand_id = (uint16_t)(uintptr_t)vm->cur_strand;
    umemdbg_write_redzone(cell, size);
#endif

    /* Accounting (USER size — redzone is debug-only overhead, kept out of
     * gc accounting so GC pacing matches release builds). */
    vm->gc_total_allocated += size;
    vm->gc_debt += (int64_t)size;

    /* Trigger: if debt turns positive and GC is not paused, request a slice. */
    if (UNLIKELY(vm->gc_debt > 0 && !vm->gc_paused)) {
        vm->gc_pending = 1U;
    }

    return cell;
}

/* === gc_shade_gray ===
 *
 * Paints a cell gray and pushes it onto the gray work-list via its sidecar
 * node.  Called from mark_root_callback and the forward write barrier (T25).
 *
 * Algorithm:
 *   1. Paint cell gc_byte to GRAY.
 *   2. Find the cell's sidecar node (O(N) linear scan; T27 collapse makes
 *      this O(1) via back-pointer).
 *   3. Push sidecar onto the gray work-list via next_gray.
 *      Skip if sidecar is already on the gray list (next_gray != NULL or
 *      is already the gray head).
 *
 * Idempotency guard: if the cell is already gray or black, no-op. */
void
gc_shade_gray(UVM *vm, UCell *cell)
{
    URBI_ASSERT_NOT_ISR(vm);

    /* Idempotency: only shade white cells. */
    uint8_t color = (uint8_t)(cell->gc_byte & UGC_COLOR_MASK);
    if (color == UGC_COLOR_GRAY || color == UGC_COLOR_BLACK) return;

    /* Paint gray. */
    urbi_gc_set_color(cell, UGC_COLOR_GRAY);

    /* Find sidecar — O(N) at T24.
     * T27: replace with back-pointer lookup once sidecar disappears.
     *
     * NULL contract (closes GC-009 — DOCUMENT-only resolution):
     *
     * v0.5.x has THREE distinct cell-allocation regimes; only one of them
     * adds a sidecar to vm->all_cells_head.  A NULL return here is a
     * legitimate, expected outcome for the other two — NOT a bug:
     *
     *   1. urbi_gc_alloc cells (UObject, UEvent, UTag, UChangedNode,
     *      UShape, UProtos, USlots, UChunkInstance ...): sidecar
     *      enrolled at alloc; sweep walks via cursor; gc_shade_gray
     *      pushes onto the gray work-list via the sidecar so the
     *      drain_gray loop reaches walk_payload.
     *
     *   2. UWatcher pool slots (UGC_IS_FIXED): pool-managed via
     *      vm->alloc_fn at uwatcher_pool_init; never freed by sweep
     *      (spec §3.6); payload references walked via the dedicated
     *      watcher_table_walk_roots root provider, not via the type
     *      walker.  No sidecar; gc_shade_gray called from walk_uevent /
     *      walk_utag for chain shading sets the color flag (idempotency)
     *      but the work-list push is correctly a no-op.
     *
     *   3. UClosure / UUpvalCell cells (vm_alloc_closure / vm_open_upvalue):
     *      fully GC-managed since v0.8.4 Step C-2 (enrolled on all_cells_head
     *      via urbi_gc_alloc with UTYPE_CLOSURE / UTYPE_UPVAL_CELL).  The
     *      legacy strand closure_list free-list was deleted at Step C-3.
     *
     * Pre-GC-009-fix shape: silent `if (!node) return;` covered all three
     * cases but obscured which were intentional.  The audit asked for
     * either an explicit guard (early-return per regime) or a thorough
     * comment.  We keep silent-return + thorough comment because:
     *   - regime (2) FIXED-skip would still leave (3) UClosure unhandled
     *     without a parallel UCell-header-only fast-path,
     *   - upgrading (3) to GC-managed is the right v1.x fix and will
     *     eliminate this class of NULL altogether (sidecar enrolled,
     *     work-list push proceeds normally).
     *
     * Future-proof: the symmetric T27 sidecar-collapse drops this lookup
     * entirely (back-pointer in UCell payload makes it O(1) and makes the
     * NULL question moot for regime 1; regimes 2 and 3 will still take
     * the silent-return path until they migrate to urbi_gc_alloc). */
    UAllCellsNode *node = find_sidecar_for_cell(vm, cell);
    if (node == NULL) return;  /* expected for FIXED + UClosure regimes; see contract above */

    /* Push onto gray work-list if not already on it.
     * Guard: next_gray == NULL means "not on gray list".  A node that's
     * the gray head also has its next_gray set to the next node (or NULL
     * if it's the only node), so we can't rely on next_gray == NULL to
     * mean "is the head".  Use a simple "already gray color" check above
     * for the fast path; the push here is only reached when cell was white. */
    node->next_gray = gc_gray_head(vm);
    gc_set_gray_head(vm, node);
}

/* === urbi_gc_slice ===
 *
 * Per-safepoint GC slice driver.  Advances the state machine by approximately
 * byte_budget bytes of work.  Called from the dispatcher when gc_pending != 0
 * (row 9 §5.4).
 *
 * Phase transitions:
 *   IDLE            — if gc_debt > 0 and !gc_paused: flip current_white,
 *                     start cycle (→ MARK_ROOTS).  Otherwise clear pending.
 *   MARK_ROOTS      — one bounded root-scan step (→ MARK_INCREMENTAL).
 *   MARK_INCREMENTAL — drain gray work-list up to remaining budget.
 *   ATOMIC_FINISH   — one bounded STW drain (→ SWEEP).
 *   SWEEP           — walk all-cells list up to remaining budget.
 *
 * Returns when budget is exhausted or when the state machine reaches IDLE
 * (end of cycle).
 *
 * ISR note: urbi_gc_slice is NOT ISR-safe — it allocates/frees memory and
 * modifies shared VM state. */
void
urbi_gc_slice(UVM *vm, size_t byte_budget)
{
    URBI_ASSERT_NOT_ISR(vm);

    size_t consumed = 0U;

    vm->gc_slices++;   /* v0.11.1: one incremental slice driver invocation */

    while (consumed < byte_budget) {
        switch (vm->gc_phase) {

        case GC_PHASE_IDLE:
            if (vm->gc_debt > 0 && !vm->gc_paused) {
                /* Start a new collection cycle: flip current_white so that
                 * all existing cells (born in the previous white) become
                 * "other white" and are treated as dead unless re-marked. */
                vm->current_white ^= 0x01U;
                vm->gc_phase = GC_PHASE_MARK_ROOTS;
                /* v0.11.1: stamp cycle-start (always-on; 0 if no clock).
                 * last_gc_us holds the START timestamp during the cycle and is
                 * overwritten with the DURATION at cycle end (GC_PHASE_IDLE). */
                vm->last_gc_us = vm->host_time_us ? vm->host_time_us(vm->host_time_ud) : 0;
                URBI_TP(vm, URBI_TRACE_GC, URBI_LOG_DEBUG, URBI_TP_GC_PHASE, (uint32_t)vm->gc_phase, 0);
            } else {
                /* Nothing to do. */
                vm->gc_pending = 0U;
                return;
            }
            break;

        case GC_PHASE_MARK_ROOTS:
            consumed += gc_mark_roots_step(vm);
            break;

        case GC_PHASE_MARK_INCREMENTAL:
            consumed += gc_mark_incremental_step(vm, byte_budget - consumed);
            break;

        case GC_PHASE_ATOMIC_FINISH:
            consumed += gc_atomic_finish_step(vm);
            break;

        case GC_PHASE_SWEEP:
            consumed += gc_sweep_step(vm, byte_budget - consumed);
            /* If sweep completed and we returned to IDLE, stop the slice
             * (threshold and debt were reset; no further work this slice). */
            if (vm->gc_phase == GC_PHASE_IDLE) return;
            break;

        default:
            /* Unreachable; defensive return. */
            return;
        }
    }
}

/* === urbi_gc_force_full ===
 *
 * Runs the GC state machine to completion synchronously.  If currently IDLE,
 * starts a new cycle by flipping current_white and entering MARK_ROOTS.
 * Runs slices with SIZE_MAX budget until back to IDLE.
 *
 * Intended for testing and explicit collection requests (urbi_gc_collect).
 * Not for production use on MCUs where bounded pauses are required. */
void
urbi_gc_force_full(UVM *vm)
{
    URBI_ASSERT_NOT_ISR(vm);

    /* If already IDLE, start a new cycle. */
    if (vm->gc_phase == GC_PHASE_IDLE) {
        vm->current_white ^= 0x01U;
        vm->gc_phase = GC_PHASE_MARK_ROOTS;
        /* v0.11.1: stamp cycle-start (see urbi_gc_slice for the repurpose note). */
        vm->last_gc_us = vm->host_time_us ? vm->host_time_us(vm->host_time_ud) : 0;
        URBI_TP(vm, URBI_TRACE_GC, URBI_LOG_DEBUG, URBI_TP_GC_PHASE, (uint32_t)vm->gc_phase, 0);
    }

    /* Run slices until IDLE.  Use SIZE_MAX budget per slice to complete
     * each phase in one shot. */
    while (vm->gc_phase != GC_PHASE_IDLE) {
        urbi_gc_slice(vm, (size_t)-1U);
    }
}

/* === urbi_gc_walk_roots ===
 *
 * Iterates all registered root providers.  Provided for host/test use and
 * called indirectly via gc_mark_roots_step.  VM-level globals (if any) are
 * reached via providers — no separate VM-globals walk. */
void
urbi_gc_walk_roots(UVM *vm, UGcRootCallback cb, void *ctx)
{
    URBI_ASSERT_NOT_ISR(vm);
    uint8_t i;
    for (i = 0U; i < vm->root_provider_count; i++) {
        vm->root_providers[i](vm, cb, ctx);
    }
}

/* === urbi_gc_walk_all_cells ===
 *
 * Generic all-cells iterator (T12).  Walks the sidecar list and invokes
 * cb(vm, cell, ctx) once per live cell.  cb must not free, allocate, or
 * trigger sweep work — it may safely mutate cell->gc_byte and the cell's
 * type-private payload bytes.
 *
 * Used by urbi_object_lookup_id_force_wrap to clear UObject.lookup_stamp on
 * u32 rollover.  The sidecar list pre-dates T27's trailing-pointer
 * collapse; once T27 lands the implementation here updates to the same
 * iteration form, but the public signature stays the same. */
void
urbi_gc_walk_all_cells(UVM *vm, UGcCellCallback cb, void *ctx)
{
    URBI_ASSERT_NOT_ISR(vm);
    UAllCellsNode *node = gc_node_head(vm);
    while (node != NULL) {
        cb(vm, node->cell, ctx);
        node = node->next;
    }
}

/* === urbi_gc_register_root_provider ===
 *
 * Appends provider to the VM's fixed root-provider array.
 * URBI_MAX_ROOT_PROVIDERS is 12 (bumped from 8→12 at Step C-1); capacity
 * assertion fires on overflow so the programmer knows to raise the constant. */
void
urbi_gc_register_root_provider(UVM *vm, UGcRootProviderFn provider)
{
    URBI_ASSERT_NOT_ISR(vm);
    URBI_INTERNAL_ASSERT(vm->root_provider_count < (uint8_t)URBI_MAX_ROOT_PROVIDERS);
    vm->root_providers[vm->root_provider_count++] = provider;
}

size_t
urbi_gc_bytes_allocated_inline(const UVM *vm)
{
    return vm->gc_total_allocated;
}

/* === urbi_gc_collect ===
 *
 * Trigger a full synchronous GC collection (all phases in one call).
 * Delegates to urbi_gc_force_full. */
void
urbi_gc_collect(UVM *vm)
{
    URBI_ASSERT_NOT_ISR(vm);
    urbi_gc_force_full(vm);
}

/* === urbi_gc_pause ===
 *
 * Pause / resume automatic GC collection.
 * While paused, urbi_gc_slice() will not start new cycles.
 * urbi_gc_force_full() / urbi_gc_collect() still work (explicit override). */
void
urbi_gc_pause(UVM *vm, bool paused)
{
    URBI_ASSERT_NOT_ISR(vm);
    vm->gc_paused = paused ? 1U : 0U;
}

/* === Read-only GC query functions ===
 *
 * These read simple VM fields.  They are NOT ISR-safe per se (reading a
 * size_t / uint64 across an ISR boundary is not guaranteed atomic on all
 * platforms), but they are safe for diagnostic use from non-ISR contexts.
 * URBI_ASSERT_NOT_ISR is omitted — they are test/diagnostic accessors. */

size_t
urbi_gc_bytes_allocated(const UVM *vm)
{
    return vm->gc_total_allocated;
}

size_t
urbi_gc_live_bytes(const UVM *vm)
{
    return vm->gc_live_bytes;
}

size_t
urbi_gc_threshold(const UVM *vm)
{
    return vm->gc_threshold;
}

uint8_t
urbi_gc_phase(const UVM *vm)
{
    return vm->gc_phase;
}

/* === urbi_pin / urbi_unpin ===
 *
 * Pin: set UGC_IS_PINNED so the cell is exempt from GC sweep.
 * Unpin: clear UGC_IS_PINNED, making the cell eligible for collection again.
 * Both are no-ops for non-heap UValues (NIL, INT, FLOAT, BOOL, STR, VOID).
 *
 * M3 ships single-bit pin (idempotent set/clear).  v1.x adds a refcount
 * table for nested pin/unpin pairs. */

#if URBI_GC_HAS_PINNING
void
urbi_pin(UVM *vm, UValue v)
{
    URBI_ASSERT_NOT_ISR(vm);
    if (!uvalue_is_heap(v)) return;
    UCell *c = uvalue_as_cell(v);
    if (c == NULL) return;
    c->gc_byte |= UGC_IS_PINNED;
    (void)vm;
}

void
urbi_unpin(UVM *vm, UValue v)
{
    URBI_ASSERT_NOT_ISR(vm);
    if (!uvalue_is_heap(v)) return;
    UCell *c = uvalue_as_cell(v);
    if (c == NULL) return;
    c->gc_byte = (uint8_t)(c->gc_byte & ~(uint8_t)UGC_IS_PINNED);
    (void)vm;
}
#endif

/* === uvalue_is_heap_white ===
 *
 * Defined here (not inline in the header) because ugc_incremental.h cannot
 * include uvm.h (circular dependency: uvm.h → urbi/gc.h → ugc_incremental.h).
 * The static-inline barriers in ugc_incremental.h call this via a forward decl;
 * the linker resolves the call to this TU where UVM is fully defined.
 *
 * uvalue_is_heap and uvalue_as_cell are static inline in ugc_incremental.h;
 * this function uses them directly. */
bool
uvalue_is_heap_white(const UVM *vm, UValue v)
{
    if (!uvalue_is_heap(v)) return false;
    const UCell *c = uvalue_as_cell(v);
    if (c == NULL) return false;
    return (c->gc_byte & UGC_COLOR_MASK) == vm->current_white;
}

#if URBI_MEM_DEBUG
/* === urbi_gc_mem_validate (v0.11.3) ===
 *
 * Validate every live cell's trailing redzone + the quarantine poison.
 * Returns the total violation count; accumulates into memdbg stats.
 * Lives here (not in umemdebug.c) because it needs the private
 * UAllCellsNode type and the gc_node_head accessor. */
int
urbi_gc_mem_validate(UVM *vm)
{
    int viol = 0;
    UAllCellsNode *n = gc_node_head(vm);
    while (n != NULL) {
        const uint8_t *rz = (const uint8_t *)n->cell + n->size;
        size_t i;
        for (i = 0; i < URBI_MEM_REDZONE_BYTES; i++) {
            if (rz[i] != (uint8_t)URBI_MEM_REDZONE_BYTE) {
                if (vm->memdbg) vm->memdbg->redzone_violations++;
                viol++;
                break;
            }
        }
        n = n->next;
    }
    viol += umemdbg_quarantine_verify(vm);
    return viol;
}

/* === urbi_gc_count_pinned (v0.11.3) ===
 *
 * Count cells flagged UGC_IS_PINNED — a never-unpinned pin is a host leak
 * (the cell is exempt from sweep forever).  Walks the all-cells sidecar. */
size_t
urbi_gc_count_pinned(UVM *vm)
{
    size_t n = 0;
    UAllCellsNode *node = gc_node_head(vm);
    while (node != NULL) {
        if ((node->cell->gc_byte & UGC_IS_PINNED) != 0u) n++;
        node = node->next;
    }
    return n;
}
#endif /* URBI_MEM_DEBUG */
