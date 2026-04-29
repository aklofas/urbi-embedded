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
#include "uvm.h"
#include "urbi/urbi.h"
#include "umacros.h"

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
    size_t              size;
    struct UAllCellsNode *next;
    struct UAllCellsNode *next_gray;  /* T24: gray work-list link; NULL when not on gray queue */
} UAllCellsNode;

/* Accessor: recover the sidecar head from vm->all_cells_head (which stores
 * UAllCellsNode* cast to UCell*).  All internal traversals use this. */
static UAllCellsNode *gc_node_head(UVM *vm) {
    return (UAllCellsNode *)(void *)vm->all_cells_head;
}

/* Accessor: recover the gray-list head from vm->gray_work_head.
 * T27: when sidecar disappears, vm->gray_work_head holds UCell* directly. */
static UAllCellsNode *gc_gray_head(UVM *vm) {
    return (UAllCellsNode *)(void *)vm->gray_work_head;
}

/* Set the gray-list head in vm->gray_work_head.
 * T27: when sidecar disappears, store UCell* directly. */
static void gc_set_gray_head(UVM *vm, UAllCellsNode *node) {
    vm->gray_work_head = (UCell *)(void *)node;
}

/* Accessor: recover sidecar from vm->sweep_cursor.
 * T27: when sidecar disappears, vm->sweep_cursor holds UCell* directly. */
static UAllCellsNode *gc_sweep_node(UVM *vm) {
    return (UAllCellsNode *)(void *)vm->sweep_cursor;
}

/* Accessor: recover prev-sidecar from vm->sweep_cursor_prev.
 * T27: when sidecar disappears, vm->sweep_cursor_prev holds UCell* directly. */
static UAllCellsNode *gc_sweep_node_prev(UVM *vm) {
    return (UAllCellsNode *)(void *)vm->sweep_cursor_prev;
}

/* Set vm->sweep_cursor to a sidecar node (or NULL).
 * T27: when sidecar disappears, store UCell* directly. */
static void gc_set_sweep_cursor(UVM *vm, UAllCellsNode *node) {
    vm->sweep_cursor = (UCell *)(void *)node;
}

/* Set vm->sweep_cursor_prev to a sidecar node (or NULL).
 * T27: when sidecar disappears, store UCell* directly. */
static void gc_set_sweep_cursor_prev(UVM *vm, UAllCellsNode *node) {
    vm->sweep_cursor_prev = (UCell *)(void *)node;
}

/* Zero a region without memset — keeps this TU freestanding. */
static void gc_zero(void *p, size_t n) {
    volatile unsigned char *b = (volatile unsigned char *)p;
    size_t i;
    for (i = 0u; i < n; i++) b[i] = 0u;
}

/* === Static helpers for gray work-list and all-cells traversal ===
 *
 * find_sidecar_for_cell: linear scan of the all-cells sidecar list to find
 * the sidecar node whose cell pointer matches the target cell.  O(N) at T24;
 * T27 collapse adds a back-pointer that makes this O(1).
 *
 * Returns NULL if not found (shouldn't happen in correct use). */
static UAllCellsNode *
find_sidecar_for_cell(UVM *vm, UCell *target)
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

    /* Only UVAL_CLOSURE carries a GC-managed cell pointer at M3. */
    if (slot->kind != UVAL_CLOSURE) return;
    if (slot->v.p == NULL) return;

    UCell *cell = (UCell *)(slot->v.p);

    /* Only shade if cell is white (not yet gray or black).
     * Skip already-grayed/blackened cells for idempotency. */
    if (IS_GRAY(cell) || IS_BLACK(cell)) return;

    /* Paint gray and push onto work-list via sidecar. */
    gc_shade_gray(vm, cell);
}

/* === walk_vm_globals ===
 *
 * Walks UVM-level UValues that aren't owned by any subsystem root provider.
 * Per spec §5.7: fatal_handler_value, prototypes[], error_protos[].
 * At M3 baseline these fields don't exist on UVM yet (they land at M4+);
 * this is a no-op stub.
 *
 * TODO(T26): enumerate vm->prototypes[] / vm->error_protos[] /
 * vm->fatal_handler_value when those land at M4+. */
