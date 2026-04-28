/* SPDX-License-Identifier: BSD-3-Clause */
/* GC umbrella header: common UCell/UType definitions, build-flag values,
 * type-tag constants, and non-inline GC C API declarations.
 *
 * Include hierarchy:
 *   ugc.h          <- this file (common defs + non-inline API)
 *   ugc_capi.h     <- strategy-dispatch router; includes ugc.h + strategy header
 *   ugc_incremental.h <- URBI_GC_INCREMENTAL strategy (gc_byte layout + barriers)
 *
 * uvm.h includes ugc_capi.h so that inline barrier helpers are visible
 * throughout the interpreter.  DO NOT include uvm.h from this file (circular). */

#ifndef UGC_H
#define UGC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "umodule.h"   /* UValue typedef — needed by callback signatures */

/* === Build-flag values === */

#define URBI_GC_INCREMENTAL    1   /* Ships M3 (default) */
#define URBI_GC_NONE           2   /* Spec-only at M3, ships v2 */
#define URBI_GC_GENERATIONAL   3   /* RESERVED — v1.x */
#define URBI_GC_ARENA_PER_TAG  4   /* RESERVED — v1.x design / v2 ship */

#ifndef URBI_GC
#  define URBI_GC URBI_GC_INCREMENTAL
#endif

/* Forward declarations — defined in uvm.h / ustrand.h / uframe.h.
 * Referenced only as pointers here to avoid circular includes. */
struct UVM;

/* === GC root provider callbacks ===
 * Declared here (not in uvm.h) so ugc_capi.h and ugc_incremental.h can use them
 * without pulling in the full UVM struct definition.
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
 * UGcWalkPayloadFn: per-type precise scanner stored in UType.walk_payload.
 *   Called during the mark phase to shade all UValues reachable from a cell's
 *   payload.  payload is the raw bytes immediately after the UCell header. */
typedef void (*UGcRootCallback)(struct UVM *vm, UValue *root, void *ctx);
typedef void (*UGcRootProviderFn)(struct UVM *vm, UGcRootCallback cb, void *ctx);
typedef void (*UGcWalkPayloadFn)(struct UVM *vm, void *payload,
                                 UGcRootCallback cb, void *ctx);

/* === UCell common header (2 bytes shared across all strategies) === */

typedef struct UCell {
    uint8_t  type_tag;          /* offset 0 */
    uint8_t  gc_byte;           /* offset 1: strategy-private (see ugc_incremental.h) */
    /* offset 2..7: padding absorbed by first payload field's natural alignment */
} UCell;

/* === UType — per-type-tag descriptor ===
 *
 * One UType is registered per type_tag value.  The VM keeps a 256-entry
 * type_table[] indexed by type_tag.  Entries are populated by built-in init
 * code (T27) and by urbi_register_type() for host types.
 *
 * flags field bits: */
#define TYPE_HAS_FINALIZER  0x01   /* destroy != NULL and should be called */
#define TYPE_HOST_BACKED    0x02   /* payload references host-owned memory */

typedef void (*UTypeDestroyFn)(struct UVM *vm, void *payload);

typedef struct UType {
    uint8_t           type_tag;
    uint8_t           flags;
    uint16_t          payload_size;        /* fixed payload bytes; 0 = variable */
    const char       *name;
    UGcWalkPayloadFn  walk_payload;        /* precise scan; NULL = leaf (no refs) */
    UTypeDestroyFn    destroy;             /* finalizer; NULL if none */
    /* M4 will add prototype + methods here */
} UType;

/* === Type-tag enum (extended at M4 for object model) === */

#define UTYPE_OBJECT     1
#define UTYPE_CLOSURE    2
#define UTYPE_STRING     3   /* M2 baseline */
#define UTYPE_ARRAY      4   /* M4 lists */
#define UTYPE_TAG        5   /* M3 row 11 */
#define UTYPE_WATCHER    6   /* M3 row 11 */
#define UTYPE_COROUTINE  7   /* row 9 */
#define UTYPE_NAMESPACE  8   /* M3 row 8 */
#define UTYPE_HOST_BASE  64  /* host-registered types start here */
#define UTYPE_HOST_MAX   255

/* === Non-inline GC C API ===
 * Inline ops (alloc, slice, walk_roots, register_root_provider, init, destroy,
 * force_full, bytes_allocated_inline, and the three barrier surfaces) are
 * declared in ugc_capi.h via the strategy header. */

/* Register a built-in or host type descriptor with the VM.
 * Returns the assigned type_tag on success (always == type->type_tag).
 * Returns 0 on error (tag already occupied, or host-type table full). */
uint8_t urbi_register_type(struct UVM *vm, const UType *type);

/* Trigger a full synchronous GC collection (all phases in one call).
 * Intended for testing and shutdown; not for production use on MCUs. */
void   urbi_gc_collect(struct UVM *vm);

/* Pause / resume incremental GC slices.  While paused, urbi_gc_slice() is a
 * no-op.  urbi_gc_collect() still works (explicit override). */
void   urbi_gc_pause(struct UVM *vm, bool paused);

/* Query GC accounting (reads directly from vm fields; no locks). */
size_t urbi_gc_bytes_allocated(struct UVM *vm);
size_t urbi_gc_live_bytes(struct UVM *vm);
size_t urbi_gc_threshold(struct UVM *vm);
uint8_t urbi_gc_phase(struct UVM *vm);

#if URBI_GC_HAS_PINNING
void urbi_pin(struct UVM *vm, UValue v);
void urbi_unpin(struct UVM *vm, UValue v);
#endif

#endif /* UGC_H */
