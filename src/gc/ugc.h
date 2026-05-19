/* SPDX-License-Identifier: BSD-3-Clause */
/* GC umbrella header: common UCell/UType definitions, build-flag values,
 * type-tag constants, and non-inline GC C API declarations.
 *
 * Include hierarchy:
 *   ugc.h             <- this file (common defs + non-inline API)
 *   urbi/gc.h         <- strategy-dispatch router; includes ugc.h + strategy header
 *   ugc_incremental.h <- URBI_GC_INCREMENTAL strategy (gc_byte layout + barriers)
 *
 * uvm.h includes urbi/gc.h so that inline barrier helpers are visible
 * throughout the interpreter.  DO NOT include uvm.h from this file (circular). */

#ifndef UGC_H
#define UGC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "chunk/uchunk.h"   /* UValue typedef — needed by callback signatures */

/* === Build-flag values === */

#define URBI_GC_INCREMENTAL    1   /* Ships M3 (default) */
#define URBI_GC_NONE           2   /* Compile-smoke only (see ugc_none.h); real impl deferred to v2 */
#define URBI_GC_GENERATIONAL   3   /* RESERVED — v1.x */
#define URBI_GC_ARENA_PER_TAG  4   /* RESERVED — v1.x design / v2 ship */

#ifndef URBI_GC
#  define URBI_GC URBI_GC_INCREMENTAL
#endif

/* Forward declarations — defined in src/vm/uvm.h / src/sched/ustrand.h /
 * src/runtime/uframe.h.  Referenced only as pointers here to avoid circular
 * includes. */
struct UVM;

/* === GC root provider callbacks ===
 * Declared here (not in uvm.h) so urbi/gc.h and ugc_incremental.h can use them
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
    uint8_t  type_tag;  /* offset 0 */
    uint8_t  gc_byte;   /* offset 1: strategy-private (see ugc_incremental.h) */
    /* offset 2..N-1: padding inserted by compiler; N = alignof(first payload field).
     * Concrete cell types embed UCell as their FIRST struct member — the compiler
     * handles alignment.  Do NOT use sizeof(UCell)+payload raw arithmetic. */
} UCell;

/* === UType — per-type-tag descriptor ===
 *
 * One UType is registered per type_tag value.  The VM keeps a 256-entry
 * type_table[] indexed by type_tag.  Entries are populated by built-in init
 * code (T27) and by urbi_register_type() for host types.
 *
 * flags field bits: */
#define TYPE_HAS_FINALIZER  0x01   /* destroy != NULL and should be called */

typedef void (*UTypeDestroyFn)(struct UVM *vm, void *payload);

