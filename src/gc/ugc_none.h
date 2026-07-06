/* SPDX-License-Identifier: BSD-3-Clause */
/* URBI_GC_NONE — zero-overhead GC stub configuration.
 *
 * URBI_GC_NONE is a compile-only configuration providing zero-overhead GC
 * stubs; gated by `make test-gc-none-build` smoke build (in releasetest).
 * Functional GC for embedded targets is URBI_GC_INCREMENTAL
 * (see ugc_incremental.{c,h}).
 *
 * This header satisfies the strategy-router include path so that the
 * URBI_GC_NONE compile-smoke build can verify the strategy interface contract
 * holds at the header level.  A real heap-freeze allocator (fixed arena, no
 * sweep, destroy on urbi_vm_destroy) is deferred to v2.
 *
 * All barriers are no-op static inlines.  All allocator ops are declared as
 * no-op stubs; linking against a real URBI_GC_NONE library is deferred to v2.
 * The GC-NONE tier freeze strategy is documented in the GC design (Row 10 §2.1).
 */

#ifndef UGC_NONE_H
#define UGC_NONE_H

#include "ugc.h"

/* === Feature-flag overrides ===
 * URBI_GC_NONE has no GC-managed cells, no pinning, no finalizer tracking,
 * and no incremental barrier.
 *
 * include/urbi/gc.h now defines default values for these macros via
 * #ifndef guards.  When ugc_none.h is included after urbi/gc.h (the correct
 * order for internal callers who include both), the defaults are already set.
 * #undef + redefine is the standard C idiom for intentional override.  The
 * compiler may emit a "macro redefined" warning with -Wundef-redef or similar
 * flags, but -Wpedantic itself does not fire on #undef+redefine (only on
 * plain redefinition without an intervening #undef). */
#undef  URBI_GC_HAS_FINALIZERS
#define URBI_GC_HAS_FINALIZERS       0
#undef  URBI_GC_HAS_PINNING
#define URBI_GC_HAS_PINNING          0
#undef  URBI_GC_INCREMENTAL_BARRIER
#define URBI_GC_INCREMENTAL_BARRIER  0

/* === gc_byte layout — no color tracking under NONE === */
#define UGC_COLOR_MASK           0x00U
#define UGC_HAS_FINALIZER        0x00U
#define UGC_IS_PINNED            0x00U
#define UGC_IS_FIXED             0x00U
#define UGC_HAS_WATCHER_OBSERVER 0x00U
#define UGC_HAS_SLOT_CHANGE_EVENT 0x00U
#define UGC_IS_WEAK              0x00U  /* URBI_GC_NONE: no GC => no weak references. */

/* === Phase constant — NONE is permanently IDLE (no cycles) === */
#define GC_PHASE_IDLE            0

/* === UNLIKELY hint — plain expression under NONE (no hot-path barriers) === */
#ifndef UNLIKELY
#  define UNLIKELY(x) (x)
#endif

/* Forward declarations for pointer types used in barrier signatures.
 * (struct UClosure dropped at Task 9c — mirrors ugc_incremental.h.) */
struct UVM;
struct UStrand;

/* === Barrier surfaces — all no-op static inlines under URBI_GC_NONE ===
 *
 * Under URBI_GC_NONE there is no incremental barrier; writes are always
 * safe.  The inlines expand to nothing and the compiler removes them.
 * API mirrors ugc_incremental.h (runtime-invariants F12). */

/* urbi_gc_slot_pre_store — barrier-only stub (no-op under NONE). */
static inline void
urbi_gc_slot_pre_store(struct UVM *vm, UCell *parent,
                       uint32_t key, UValue child)
{
    (void)vm; (void)parent; (void)key; (void)child;
}

/* urbi_gc_slot_store — combined barrier + store.  Under NONE, just stores. */
static inline void
urbi_gc_slot_store(struct UVM *vm, UCell *parent, uint32_t key,
                   UValue *dst, UValue child)
{
    (void)vm; (void)parent; (void)key;
    *dst = child;
}

static inline void
urbi_gc_register_write(struct UVM *vm, struct UStrand *s,
                       uint16_t reg_idx, UValue child)
{
    (void)vm; (void)s; (void)reg_idx; (void)child;
}

/* urbi_gc_upvalue_pre_store — barrier-only stub (no-op under NONE).
 * Task 9c: parent retargeted closure → UUpvalCell header (matches the
 * incremental strategy's signature). */
static inline void
urbi_gc_upvalue_pre_store(struct UVM *vm, const UCell *cell, UValue child)
{
    (void)vm; (void)cell; (void)child;
}

#endif /* UGC_NONE_H */