static void
walk_vm_globals(UVM *vm, UGcRootCallback cb, void *ctx)
{
    (void)vm;
    (void)cb;
    (void)ctx;
    /* No VM-level UValue globals exist at M3. */
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
    /* Walk VM-internal globals (no-op stub at M3; T26 fills in). */
    walk_vm_globals(vm, mark_root_callback, vm);

    /* Walk all registered root providers. */
    uint8_t i;
    for (i = 0u; i < vm->root_provider_count; i++) {
        vm->root_providers[i](vm, mark_root_callback, vm);
    }

    vm->gc_phase = GC_PHASE_MARK_INCREMENTAL;
    return 1024u;  /* approximate work units for root scanning */
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
    size_t consumed = 0u;

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
        cell->gc_byte = (uint8_t)((cell->gc_byte & ~UGC_COLOR_MASK) | UGC_COLOR_BLACK);

        consumed += node->size;
    }

    if (gc_gray_head(vm) == NULL) {
        vm->gc_phase = GC_PHASE_ATOMIC_FINISH;
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
    size_t consumed = 0u;
    while (gc_gray_head(vm) != NULL) {
        /* T27: when sidecar disappears, vm->gray_work_head holds UCell* directly. */
        UAllCellsNode *node = gc_gray_head(vm);
        UCell *cell = node->cell;

        gc_set_gray_head(vm, node->next_gray);
        node->next_gray = NULL;

        const UType *t = vm->type_table[cell->type_tag];
        if (t != NULL && t->walk_payload != NULL) {
            t->walk_payload(vm, (void *)(cell + 1), mark_root_callback, vm);
        }

        cell->gc_byte = (uint8_t)((cell->gc_byte & ~UGC_COLOR_MASK) | UGC_COLOR_BLACK);
        consumed += node->size;
    }

    /* Gray work-list should be fully drained before SWEEP. */
    URBI_INTERNAL_ASSERT(gc_gray_head(vm) == NULL);

    /* Transition to SWEEP; initialise sweep cursor to start of all-cells list. */
    vm->gc_phase = GC_PHASE_SWEEP;
    gc_set_sweep_cursor(vm, gc_node_head(vm));
    gc_set_sweep_cursor_prev(vm, NULL);

    /* Return accumulated consumed bytes; if gray list was empty, return
     * a small constant so slice loop progresses. */
    return consumed > 0u ? consumed : 64u;
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

    if (live == 0u) {
        threshold = (size_t)URBI_GC_INITIAL_THRESHOLD;
    } else {
        threshold = (live * (size_t)URBI_GC_PAUSE_RATIO) / 100u;
        if (threshold < (size_t)URBI_GC_INITIAL_THRESHOLD) {
            threshold = (size_t)URBI_GC_INITIAL_THRESHOLD;
        }
    }

    vm->gc_threshold = threshold;
    vm->gc_debt      = -(int64_t)threshold;
    vm->gc_pending   = 0u;
}

/* === gc_sweep_step ===
 *
 * SWEEP phase: walk the all-cells sidecar list starting from the current
 * sweep cursor.  For each cell:
 *   - IS_DEAD (color == OTHER_WHITE): unlink sidecar, run finalizer if set,
 *     free cell + sidecar.  Increment consumed.
 *   - UGC_IS_FIXED (pool-managed): re-paint to current_white; advance;
 *     accumulate to surviving_bytes.
 *   - UGC_IS_PINNED (host-pinned): re-paint to current_white; advance;
 *     accumulate to surviving_bytes.
 *   - All others: re-paint to current_white; advance; accumulate.
 *
 * Surviving-bytes accumulation is stored locally and written to
 * vm->gc_live_bytes only when the sweep completes (cursor reaches end).
 * Because slices may resume, the accumulator must be re-derived from
 * scratch on each slice entry by scanning from sweep_cursor — simpler
 * than serialising partial sums and correct because freed cells are
 * already unlinked.
 *
 * Returns bytes of work consumed (used by urbi_gc_slice budget tracking). */