typedef struct UType {
    uint8_t           type_tag;
    uint8_t           flags;
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
/* M4 object-model tags (UTYPE_OBJECT above is reused; not re-reserved). */
#define UTYPE_PROTOS            9   /* M4 — heap UProtos block (n >= 2) */
#define UTYPE_SHAPE            10   /* M4 — UShape hidden class */
#define UTYPE_PROPS            11   /* M4 — UProps slot-property record */
#define UTYPE_SLOTHANDLE       12   /* M4 — slot handle (later task) */
#define UTYPE_MODULE_INSTANCE  13   /* M4 — module instance (later task) */
#define UTYPE_PROTO_INSTANCE   14   /* M4 — proto-bound instance (later task) */
#define UTYPE_SHAPE_MAP        15   /* M4 — UShapeMap transition cache (T13) */
#define UTYPE_PROPS_TABLE      16   /* M4 — UPropsTable (per-shape UProps* array, T17) */
#define UTYPE_SLOT_ARRAY       17   /* M4 — USlotArray wrapper (UObject's grow-on-write slot storage, T26) */
#define UTYPE_EVENT            18   /* M5 — UEvent reactive cell (spec #3 §3.1) */
#define UTYPE_CHANGED_NODE     19   /* M5 — UChangedNode slot-change subscriber (spec #4 §3.1) */
#define UTYPE_UPVAL_CELL       20   /* v0.8.4 — captured-local upvalue cell (closure-lifetime spec Piece C) */
/* Tags 21-63 are RESERVED for future runtime expansion (v1.x/v2.0 GC cell
 * types such as UString heap cells, UArray, UDict, UWeakRef, UFiber, etc.).
 * Do not assign host types here; use the host range below.
 *
 * Runtime tags claimed so far (1-20):
 *   1  UTYPE_OBJECT            9  UTYPE_PROTOS
 *   2  UTYPE_CLOSURE          10  UTYPE_SHAPE
 *   3  UTYPE_STRING           11  UTYPE_PROPS
 *   4  UTYPE_ARRAY            12  UTYPE_SLOTHANDLE
 *   5  UTYPE_TAG              13  UTYPE_MODULE_INSTANCE
 *   6  UTYPE_WATCHER          14  UTYPE_PROTO_INSTANCE
 *   7  UTYPE_COROUTINE        15  UTYPE_SHAPE_MAP
 *   8  UTYPE_NAMESPACE        16  UTYPE_PROPS_TABLE
 *                             17  UTYPE_SLOT_ARRAY
 *                             18  UTYPE_EVENT
 *                             19  UTYPE_CHANGED_NODE
 *                             20  UTYPE_UPVAL_CELL */
#define UTYPE_HOST_BASE  64  /* host-registered types start here (64-255) */
#define UTYPE_HOST_MAX   255

/* === Non-inline GC C API ===
 * Inline ops (alloc, slice, walk_roots, register_root_provider, init, destroy,
 * force_full, bytes_allocated_inline, and the three barrier surfaces) are
 * declared in urbi/gc.h via the strategy header. */

/* Register a host type descriptor with the VM.
 * Precondition: type->type_tag must be 0 (auto-assign) or >= UTYPE_HOST_BASE (64).
 *   Tags 1..(UTYPE_HOST_BASE-1) are reserved for built-in runtime types;
 *   passing one of those tags triggers URBI_INTERNAL_ASSERT in debug builds
 *   and returns 0 in release builds.
 * Returns the assigned type_tag on success (== type->type_tag for explicit
 * tags; next auto-slot for tag == 0).
 * Returns 0 on error (tag conflict, runtime-reserved tag, or table full).
 * See src/value/utype.c for the implementation. */
uint8_t urbi_register_type(struct UVM *vm, const UType *type);

/* Trigger a full synchronous GC collection (all phases in one call).
 * Intended for testing and shutdown; not for production use on MCUs. */
void   urbi_gc_collect(struct UVM *vm);

/* Declared in urbi/gc.h (strategy-router header) — reproduced here for
 * subsystems that include ugc.h directly:
 *
 * void urbi_gc_destroy(struct UVM *vm);
 *
 * Ordering constraint: must be called AFTER all subsystems that hold
 * GC-managed cells have been torn down.  At M5 the required order is:
 *   1. urealm_teardown_all()  — releases Realm bindings (GC-managed values)
 *   2. uwatcher_pool_destroy() — frees the watcher pool slab before GC
 *   3. urbi_gc_destroy()      — frees all remaining GC cells + sidecar list
 * Remaining VM infrastructure (event ring, sched queues, deferred
 * slot-change ring, handle table) does NOT own GC cells and is freed
 * after urbi_gc_destroy in urbi_vm_destroy.  See src/vm/uvm.c:urbi_vm_destroy. */

/* Pause / resume incremental GC slices.  While paused, urbi_gc_slice() is a
 * no-op.  urbi_gc_collect() still works (explicit override). */
void   urbi_gc_pause(struct UVM *vm, bool paused);

/* Query GC accounting (reads directly from vm fields; no locks). */
size_t urbi_gc_bytes_allocated(const struct UVM *vm);
size_t urbi_gc_live_bytes(const struct UVM *vm);
size_t urbi_gc_threshold(const struct UVM *vm);
uint8_t urbi_gc_phase(const struct UVM *vm);

#endif /* UGC_H */
