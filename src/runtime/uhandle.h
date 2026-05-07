/* SPDX-License-Identifier: BSD-3-Clause */
/* Host-handle table — opaque uint32_t handles to UValues, per row 10 §5.6.
 *
 * Handles are 1-indexed (0 is URBI_HANDLE_INVALID).  The table grows on
 * demand via vm->alloc_fn.  Slots are "released" by writing nil in-place;
 * slot reuse via free-list is deferred to v1.x.
 *
 * host_handle_walk_roots is registered as a GC root provider by urbi_vm_init
 * (T26 wired the call; T27 provides the real implementation).
 *
 * Allocator contract (FOUND-003, v0.5.5): grow uses vm->alloc_fn with the
 * (ptr != NULL, nbytes > 0) realloc form — the canonical contract is the
 * UVMAllocFn typedef in <urbi/types.h>.  Required behaviour:
 *   - On success, return a buffer of the new size; the existing prefix
 *     (the old capacity worth of UValue slots) is preserved bit-identically.
 *   - On OOM, return NULL AND leave the original buffer intact and valid.
 *     uhandle_create propagates the OOM as URBI_HANDLE_INVALID; the table
 *     stays at the old capacity and can continue serving requests up to it.
 * Allocators that move/free the original on OOM (non-conforming) will
 * corrupt the live handle table.  Hosted-build wrappers around stdlib
 * realloc are conforming on all v1.0 targets. */

#ifndef UHANDLE_H
#define UHANDLE_H

#include <stdint.h>
#include "module/umodule.h"   /* UValue */
#include "gc/ugc.h"       /* UGcRootCallback */

struct UVM;

#define URBI_HANDLE_INVALID  0U
typedef uint32_t UHandle;

/* Create a handle for value v.  Returns URBI_HANDLE_INVALID on OOM.
 * The table grows as needed.  Not ISR-safe. */
UHandle  urbi_handle_create(struct UVM *vm, UValue v);

/* Retrieve the value stored under handle h.
 * Returns a nil UValue if h is URBI_HANDLE_INVALID or out of range. */
UValue   urbi_handle_get(struct UVM *vm, UHandle h);

/* Release handle h, writing nil into the slot.
 * No-op if h is URBI_HANDLE_INVALID or out of range.  Not ISR-safe. */
void     urbi_handle_release(struct UVM *vm, UHandle h);

/* GC root provider: walks all non-nil slots and calls cb for each.
 * Registered with urbi_gc_register_root_provider in urbi_vm_init (T26 site).
 * Not ISR-safe. */
void     host_handle_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx);

#endif /* UHANDLE_H */