static size_t
gc_sweep_step(UVM *vm, size_t budget)
{
    size_t consumed  = 0u;
    size_t surviving = 0u;

    /* Re-derive surviving bytes for cells already processed before this
     * slice by walking from the list head to the current cursor.  This
     * accounts for surviving cells freed in previous slices being
     * unlinked and thus absent from the list. */
    {
        UAllCellsNode *scan = gc_node_head(vm);
        UAllCellsNode *stop = gc_sweep_node(vm);
        while (scan != NULL && scan != stop) {
            /* All nodes before sweep_cursor have already been processed
             * (survived and re-painted).  Accumulate their sizes. */
            surviving += scan->size;
            scan = scan->next;
        }
    }

    UAllCellsNode *prev = gc_sweep_node_prev(vm);
    UAllCellsNode *cur  = gc_sweep_node(vm);

    while (cur != NULL && consumed < budget) {
        UAllCellsNode *next = cur->next;
        UCell         *cell = cur->cell;

        /* FIXED and PINNED cells are exempt from collection per spec §3.6.
         * Check these flags BEFORE IS_DEAD: even if a fixed/pinned cell's
         * color didn't get updated by the mark phase (because no root
         * registered it as reachable), it must not be freed.  Re-paint it
         * to current_white so it survives further cycles too. */
        if ((cell->gc_byte & (UGC_IS_FIXED | UGC_IS_PINNED)) != 0u) {
            cell->gc_byte = (uint8_t)((cell->gc_byte & ~UGC_COLOR_MASK) | vm->current_white);
            surviving += cur->size;
            consumed  += cur->size;
            prev = cur;
            cur  = next;

        } else if (IS_DEAD(vm, cell)) {
            /* Dead cell: unlink from all-cells list and free. */

            /* Unlink sidecar. */
            if (prev == NULL) {
                /* cur was the head. */
                vm->all_cells_head = (UCell *)(void *)next;
            } else {
                prev->next = next;
            }

            /* Run finalizer if registered. */
            if ((cell->gc_byte & UGC_HAS_FINALIZER) != 0u) {
                const UType *t = vm->type_table[cell->type_tag];
                if (t != NULL && t->destroy != NULL) {
                    vm->in_destroy_callback = 1u;
                    t->destroy(vm, (void *)(cell + 1));
                    vm->in_destroy_callback = 0u;
                }
            }

            consumed += cur->size;

            /* Free cell, then sidecar. */
            vm->alloc_fn(cell, 0u, vm->alloc_ud);
            vm->alloc_fn(cur,  0u, vm->alloc_ud);

            /* prev stays unchanged; cur moves to next. */
            cur = next;

        } else {
            /* Live cell (marked black): re-paint to current_white. */
            cell->gc_byte = (uint8_t)((cell->gc_byte & ~UGC_COLOR_MASK) | vm->current_white);
            surviving += cur->size;
            consumed  += cur->size;
            prev = cur;
            cur  = next;
        }
    }

    /* Save updated cursors. */
    gc_set_sweep_cursor_prev(vm, prev);
    gc_set_sweep_cursor(vm, cur);

    if (cur == NULL) {
        /* Sweep complete: update live-bytes and trigger threshold update. */
        vm->gc_live_bytes = surviving;
        vm->gc_phase      = GC_PHASE_IDLE;
        end_of_cycle_threshold_update(vm);
    }

    return consumed;
}

/* === urbi_gc_init ===
 *
 * uvm_init() already zero-initialises every GC field added at T4/T22, so
 * urbi_gc_init only needs to set fields whose correct initial value is NOT
 * zero: gc_threshold and gc_debt.  All pointer and flag fields are already
 * NULL / 0 after uvm_init's zero pass.
 *
 * Called from uvm_init() after all other field zero-init. */
