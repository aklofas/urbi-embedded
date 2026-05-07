/* SPDX-License-Identifier: BSD-3-Clause */
/* URBI_GC_INCREMENTAL strategy header.
 *
 * Provides:
 *   - gc_byte bit-layout macros (row 10 §3.1)
 *   - Two-white tri-color scheme macros (row 10 §3.2)
 *   - GC phase constants (row 10 §6.1)
 *   - Compile-time tunables (row 10 §6.5)
 *   - UNLIKELY branch-prediction hint
 *   - Real Dijkstra forward-barrier implementations of the three inline barrier surfaces
 *   - Forward declarations for gc_shade_gray and observer_dirty
 *
 * Included by urbi/gc.h when URBI_GC == URBI_GC_INCREMENTAL.
 * Do NOT include uvm.h from this file (would be circular). */

#ifndef UGC_INCREMENTAL_H
#define UGC_INCREMENTAL_H

#include "ugc.h"

/* Forward declarations for pointer types used in barrier signatures.
 * Full definitions live in src/sched/ustrand.h / src/runtime/uframe.h /
 * src/module/umodule.h. */
struct UVM;
struct UStrand;
struct UClosure;

/* === gc_byte bit layout (row 10 §3.1) ===
 *
 * All 8 bits of gc_byte are claimed; their owners are:
 *
 * Bits [1:0] — tri-color marking color (UGC_COLOR_MASK):
 *   WHITE0 (0x00) — dead or unvisited; "current dead" when == current_white
 *   WHITE1 (0x01) — dead or unvisited; alternate white (rotates each cycle)
 *   GRAY   (0x02) — discovered, children not yet scanned
 *   BLACK  (0x03) — fully scanned in current cycle
 * Bit 2  — UGC_HAS_FINALIZER: mirrors UType.flags & TYPE_HAS_FINALIZER for
 *           the hot-path sweep check; avoids a type_table dereference.
 * Bit 3  — UGC_IS_WEAK: RESERVED for v1.x weak-reference support (backlog).
 *           Always 0 at v0.5.x; do not set.
 * Bit 4  — UGC_IS_PINNED: cell is exempt from sweep (host-pinned value).
 * Bit 5  — UGC_IS_FIXED: pool-managed cell; never freed by GC sweep.
 * Bit 6  — UGC_HAS_WATCHER_OBSERVER: object has at least one watcher in the
 *           read-set; triggers observer_dirty() in urbi_gc_slot_write().
 *           Maintained at row 10/11 boundary (T33).
 * Bit 7  — UGC_HAS_SLOT_CHANGE_EVENT: at least one slot on this UObject has
 *           a slot-change subscriber (spec #4 §3.4); post-store hook in
 *           urbi_gc_slot_write() fires the deferred emit ring. */

#define UGC_COLOR_MASK            0x03
#define   UGC_COLOR_WHITE0          0x00
#define   UGC_COLOR_WHITE1          0x01
#define   UGC_COLOR_GRAY            0x02
#define   UGC_COLOR_BLACK           0x03

#define UGC_HAS_FINALIZER         0x04
#define UGC_IS_WEAK               0x08    /* RESERVED: v1.x weak-reference support (not implemented at v0.5.x) */
#define UGC_IS_PINNED             0x10
#define UGC_IS_FIXED              0x20    /* pool-managed; never swept */
#define UGC_HAS_WATCHER_OBSERVER  0x40    /* row 10/11 boundary; T33 maintains */
#define UGC_HAS_SLOT_CHANGE_EVENT 0x80   /* spec #4 §3.4: at least one slot on
                                          * this UObject has a slot-change
                                          * subscriber; post-store hook in
                                          * urbi_gc_slot_write fires the
                                          * deferred emit ring. */

/* === Two-white scheme (row 10 §3.2) ===
 *
 * vm->current_white alternates between UGC_COLOR_WHITE0 (0) and UGC_COLOR_WHITE1 (1)
 * at the end of each sweep.  A cell is "dead" (unreachable in the last completed
 * cycle) if its color matches the *other* white.
 *
 * IS_WHITE covers both white values — a cell is white if it hasn't been marked
 * gray or black in the current cycle. */

