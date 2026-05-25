/* SPDX-License-Identifier: BSD-3-Clause */
/* GC strategy-dispatch router.
 *
 * Stability: core.
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
/* Pin / unpin a UValue against GC sweep.
 *
 * urbi_pin: set UGC_IS_PINNED on the cell so it is exempt from sweep.
 * urbi_unpin: clear UGC_IS_PINNED so the cell is eligible for collection.
 *
 * Both are no-ops for non-heap UValues (NIL/INT/FLOAT/BOOL/STR/VOID — the
 * tag carries the value directly so there is no cell to pin).  Caller-owned
 * `vm` and `v`; nothing escapes.  v1.0 ships single-bit pin (idempotent set
 * and clear); v1.x adds a refcount table for nested pin/unpin pairs.
 *
 * Not ISR-safe (URBI_ASSERT_NOT_ISR fires in URBI_DEBUG builds). */
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
 *   size_t urbi_gc_bytes_allocated_inline(const struct UVM *vm);
 *
 * Barrier surfaces (always inline, defined in ugc_incremental.h; renamed at
 * v0.10.1 per runtime-invariants F12 to make barrier+store atomic by default):
 *
 *   static inline void urbi_gc_slot_store(
 *       struct UVM *vm, UCell *parent, uint32_t key,
 *       UValue *dst, UValue child);            -- combined barrier + store (preferred)
 *
 *   static inline void urbi_gc_slot_pre_store(
 *       struct UVM *vm, UCell *parent, uint32_t key, UValue child);
 *                                                -- barrier-only; caller stores
 *
 *   static inline void urbi_gc_register_write(
 *       struct UVM *vm, struct UStrand *s, uint16_t reg_idx, UValue child);
 *
 *   static inline void urbi_gc_upvalue_pre_store(
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
 * docs/milestones/m3-concurrency.md for the end-to-end pause-time history.
 *
 * ISR note: NOT ISR-safe — allocates/frees memory and modifies VM state. */
void   urbi_gc_slice(struct UVM *vm, size_t byte_budget);

/* Iterate every registered root provider, invoking cb(vm, slot, ctx) once
 * per UValue root reached.  Provided for host/test use; the GC mark phase
 * also calls this indirectly through gc_mark_roots_step.  VM-level globals
 * are reached through providers — there is no separate VM-globals walk.
 * cb and ctx are caller-owned and must remain valid for the duration of
 * the call.  Not ISR-safe. */
void   urbi_gc_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx);

/* Append `provider` to the VM's fixed root-provider array (capacity
 * URBI_MAX_ROOT_PROVIDERS = 12 per row 10 §5.1; bumped from 8 at Step C-1).  The provider function
 * pointer is borrowed: callee retains it for the lifetime of `vm`, and
 * caller must keep the underlying code object alive at least that long.
 * URBI_INTERNAL_ASSERT fires on overflow.  Not ISR-safe. */
void   urbi_gc_register_root_provider(struct UVM *vm, UGcRootProviderFn provider);

/* Initialize the GC fields on a fresh UVM.  Called from urbi_vm_init after
 * its zero-init pass; sets the only fields whose correct initial value is
 * NOT zero (gc_threshold, gc_debt).  Caller owns `vm`; no allocation.
 * Not ISR-safe. */
void   urbi_gc_init(struct UVM *vm);

/* Tear down the GC: walks the all-cells sidecar list and frees every cell
 * via the VM allocator (ignoring UGC_IS_FIXED / UGC_IS_PINNED — at teardown
 * everything goes).  Cells with UGC_HAS_FINALIZER trigger their type's
 * destroy callback before free.  Must be the LAST subsystem teardown step
 * in urbi_vm_destroy, after every other subsystem that allocates GC cells
 * has dropped its references.  Caller owns `vm`.  Not ISR-safe. */
void   urbi_gc_destroy(struct UVM *vm);

/* Run the GC state machine to completion synchronously.  If currently IDLE,
 * starts a new cycle (flips current_white, enters MARK_ROOTS).  Runs slices
 * with SIZE_MAX budget per call until back to IDLE.  Intended for tests and
 * explicit-collection requests; NOT for production MCU use where bounded
 * pauses matter.  Caller owns `vm`.  Not ISR-safe. */
void   urbi_gc_force_full(struct UVM *vm);

/* Read total GC-tracked bytes allocated since VM creation (vm->gc_total_
 * allocated).  Pure read of a single size_t field; safe to call from any
 * non-ISR context.  Used by test harnesses + the determinism checksum. */
size_t urbi_gc_bytes_allocated_inline(const struct UVM *vm);

/* === Root provider forward declarations (T26) ===
 * Each subsystem's root-walker function is declared here so that urbi_vm_init
 * can register them without pulling in each subsystem's full header.
 * Definitions live in their respective source files.
 *
 * Note: host_handle_walk_roots is declared in uhandle.h (T27 moved it there
 * to give the host-handle subsystem a proper home).  uvm.c includes uhandle.h
 * directly when registering the provider. */

#endif /* URBI_GC_H */
