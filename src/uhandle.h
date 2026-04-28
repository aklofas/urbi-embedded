/* SPDX-License-Identifier: BSD-3-Clause */
/* Host-handle table — opaque uint32_t handles to UValues, per row 10 §5.6.
 *
 * Handles are 1-indexed (0 is URBI_HANDLE_INVALID).  The table grows on
 * demand via vm->alloc_fn.  Slots are "released" by writing nil in-place;
 * slot reuse via free-list is deferred to v1.x.
 *
 * host_handle_walk_roots is registered as a GC root provider by uvm_init
 * (T26 wired the call; T27 provides the real implementation). */

#ifndef UHANDLE_H
#define UHANDLE_H

#include <stdint.h>
#include "umodule.h"   /* UValue */
#include "ugc.h"       /* UGcRootCallback */

struct UVM;

#define URBI_HANDLE_INVALID  0u
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
 * Registered with urbi_gc_register_root_provider in uvm_init (T26 site).
 * Not ISR-safe. */
void     host_handle_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx);

#endif /* UHANDLE_H */