#define OTHER_WHITE(vm)     ((vm)->current_white ^ 0x01)
#define IS_DEAD(vm, cell)   (((cell)->gc_byte & UGC_COLOR_MASK) == OTHER_WHITE(vm))
#define IS_BLACK(cell)      (((cell)->gc_byte & UGC_COLOR_MASK) == UGC_COLOR_BLACK)
#define IS_WHITE(cell)      (((cell)->gc_byte & UGC_COLOR_MASK) <= UGC_COLOR_WHITE1)
#define IS_GRAY(cell)       (((cell)->gc_byte & UGC_COLOR_MASK) == UGC_COLOR_GRAY)

/* === GC phase constants (row 10 §6.1) ===
 *
 * Stored in vm->gc_phase (uint8_t).
 * Transitions: IDLE → MARK_ROOTS → MARK_INCREMENTAL → ATOMIC_FINISH → SWEEP → IDLE */

#define GC_PHASE_IDLE             0
#define GC_PHASE_MARK_ROOTS       1
#define GC_PHASE_MARK_INCREMENTAL 2
#define GC_PHASE_ATOMIC_FINISH    3
#define GC_PHASE_SWEEP            4

/* === Compile-time tunables (row 10 §6.5) ===
 *
 * URBI_GC_PAUSE_RATIO: threshold = live_bytes * PAUSE_RATIO / 100.
 *   200 → threshold = 2× live (allocate up to 2× live before next cycle).
 *
 * URBI_GC_INITIAL_THRESHOLD: initial gc_threshold value set by urbi_gc_init().
 *   Canonical definition lives here (row 10 §6.5); uvm.h carries a parallel
 *   #ifndef guard kept as a dead-path safeguard against double-definition.
 *
 * URBI_GC_SLICE_BUDGET: defined in urbi/gc.h (visible to all callers of
 *   urbi_gc_slice, internal and external). */

#ifndef URBI_GC_PAUSE_RATIO
#  define URBI_GC_PAUSE_RATIO        200
#endif
#ifndef URBI_GC_INITIAL_THRESHOLD
#  define URBI_GC_INITIAL_THRESHOLD  (16 * 1024)
#endif

/* === UNLIKELY branch-prediction hint ===
 * Used in hot-path barrier checks; falls back to plain expression on MSVC. */

#ifndef UNLIKELY
#  if defined(__GNUC__) || defined(__clang__)
#    define UNLIKELY(x) __builtin_expect(!!(x), 0)
#  else
#    define UNLIKELY(x) (x)
#  endif
#endif

#ifndef LIKELY
#  if defined(__GNUC__) || defined(__clang__)
#    define LIKELY(x) __builtin_expect(!!(x), 1)
#  else
#    define LIKELY(x) (x)
#  endif
#endif

/* === gc_shade_gray — mark a cell gray and push onto the worklist ===
 * T23/T24: defined in ugc_incremental.c */
void gc_shade_gray(struct UVM *vm, UCell *cell);

/* === urbi_gc_walk_all_cells — generic all-cells iterator (T12) ===
 *
 * Calls cb(vm, cell, ctx) once for every live GC cell on vm->all_cells_head.
 * Iteration order is unspecified (matches sweep order — O(n) over the sidecar
 * list).  Cells freed during the walk would corrupt iteration; cb must not
 * trigger urbi_gc_alloc / urbi_gc_collect / sweep work.  cb may mutate
 * cell->gc_byte / type-private payload bytes safely.
 *
 * Used by urbi_object_lookup_id_force_wrap (T12) to clear UObject.lookup_stamp
 * bytes on u32 rollover.  T36 may fold this into the mark phase to avoid the
 * separate pass; until then this iterator is the load-bearing surface.
 *
 * Internal API — the UAllCellsNode sidecar layout is private to
 * ugc_incremental.c, so direct iteration outside that TU is not possible. */
