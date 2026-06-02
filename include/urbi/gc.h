/* SPDX-License-Identifier: BSD-3-Clause */
/* Public GC interface.
 *
 * Stability: core (non-inline ops); advanced (urbi_gc_alloc / urbi_gc_slice).
 *
 * Declares the strategy-neutral GC C API that external embedders use:
 * opaque UCell typedef, root-provider callback signatures, build-flag
 * constants, feature-flag defaults, and the 8 non-inline ops.
 *
 * Strategy-specific inline barrier helpers (urbi_gc_slot_store,
 * urbi_gc_slot_pre_store, urbi_gc_register_write, urbi_gc_upvalue_pre_store)
 * are NOT declared here.  Internal src/ callers include the strategy header
 * directly (e.g. "gc/ugc_incremental.h" with -Isrc).  External embedders
 * that write C extensions mutating UCell fields directly must compile with
 * -Isrc and include the strategy header themselves; this is intentional and
 * documented in docs/embedding-guide.md §advanced-barriers.
 *
 * === W2: public-header de-leak ===
 * Before v0.10.3, this header included "gc/ugc.h" and the active strategy
 * header ("gc/ugc_incremental.h" or "gc/ugc_none.h") — all src/-prefixed
 * paths that caused header-not-found errors for embedders using -Iinclude
 * alone.  W2 moves all public-facing declarations inline so that -Iinclude
 * alone is sufficient.  Closes audit-1 F1 (completion).
 * === end W2 === */

#ifndef URBI_GC_H
#define URBI_GC_H

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility push(default)   /* v1.0: export only public-header symbols */
#endif

#include <stddef.h>
#include <stdint.h>

#include "urbi/types.h"    /* UValue — needed by root-callback signatures */
#include "urbi/version.h"  /* URBI_ADVANCED */

#ifdef __cplusplus
extern "C" {
#endif

/* === Build-flag values (strategy selector) ===
 * Mirrors src/gc/ugc.h numeric values; both must stay in sync. */
#define URBI_GC_INCREMENTAL    1   /* Incremental tri-color mark-sweep (default) */
#define URBI_GC_NONE           2   /* Compile-smoke stub; real impl deferred to v2 */
#define URBI_GC_GENERATIONAL   3   /* RESERVED — v1.x */
#define URBI_GC_ARENA_PER_TAG  4   /* RESERVED — v1.x / v2 */

#ifndef URBI_GC
#  define URBI_GC URBI_GC_INCREMENTAL
#endif

/* === Default GC feature flags ===
 * Strategy headers (src/gc/ugc_incremental.h, src/gc/ugc_none.h) may
 * override these before a TU sees the #ifndef guards by #define-ing the
 * macro before including this header.  For external embedders that never
 * include a strategy header directly, these defaults reflect the
 * URBI_GC_INCREMENTAL baseline. */
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

/* Default per-slice byte-work budget for urbi_gc_slice().
 * MCU default; Linux builds can override with -DURBI_GC_SLICE_BUDGET=16384.
 * Tunable per row 10 §6.5 of the M3 incremental GC design. */
#ifndef URBI_GC_SLICE_BUDGET
#  define URBI_GC_SLICE_BUDGET 4096
#endif

/* === Root-provider callback signatures ===
 *
 * UGcRootCallback: called once per GC root during a root walk.
 *   vm   — the owning VM.
 *   root — pointer to the UValue slot holding the root (writable for relocation).
 *   ctx  — opaque caller cookie.
 *
 * UGcRootProviderFn: registered via urbi_gc_register_root_provider().
 *   The GC calls each registered provider at the start of each mark phase;
 *   the provider must call cb(vm, &slot, ctx) for every live root it owns.
 *
 * UValue is defined in urbi/types.h (included above).
 *
 * URBI_GC_ROOT_CALLBACK_DEFINED guards against -Wpedantic redefinition when
 * src/gc/ugc.h is also included in the same TU. */
#ifndef URBI_GC_ROOT_CALLBACK_DEFINED
#  define URBI_GC_ROOT_CALLBACK_DEFINED 1
typedef void (*UGcRootCallback)(struct UVM *vm, UValue *root, void *ctx);
typedef void (*UGcRootProviderFn)(struct UVM *vm, UGcRootCallback cb, void *ctx);
#endif

/* === end W2: public-header de-leak === */

/* Opaque GC-cell type — full layout (type_tag, gc_byte, payload) is in
 * src/gc/ugc.h for internal callers only.  External embedders use
 * struct UCell* pointers and never access fields directly. */
struct UCell;

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

/* === Strategy interface — 8 non-inline ops ===
 *
 * Non-inline ops (T23 provides implementations in ugc_incremental.c):
 *
 *   struct UCell *urbi_gc_alloc(struct UVM *vm, size_t size, uint8_t type_tag);
 *   void   urbi_gc_slice(struct UVM *vm, size_t byte_budget);
 *   void   urbi_gc_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx);
 *   void   urbi_gc_register_root_provider(struct UVM *vm, UGcRootProviderFn provider);
 *   void   urbi_gc_init(struct UVM *vm);
 *   void   urbi_gc_destroy(struct UVM *vm);
 *   void   urbi_gc_force_full(struct UVM *vm);
 *   size_t urbi_gc_bytes_allocated_inline(const struct UVM *vm);
 *
 * Barrier surfaces (always inline, defined in src/gc/ugc_incremental.h;
 * renamed at v0.10.1 per runtime-invariants F12 to make barrier+store
 * atomic by default).  Internal src/ callers include "gc/ugc_incremental.h"
 * directly; external embedders that need barriers must compile with -Isrc.
 *
 *   urbi_gc_slot_store       -- combined barrier + store (preferred)
 *   urbi_gc_slot_pre_store   -- barrier-only; caller stores
 *   urbi_gc_register_write   -- register-slot barrier
 *   urbi_gc_upvalue_pre_store -- upvalue barrier */

/* Allocate a new GC-managed cell of the given size and type_tag.
 * The cell is born at the current white color per spec §3.5.
 * Increments gc_debt and sets gc_pending if debt turns positive and GC is
 * not paused.  Does NOT advance the collector (no slice is run here).
 * Returns NULL on OOM. */
URBI_ADVANCED struct UCell *urbi_gc_alloc(struct UVM *vm, size_t size, uint8_t type_tag);

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
URBI_ADVANCED void   urbi_gc_slice(struct UVM *vm, size_t byte_budget);

/* Iterate every registered root provider, invoking cb(vm, slot, ctx) once
 * per UValue root reached.  Provided for host/test use; the GC mark phase
 * also calls this indirectly through gc_mark_roots_step.  VM-level globals
 * are reached through providers — there is no separate VM-globals walk.
 * cb and ctx are caller-owned and must remain valid for the duration of
 * the call.  Not ISR-safe. */
URBI_ADVANCED void   urbi_gc_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx);

