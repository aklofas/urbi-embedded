/* SPDX-License-Identifier: BSD-3-Clause */
/* GC strategy-dispatch router.
 *
 * Selects the active GC strategy header based on URBI_GC, then declares
 * the strategy-neutral interface that all callers use.
 *
 * uvm.h includes this file so that all inline barrier helpers are available
 * throughout the interpreter without additional explicit includes. */

#ifndef URBI_GC_H
#define URBI_GC_H

#include "gc/ugc.h"

#if URBI_GC == URBI_GC_INCREMENTAL
#  include "gc/ugc_incremental.h"
#elif URBI_GC == URBI_GC_NONE
#  include "gc/ugc_none.h"
#else
#  error "URBI_GC set to unknown value"
#endif

/* === Default feature flags ===
 * Strategy headers may #define these before urbi/gc.h is processed to
 * override the defaults; the #ifndef guards here ensure strategy-set values
 * are not overwritten. */
#ifndef URBI_GC_HAS_GENERATIONS
#  define URBI_GC_HAS_GENERATIONS  0
#endif
#ifndef URBI_GC_HAS_FINALIZERS
#  define URBI_GC_HAS_FINALIZERS   1
#endif
#ifndef URBI_GC_HAS_WEAK_REFS
#  define URBI_GC_HAS_WEAK_REFS    0   /* always 0 at v1 */
#endif
#ifndef URBI_GC_HAS_PINNING
#  define URBI_GC_HAS_PINNING      1
#endif
#ifndef URBI_GC_HAS_ARENAS
#  define URBI_GC_HAS_ARENAS       0
#endif
#ifndef URBI_GC_INCREMENTAL_BARRIER
#  define URBI_GC_INCREMENTAL_BARRIER  1
#endif
#ifndef URBI_GC_HEADER_BYTES
#  define URBI_GC_HEADER_BYTES     2
#endif

#if URBI_GC_HAS_PINNING
void urbi_pin(struct UVM *vm, UValue v);
void urbi_unpin(struct UVM *vm, UValue v);
#endif

/* === Strategy interface — 8 non-inline ops + 3 barrier surfaces ===
 *
 * Non-inline ops (T23 provides implementations in ugc_incremental.c):
 *
 *   UCell *urbi_gc_alloc(struct UVM *vm, size_t size, uint8_t type_tag);
 *   void   urbi_gc_slice(struct UVM *vm, size_t byte_budget);
 *   void   urbi_gc_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx);
 *   void   urbi_gc_register_root_provider(struct UVM *vm, UGcRootProviderFn provider);
 *   void   urbi_gc_init(struct UVM *vm);
 *   void   urbi_gc_destroy(struct UVM *vm);
 *   void   urbi_gc_force_full(struct UVM *vm);
 *   size_t urbi_gc_bytes_allocated_inline(struct UVM *vm);
 *
 * Three barrier surfaces (always inline, defined as no-op stubs in
 * ugc_incremental.h; T25 lands the real Dijkstra forward-barrier logic):
 *
 *   static inline void urbi_gc_slot_write(
 *       struct UVM *vm, UCell *parent, uint32_t key, UValue child);
 *
 *   static inline void urbi_gc_register_write(
 *       struct UVM *vm, struct UStrand *s, uint16_t reg_idx, UValue child);
 *
 *   static inline void urbi_gc_upvalue_write(
 *       struct UVM *vm, struct UClosure *closure, uint8_t up_idx, UValue child);
 *
 * These ops are declared (non-inline) and defined (inline) in the strategy
 * header included above.  urbi/gc.h does not re-declare them to avoid
 * duplicate-declaration warnings. */

/* Default per-slice byte-work budget for urbi_gc_slice().
 * MCU default; Linux builds can override with -DURBI_GC_SLICE_BUDGET=16384.
 * Tunable per row 10 §6.5 of the M3 incremental GC design. */
#ifndef URBI_GC_SLICE_BUDGET
#  define URBI_GC_SLICE_BUDGET 4096
#endif

/* Non-inline op forward declarations (defined in ugc_incremental.c at T23): */

/* Allocate a new GC-managed cell of the given size and type_tag.
 * The cell is born at the current white color per spec §3.5.
 * Increments gc_debt and sets gc_pending if debt turns positive and GC is
 * not paused.  Does NOT advance the collector (no slice is run here).
 * Returns NULL on OOM. */
UCell *urbi_gc_alloc(struct UVM *vm, size_t size, uint8_t type_tag);

/* Advance the incremental GC by approximately byte_budget bytes of work.
 * Called from the dispatcher when gc_pending != 0 (cooperative safepoints).
 *
 * Termination behavior per phase:
 *   IDLE            — returns immediately if nothing to collect; starts a
 *                     new cycle (→ MARK_ROOTS) and falls through if debt > 0.
 *   MARK_ROOTS      — runs one full root-scan step; returns once complete
 *                     (ignores remaining budget for this phase).
 *   MARK_INCREMENTAL — drains the gray work-list up to the remaining budget;
 *                      transitions to ATOMIC_FINISH when work-list is empty.
 *   ATOMIC_FINISH   — runs one bounded stop-the-world re-scan step; returns
 *                     once complete (ignores remaining budget for this phase).
 *   SWEEP           — walks all-cells list up to the remaining budget; returns
 *                     early (→ IDLE) once the cycle completes.
 *
 * Step budget: incremental work is bounded so that any single
 * urbi_gc_slice() call returns within ~1ms on the host CI runner.
 * Verified by `make test-gc-pause` CI gate.  See M3 retrospective at
 * docs/milestones/m3-concurrency.md (workspace-root) for the
 * end-to-end pause-time history.
 *
 * ISR note: NOT ISR-safe — allocates/frees memory and modifies VM state. */
void   urbi_gc_slice(struct UVM *vm, size_t byte_budget);

void   urbi_gc_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx);
void   urbi_gc_register_root_provider(struct UVM *vm, UGcRootProviderFn provider);
void   urbi_gc_init(struct UVM *vm);
void   urbi_gc_destroy(struct UVM *vm);
void   urbi_gc_force_full(struct UVM *vm);
size_t urbi_gc_bytes_allocated_inline(struct UVM *vm);

/* === Root provider forward declarations (T26) ===
 * Each subsystem's root-walker function is declared here so that uvm_init
 * can register them without pulling in each subsystem's full header.
 * Definitions live in their respective source files.
 *
 * Note: host_handle_walk_roots is declared in uhandle.h (T27 moved it there
 * to give the host-handle subsystem a proper home).  uvm.c includes uhandle.h
 * directly when registering the provider. */

#endif /* URBI_GC_H */
