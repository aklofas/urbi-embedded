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
 * See docs/superpowers/specs/ Row 10 §2.1 for the freeze strategy.
 */

#ifndef UGC_NONE_H
#define UGC_NONE_H

#include "ugc.h"

/* === Feature-flag overrides ===
 * URBI_GC_NONE has no GC-managed cells, no pinning, no finalizer tracking,
 * and no incremental barrier. */
#define URBI_GC_HAS_FINALIZERS       0
#define URBI_GC_HAS_PINNING          0
#define URBI_GC_INCREMENTAL_BARRIER  0

/* === gc_byte layout — no color tracking under NONE === */
#define UGC_COLOR_MASK           0x00U
#define UGC_HAS_FINALIZER        0x00U
#define UGC_IS_PINNED            0x00U
#define UGC_IS_FIXED             0x00U
#define UGC_HAS_WATCHER_OBSERVER 0x00U
#define UGC_HAS_SLOT_CHANGE_EVENT 0x00U

/* === Phase constant — NONE is permanently IDLE (no cycles) === */
#define GC_PHASE_IDLE            0

/* === UNLIKELY hint — plain expression under NONE (no hot-path barriers) === */
#ifndef UNLIKELY
#  define UNLIKELY(x) (x)
#endif

/* Forward declarations for pointer types used in barrier signatures. */
struct UVM;
struct UStrand;
struct UClosure;

/* === Three barrier surfaces — all no-op static inlines ===
 *
 * Under URBI_GC_NONE there is no incremental barrier; writes are always
 * safe.  The inlines expand to nothing and the compiler removes them. */

static inline void
urbi_gc_slot_write(struct UVM *vm, UCell *parent,
                   uint32_t key, UValue child)
{
    (void)vm; (void)parent; (void)key; (void)child;
}

static inline void
urbi_gc_register_write(struct UVM *vm, struct UStrand *s,
                       uint16_t reg_idx, UValue child)
{
    (void)vm; (void)s; (void)reg_idx; (void)child;
}

static inline void
urbi_gc_upvalue_write(struct UVM *vm, struct UClosure *closure,
                      uint8_t up_idx, UValue child)
{
    (void)vm; (void)closure; (void)up_idx; (void)child;
}

#endif /* UGC_NONE_H */