/* Append `provider` to the VM's fixed root-provider array (capacity
 * URBI_MAX_ROOT_PROVIDERS = 12 per row 10 §5.1; bumped from 8 at Step C-1).  The provider function
 * pointer is borrowed: callee retains it for the lifetime of `vm`, and
 * caller must keep the underlying code object alive at least that long.
 * URBI_INTERNAL_ASSERT fires on overflow.  Not ISR-safe. */
URBI_ADVANCED void   urbi_gc_register_root_provider(struct UVM *vm, UGcRootProviderFn provider);

/* Initialize the GC fields on a fresh UVM.  Called from urbi_vm_init after
 * its zero-init pass; sets the only fields whose correct initial value is
 * NOT zero (gc_threshold, gc_debt).  Caller owns `vm`; no allocation.
 * Not ISR-safe. */
URBI_ADVANCED void   urbi_gc_init(struct UVM *vm);

/* Tear down the GC: walks the all-cells sidecar list and frees every cell
 * via the VM allocator (ignoring UGC_IS_FIXED / UGC_IS_PINNED — at teardown
 * everything goes).  Cells with UGC_HAS_FINALIZER trigger their type's
 * destroy callback before free.  Must be the LAST subsystem teardown step
 * in urbi_vm_destroy, after every other subsystem that allocates GC cells
 * has dropped its references.  Caller owns `vm`.  Not ISR-safe. */
URBI_ADVANCED void   urbi_gc_destroy(struct UVM *vm);

/* Run the GC state machine to completion synchronously.  If currently IDLE,
 * starts a new cycle (flips current_white, enters MARK_ROOTS).  Runs slices
 * with SIZE_MAX budget per call until back to IDLE.  Intended for tests and
 * explicit-collection requests; NOT for production MCU use where bounded
 * pauses matter.  Caller owns `vm`.  Not ISR-safe. */
URBI_ADVANCED void   urbi_gc_force_full(struct UVM *vm);

/* Read total GC-tracked bytes allocated since VM creation (vm->gc_total_
 * allocated).  Pure read of a single size_t field; safe to call from any
 * non-ISR context.  Used by test harnesses + the determinism checksum. */
URBI_ADVANCED size_t urbi_gc_bytes_allocated_inline(const struct UVM *vm);

/* === Root provider forward declarations (T26) ===
 * Each subsystem's root-walker function is declared here so that urbi_vm_init
 * can register them without pulling in each subsystem's full header.
 * Definitions live in their respective source files.
 *
 * Note: host_handle_walk_roots is declared in uhandle.h (T27 moved it there
 * to give the host-handle subsystem a proper home).  uvm.c includes uhandle.h
 * directly when registering the provider. */

#ifdef __cplusplus
}
#endif


#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility pop
#endif
#endif /* URBI_GC_H */
