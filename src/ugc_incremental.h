/* SPDX-License-Identifier: BSD-3-Clause */
/* URBI_GC_INCREMENTAL strategy header.
 *
 * Provides:
 *   - gc_byte bit-layout macros (row 10 §3.1)
 *   - Two-white tri-color scheme macros (row 10 §3.2)
 *   - GC phase constants (row 10 §6.1)
 *   - Compile-time tunables (row 10 §6.5)
 *   - UNLIKELY branch-prediction hint
 *   - No-op stub implementations of the three inline barrier surfaces (T25 fills these)
 *   - Forward declarations for gc_shade_gray and observer_dirty
 *
 * Included by ugc_capi.h when URBI_GC == URBI_GC_INCREMENTAL.
 * Do NOT include uvm.h from this file (would be circular). */

#ifndef UGC_INCREMENTAL_H
#define UGC_INCREMENTAL_H

#include "ugc.h"

/* Forward declarations for pointer types used in barrier signatures.
 * Full definitions live in ustrand.h / uframe.h / umodule.h. */
struct UVM;
struct UStrand;
struct UClosure;

/* === gc_byte bit layout (row 10 §3.1) ===
 *
 * Bits [1:0] — tri-color marking:
 *   WHITE0 / WHITE1 — dead or unvisited (which white is "current" rotates each cycle)
 *   GRAY            — discovered, children not yet scanned
 *   BLACK           — fully scanned in current cycle
 * Bit 2  — has finalizer (mirrors UType.flags & TYPE_HAS_FINALIZER for hot-path)
 * Bit 3  — weak reference (RESERVED v1.x; always 0 at M3)
 * Bit 4  — pinned (exempt from sweep)
 * Bit 5  — fixed / pool-managed (never freed by GC sweep)
 * Bit 6  — has watcher observer (read-set membership; row 10/11 boundary)
 * Bit 7  — RESERVED */

#define UGC_COLOR_MASK            0x03
#define   UGC_COLOR_WHITE0          0x00
#define   UGC_COLOR_WHITE1          0x01
#define   UGC_COLOR_GRAY            0x02
#define   UGC_COLOR_BLACK           0x03

#define UGC_HAS_FINALIZER         0x04
#define UGC_IS_WEAK               0x08    /* RESERVED v1.x */
#define UGC_IS_PINNED             0x10
#define UGC_IS_FIXED              0x20    /* pool-managed; never swept */
#define UGC_HAS_WATCHER_OBSERVER  0x40    /* row 10/11 boundary; T33 maintains */
#define UGC_RESERVED7             0x80

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
 * URBI_GC_SLICE_BUDGET: default per-slice byte-work budget.
 *   MCU default 4096; Linux builds can override with -DURBI_GC_SLICE_BUDGET=16384.
 *
 * URBI_GC_PAUSE_RATIO: threshold = live_bytes * PAUSE_RATIO / 100.
 *   200 → threshold = 2× live (allocate up to 2× live before next cycle).
 *
 * URBI_GC_INITIAL_THRESHOLD: initial gc_threshold value set by urbi_gc_init().
 *   Canonical definition lives here (row 10 §6.5); uvm.h's #ifndef guard
 *   was a placeholder that is now removed. */

#ifndef URBI_GC_SLICE_BUDGET
#  define URBI_GC_SLICE_BUDGET       4096
#endif
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

/* === uvalue_is_heap_white — forward declaration only ===
 *
 * The real implementation requires uvalue_is_heap() and uvalue_as_cell(),
 * which do not exist yet.  T25 defines this function in ugc_incremental.c
 * alongside the full barrier implementations.
 *
 * Declared here (not inline) so that ugc_incremental.h can be included
 * transitively without accidentally providing a wrong stub body. */
bool uvalue_is_heap_white(struct UVM *vm, UValue v);   /* T25: defined in ugc_incremental.c */

/* === gc_shade_gray — mark a cell gray and push onto the worklist ===
 * Defined in ugc_incremental.c (T23/T24 ship the implementation). */
void gc_shade_gray(struct UVM *vm, UCell *cell);

/* === observer_dirty — mark a cell's watcher observer dirty ===
 * Defined in ugc_incremental.c / uwatcher.c (row 11 — T33/T34). */
void observer_dirty(struct UVM *vm, UCell *cell, uint32_t key);

/* === Three inline barrier surfaces ===
 *
 * These are no-op stubs at T22.  T25 replaces each body with the real
 * Dijkstra forward-barrier logic:
 *   if (URBI_GC_INCREMENTAL_BARRIER && IS_BLACK(parent) && uvalue_is_heap_white(vm, child))
 *       gc_shade_gray(vm, uvalue_as_cell(child));
 *
 * The (void) casts suppress unused-parameter warnings in the stub bodies.
 *
 * urbi_gc_slot_write:
 *   Called when assigning a UValue to an object slot or Realm namespace binding.
 *   parent  — the containing cell (must already be GC-managed).
 *   key     — slot index or namespace key (for observer_dirty fold at T33).
 *   child   — the new value being stored.
 *
 * urbi_gc_register_write:
 *   Called when the dispatch loop writes a UValue into a strand register
 *   (OP_MOVE, arithmetic results, OP_LOADK, etc.).
 *   s       — the strand whose register file is being updated.
 *   reg_idx — register index within s->R[].
 *   child   — the value being stored.
 *
 * urbi_gc_upvalue_write:
 *   Called when OP_SETUPVAL stores a value through a closure's upvalue chain.
 *   closure — the closure owning the upvalue.
 *   up_idx  — index into closure->upvals[].
 *   child   — the value being stored. */

static inline void
urbi_gc_slot_write(struct UVM *vm, UCell *parent, uint32_t key, UValue child)
{
    (void)vm; (void)parent; (void)key; (void)child;
    /* T25: real Dijkstra forward barrier + observer_dirty fold */
}

static inline void
urbi_gc_register_write(struct UVM *vm, struct UStrand *s, uint16_t reg_idx, UValue child)
{
    (void)vm; (void)s; (void)reg_idx; (void)child;
    /* T25: shade gray if vm is in mark phase and child is heap-white */
}

static inline void
urbi_gc_upvalue_write(struct UVM *vm, struct UClosure *closure, uint8_t up_idx, UValue child)
{
    (void)vm; (void)closure; (void)up_idx; (void)child;
    /* T25: shade gray if vm is in mark phase and child is heap-white */
}

#endif /* UGC_INCREMENTAL_H */
