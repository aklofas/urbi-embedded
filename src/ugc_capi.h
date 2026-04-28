/* SPDX-License-Identifier: BSD-3-Clause */
/* GC strategy-dispatch router.
 *
 * Selects the active GC strategy header based on URBI_GC, then declares
 * the strategy-neutral interface that all callers use.
 *
 * uvm.h includes this file so that all inline barrier helpers are available
 * throughout the interpreter without additional explicit includes. */

#ifndef UGC_CAPI_H
#define UGC_CAPI_H

#include "ugc.h"

#if URBI_GC == URBI_GC_INCREMENTAL
#  include "ugc_incremental.h"
#elif URBI_GC == URBI_GC_NONE
#  include "ugc_none.h"
#else
#  error "URBI_GC set to unknown value"
#endif

/* === Default feature flags ===
 * Strategy headers may #define these before ugc_capi.h is processed to
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
 * header included above.  ugc_capi.h does not re-declare them to avoid
 * duplicate-declaration warnings. */

/* Non-inline op forward declarations (defined in ugc_incremental.c at T23): */
UCell *urbi_gc_alloc(struct UVM *vm, size_t size, uint8_t type_tag);
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

#endif /* UGC_CAPI_H */
