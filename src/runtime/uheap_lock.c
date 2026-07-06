/* SPDX-License-Identifier: BSD-3-Clause */
/* src/runtime/uheap_lock.c
 *
 * Phase 13: urbi_lock_heap public C API.
 *
 * One-way latch that flips vm->heap_locked.  Subsequent urbi_gc_alloc
 * calls observe the flag and decline to allocate new GC-managed cells
 * (returning NULL — the standard OOM-shaped failure mode the rest of
 * the runtime already handles).
 *
 * Reserved for v2.0 hard-RT mode where post-init allocation is
 * forbidden by policy.  Surface lands at v1.0; the enforcement is
 * opt-in — embedders that want lifetime allocation simply never call.
 *
 * No unlock primitive at v1.0 (one-way matches the hard-RT contract).
 */

#include "urbi/urbi.h"
#include "vm/uvm.h"

void
urbi_lock_heap(UVM *vm)
{
    if (vm == NULL) return;
    vm->heap_locked = 1U;
}