typedef void (*UGcCellCallback)(struct UVM *vm, UCell *cell, void *ctx);
void urbi_gc_walk_all_cells(struct UVM *vm, UGcCellCallback cb, void *ctx);

/* === observer_dirty — watcher dirty-set hook ===
 * Defined in src/uwatcher.c.  Increments vm->watcher_dirty_count; the
 * scheduler calls watcher_eval_dirty (T34) on the next safepoint turn. */
void observer_dirty(struct UVM *vm, UCell *cell, uint32_t key);

/* === uvalue_is_heap / uvalue_as_cell ===
 *
 * Heap-bearing UValKinds at M4: UVAL_CLOSURE (UClosure*) and UVAL_OBJECT
 * (UObject*).  Both store a pointer in v.v.p; both embed UCell as the
 * first struct member, so the cast in uvalue_as_cell is well-defined.
 *
 * Other kinds (NIL, INT, FLOAT, BOOL, STR, VOID, STRAND) are either inline
 * scalars or not GC-managed via UCell.  STRAND deliberately skipped: M3
 * strands are sched-managed, not GC cells.
 *
 * TODO(M5+): extend for UVAL_STRING (when strings move to heap), UVAL_ARRAY,
 * UVAL_TAG, UVAL_WATCHER once those UValKinds exist.
 * M5: UVAL_EVENT added (UEvent embeds UCell at offset 0). */

static inline bool
uvalue_is_heap(UValue v)
{
    return v.kind == UVAL_CLOSURE || v.kind == UVAL_OBJECT || v.kind == UVAL_EVENT;
}

static inline UCell *
uvalue_as_cell(UValue v)
{
    /* Caller must have checked uvalue_is_heap(v) first.
     * UVAL_CLOSURE stores a UClosure* in v.v.p; UVAL_OBJECT stores a
     * UObject* in v.v.p.  Both structs embed UCell as their first member
     * at offset 0 (see uclosure.h, object/uobject.h), so this cast is
     * well-defined for real heap values as well as synthetic UCell
     * objects allocated via urbi_gc_alloc. */
    return (UCell *)v.v.p;
}

/* === uvalue_is_heap_white ===
 *
 * Two-step check: (a) UValue tag indicates heap-bearing kind (via uvalue_is_heap);
 * (b) cell color matches current_white.  When (a) fails, (b) short-circuits.
 *
 * Declared here as a non-inline extern so that the barrier static-inline bodies
 * above can call it using only the forward-declared struct UVM *.
 * ugc_incremental.h cannot include uvm.h (circular: uvm.h -> urbi/gc.h ->
 * ugc_incremental.h), so the full struct UVM definition is not available here.
 * The inline barriers only need the function signature; the linker resolves the
 * call to ugc_incremental.c where uvm.h is fully included.
 *
 * Defined in ugc_incremental.c (T25). */
bool uvalue_is_heap_white(struct UVM *vm, UValue v);

