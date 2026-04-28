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

#include "ugc_incremental.h"
#include "ugc_capi.h"
#include "uvm.h"

/* No <stdlib.h> or <string.h> — freestanding-strict like every other src/*.c.
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
} UAllCellsNode;

/* Accessor: recover the sidecar head from vm->all_cells_head (which stores
 * UAllCellsNode* cast to UCell*).  All internal traversals use this. */
static UAllCellsNode *gc_node_head(UVM *vm) {
    return (UAllCellsNode *)(void *)vm->all_cells_head;
}

/* Zero a region without memset — keeps this TU freestanding. */
static void gc_zero(void *p, size_t n) {
    volatile unsigned char *b = (volatile unsigned char *)p;
    size_t i;
    for (i = 0u; i < n; i++) b[i] = 0u;
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
    vm->sweep_cursor      = NULL;
    vm->sweep_cursor_prev = NULL;
    vm->gray_work_head    = NULL;
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
 * Paints a cell gray.  The gray worklist push is deferred to T24 when the
 * 5-phase state machine lands and gray_work_head has a consumer.  At T23
 * the color paint is all that matters for the invariant tests.
 *
 * T24: push onto vm->gray_work_head; gray_work_head traversal will also
 * use sidecar-based lookup or inline payload pointers depending on what T24
 * decides for the gray-next linkage (separate sidecar field is one option;
 * embedding gray_next in the sidecar struct is another). */
void
gc_shade_gray(UVM *vm, UCell *cell)
{
    (void)vm;
    cell->gc_byte = (uint8_t)((cell->gc_byte & ~UGC_COLOR_MASK) | UGC_COLOR_GRAY);
    /* TODO(T24): push onto vm->gray_work_head once the 5-phase state machine
     * is in place and gray_work_head has a consumer (mark_incremental slice). */
}

/* === Stubs for ops declared in ugc_capi.h, landing in T24/T25/T26 ===
 *
 * These stubs satisfy the linker at T23 so the full build and test suite
 * can run.  Each stub is replaced by the real implementation in its owning
 * task. */

void
urbi_gc_slice(UVM *vm, size_t byte_budget)
{
    /* T24: 5-phase state machine slice. */
    (void)vm;
    (void)byte_budget;
}

void
urbi_gc_walk_roots(UVM *vm, UGcRootCallback cb, void *ctx)
{
    /* T26: iterate registered root providers. */
    (void)vm;
    (void)cb;
    (void)ctx;
}

void
urbi_gc_register_root_provider(UVM *vm, UGcRootProviderFn provider)
{
    /* T26: append to vm->root_providers[]. */
    (void)vm;
    (void)provider;
}

void
urbi_gc_force_full(UVM *vm)
{
    /* T24: run state machine to completion. */
    (void)vm;
}

size_t
urbi_gc_bytes_allocated_inline(UVM *vm)
{
    return vm->gc_total_allocated;
}

/* === ugc.h non-inline API stubs (landing in T26/T27) === */

uint8_t
urbi_register_type(UVM *vm, const UType *type)
{
    /* T27: populate vm->type_table[type->type_tag]. */
    (void)vm;
    (void)type;
    return 0u;
}

void
urbi_gc_collect(UVM *vm)
{
    /* T24: run full GC synchronously. */
    (void)vm;
}

void
urbi_gc_pause(UVM *vm, bool paused)
{
    vm->gc_paused = paused ? 1u : 0u;
}

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

/* === ugc_capi.h pinning stubs (landing in T27) === */

#if URBI_GC_HAS_PINNING
void
urbi_pin(UVM *vm, UValue v)
{
    /* T27: set UGC_IS_PINNED on the cell referenced by v. */
    (void)vm;
    (void)v;
}

void
urbi_unpin(UVM *vm, UValue v)
{
    /* T27: clear UGC_IS_PINNED on the cell referenced by v. */
    (void)vm;
    (void)v;
}
#endif

/* === uvalue_is_heap_white stub (landing in T25) === */

bool
uvalue_is_heap_white(UVM *vm, UValue v)
{
    /* T25: check UValue tag for heap-bearing kind, then check cell color. */
    (void)vm;
    (void)v;
    return false;
}