void
urbi_gc_init(UVM *vm)
{
    URBI_ASSERT_NOT_ISR(vm);

    /* Fields already zero-init by uvm_init:
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
 * Called from uvm_destroy() as the last subsystem teardown step, after
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
        if ((cell->gc_byte & UGC_HAS_FINALIZER) != 0u) {
            const UType *t = vm->type_table[cell->type_tag];
            if (t != NULL && t->destroy != NULL) {
                vm->in_destroy_callback = 1u;
                /* Payload starts immediately after UCell header.
                 * At M3 no concrete cell types exist, so this path is
                 * unreachable in practice; provided for correctness at T27+. */
                t->destroy(vm, (void *)(cell + 1));
                vm->in_destroy_callback = 0u;
            }
        }

        /* Free the cell. */
        vm->alloc_fn(cell, 0u, vm->alloc_ud);

        /* Free the sidecar node itself. */
        vm->alloc_fn(node, 0u, vm->alloc_ud);

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

    /* Allocate the cell. */
    UCell *cell = (UCell *)vm->alloc_fn(NULL, size, vm->alloc_ud);
    if (UNLIKELY(cell == NULL)) return NULL;

    /* Zero-init the cell (no memset — freestanding). */
    gc_zero(cell, size);

    /* Allocate the sidecar node. */
    UAllCellsNode *node = (UAllCellsNode *)vm->alloc_fn(
            NULL, sizeof(UAllCellsNode), vm->alloc_ud);
    if (UNLIKELY(node == NULL)) {
        vm->alloc_fn(cell, 0u, vm->alloc_ud);
        return NULL;
    }

    /* Initialize cell header. */
    cell->type_tag = type_tag;
    cell->gc_byte  = vm->current_white;   /* born current_white per spec §3.5 */

    /* Initialize sidecar node and prepend to all-cells list. */
    node->cell = cell;
    node->size = size;
    node->next = gc_node_head(vm);
    node->next_gray = NULL;
    /* Store sidecar head as UCell* (cast convention documented at top of file). */
    vm->all_cells_head = (UCell *)(void *)node;

    /* Accounting. */
    vm->gc_total_allocated += size;
    vm->gc_debt += (int64_t)size;

    /* Trigger: if debt turns positive and GC is not paused, request a slice. */
    if (UNLIKELY(vm->gc_debt > 0 && !vm->gc_paused)) {
        vm->gc_pending = 1u;
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
    cell->gc_byte = (uint8_t)((cell->gc_byte & ~UGC_COLOR_MASK) | UGC_COLOR_GRAY);

    /* Find sidecar — O(N) at T24.
     * T27: replace with back-pointer lookup once sidecar disappears. */
    UAllCellsNode *node = find_sidecar_for_cell(vm, cell);
    if (node == NULL) return;  /* shouldn't happen; defensive guard */

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

    size_t consumed = 0u;

    while (consumed < byte_budget) {
        switch (vm->gc_phase) {

        case GC_PHASE_IDLE:
            if (vm->gc_debt > 0 && !vm->gc_paused) {
                /* Start a new collection cycle: flip current_white so that
                 * all existing cells (born in the previous white) become
                 * "other white" and are treated as dead unless re-marked. */
                vm->current_white ^= 0x01u;
                vm->gc_phase = GC_PHASE_MARK_ROOTS;
            } else {
                /* Nothing to do. */
                vm->gc_pending = 0u;
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
        vm->current_white ^= 0x01u;
        vm->gc_phase = GC_PHASE_MARK_ROOTS;
    }

    /* Run slices until IDLE.  Use SIZE_MAX budget per slice to complete
     * each phase in one shot. */
    while (vm->gc_phase != GC_PHASE_IDLE) {
        urbi_gc_slice(vm, (size_t)-1u);
    }
}

/* === urbi_gc_walk_roots ===
 *
 * Walks VM globals (stub at M3) then iterates all registered root providers.
 * Provided for host/test use and called indirectly via gc_mark_roots_step. */
void
urbi_gc_walk_roots(UVM *vm, UGcRootCallback cb, void *ctx)
{
    URBI_ASSERT_NOT_ISR(vm);
    /* Walk VM-internal globals (no-op stub at M3). */
    walk_vm_globals(vm, cb, ctx);
    /* Iterate registered root providers. */
    uint8_t i;
    for (i = 0u; i < vm->root_provider_count; i++) {
        vm->root_providers[i](vm, cb, ctx);
    }
}

/* === urbi_gc_register_root_provider ===
 *
 * Appends provider to the VM's fixed root-provider array.
 * URBI_MAX_ROOT_PROVIDERS is 8 (row 10 §5.1); capacity assertion fires on
 * overflow so the programmer knows to raise the constant. */
void
urbi_gc_register_root_provider(UVM *vm, UGcRootProviderFn provider)
{
    URBI_ASSERT_NOT_ISR(vm);
    URBI_INTERNAL_ASSERT(vm->root_provider_count < (uint8_t)URBI_MAX_ROOT_PROVIDERS);
    vm->root_providers[vm->root_provider_count++] = provider;
}

size_t
urbi_gc_bytes_allocated_inline(UVM *vm)
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
    vm->gc_paused = paused ? 1u : 0u;
}

/* === Read-only GC query functions ===
 *
 * These read simple VM fields.  They are NOT ISR-safe per se (reading a
 * size_t / uint64 across an ISR boundary is not guaranteed atomic on all
 * platforms), but they are safe for diagnostic use from non-ISR contexts.
 * URBI_ASSERT_NOT_ISR is omitted — they are test/diagnostic accessors. */

size_t
urbi_gc_bytes_allocated(UVM *vm)
{
    return vm->gc_total_allocated;
}

size_t
urbi_gc_live_bytes(UVM *vm)
{
    return vm->gc_live_bytes;
}

size_t
urbi_gc_threshold(UVM *vm)
{
    return vm->gc_threshold;
}

uint8_t
urbi_gc_phase(UVM *vm)
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
uvalue_is_heap_white(UVM *vm, UValue v)
{
    if (!uvalue_is_heap(v)) return false;
    UCell *c = uvalue_as_cell(v);
    if (c == NULL) return false;
    return (c->gc_byte & UGC_COLOR_MASK) == vm->current_white;
}