/* === Three inline barrier surfaces ===
 *
 * urbi_gc_slot_write:
 *   Called when assigning a UValue to an object slot or Realm namespace binding.
 *   Implements a forward (Dijkstra) write barrier: if the parent cell is black
 *   and the child value is a white heap cell, shade the child gray to preserve
 *   the tri-color invariant.  Also hooks the watcher dirty-set (row 10/11
 *   boundary) when bit 6 is set on the parent.
 *   parent  — the containing cell (must already be GC-managed).
 *   key     — slot index or namespace key (for observer_dirty at T34).
 *   child   — the new value being stored.
 *   NOTE: the barrier is a hook; the actual store is the CALLER'S responsibility.
 *
 * urbi_gc_register_write:
 *   Called when the dispatch loop writes a UValue into a strand register
 *   (OP_MOVE, arithmetic results, OP_LOADK, etc.).
 *   Registers are roots walked at every mark phase via gc_walk_roots → strand
 *   walker, so no barrier is needed: conceptually registers are "always gray".
 *   s       — the strand whose register file is being updated.
 *   reg_idx — register index within s->R[].
 *   child   — the value being stored.
 *
 * urbi_gc_upvalue_write:
 *   Called when OP_SETUPVAL stores a value through a closure's upvalue chain.
 *   Applies the GC barrier (same Dijkstra logic as slot_write); no watcher
 *   hook because closures are not directly observable by watchers in v1.
 *   closure — the closure owning the upvalue.
 *   up_idx  — index into closure->upvals[].
 *   child   — the value being stored.
 *   NOTE: the actual upvalue store is the CALLER'S responsibility.
 *
 * Callsite status (M4):
 *   OP_SETUPVAL handler (src/uvm.c): UClosure embeds UCell as first member at
 *   offset 0 (see uclosure.h); urbi_gc_upvalue_write is wired before the store.
 *   The barrier may safely cast UClosure* → UCell* for the color check.
 *
 *   unamespace_set (src/realm/urealm_namespace.c): UNamespace still lacks a
 *   UCell header at this commit.  Wire urbi_gc_slot_write when UNamespace
 *   migrates to a UCell-headed cell (later M4 task).
 *
 *   OP_SETSLOT (M4 reserved): dormant; wired alongside full IC support. */

static inline void
urbi_gc_slot_write(struct UVM *vm, UCell *parent, uint32_t key, UValue child)
{
    uint8_t parent_gc = parent->gc_byte;

    /* (1) GC barrier: forward Dijkstra ("shade the target").
     * If parent is black and child is a white heap cell, paint child gray
     * to maintain the no-black-to-white invariant. */
    if (UNLIKELY((parent_gc & UGC_COLOR_MASK) == UGC_COLOR_BLACK
                 && uvalue_is_heap_white(vm, child))) {
        gc_shade_gray(vm, uvalue_as_cell(child));
    }

    /* (2) Watcher dirty-set hook.
     * observer_dirty (src/uwatcher.c) bumps vm->watcher_dirty_count;
     * the scheduler calls watcher_eval_dirty on the next safepoint turn.
     * This strategy header is always compiled with URBI_GC_INCREMENTAL, so
     * the watcher hook is always present (no #if guard needed here). */
    if (UNLIKELY(parent_gc & UGC_HAS_WATCHER_OBSERVER)) {
        observer_dirty(vm, parent, key);
    }

    /* (3) Actual store — caller's responsibility.
     * The barrier is a hook only; callers must perform the store themselves
     * immediately after calling urbi_gc_slot_write. */
}

static inline void
urbi_gc_register_write(struct UVM *vm, struct UStrand *s, uint16_t reg_idx, UValue child)
{
    /* No GC barrier on register writes — registers are roots, walked at every
     * mark phase via gc_walk_roots → strand walker.  The mark phase sees
     * current register state; no parent-color check needed. */

    /* No watcher dirty-set: registers are not watched directly (watchers read
     * slots/Realm bindings, not VM-internal registers). */

    (void)vm; (void)s; (void)reg_idx; (void)child;
}

static inline void
urbi_gc_upvalue_write(struct UVM *vm, struct UClosure *closure, uint8_t up_idx, UValue child)
{
    UCell *parent = (UCell *)closure;
    uint8_t parent_gc = parent->gc_byte;

    /* GC barrier: forward Dijkstra — same logic as slot_write.
     * UClosure embeds UCell at offset 0 (M4 — see uclosure.h), so the
     * cast above yields a valid header pointer. */
    if (UNLIKELY((parent_gc & UGC_COLOR_MASK) == UGC_COLOR_BLACK
                 && uvalue_is_heap_white(vm, child))) {
        gc_shade_gray(vm, uvalue_as_cell(child));
    }

    /* No watcher hook on upvalue writes: closures are not directly observable
     * by watchers in v1 (no first-class "watch this closure's upvalue" surface). */
    (void)up_idx;
}

#endif /* UGC_INCREMENTAL_H */
