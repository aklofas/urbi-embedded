/* SPDX-License-Identifier: BSD-3-Clause */
/* src/watcher/uwatcher_state.c — UWatcherState lifecycle.
 * Watcher substate extracted from struct UVM per audit-1 F8
 * (v0.10.4-vm-decomp Wave 5 W2). */

#include "watcher/uwatcher_state.h"
#include "vm/uvm.h"
#include "runtime/umacros.h"   /* urbi_zero */

/* uwatcher_state_create: allocate and zero-initialize UWatcherState.
 * Pool storage is NOT allocated here — that is handled by uwatcher_pool_init
 * (uwatcher.c), which runs after this call and writes pool_base / pool_freelist
 * directly.  This function only allocates the containing struct.
 * Returns NULL on OOM. */
UWatcherState *
uwatcher_state_create(struct UVM *vm)
{
    UWatcherState *ws;

    if (vm->alloc_fn == NULL) return NULL;

    ws = (UWatcherState *)vm->alloc_fn(NULL, sizeof(UWatcherState), vm->alloc_ud);
    if (ws == NULL) return NULL;

    urbi_zero(ws, sizeof(UWatcherState));
    return ws;
}

/* uwatcher_state_destroy: free UWatcherState struct memory.
 * Does NOT free the watcher pool slab — that is owned by uwatcher_pool_destroy
 * (uwatcher.c), which runs before urbi_vm_destroy frees vm->watchers. */
void
uwatcher_state_destroy(struct UVM *vm, UWatcherState *ws)
{
    if (ws == NULL) return;
    if (vm->alloc_fn == NULL) return;
    vm->alloc_fn(ws, 0, vm->alloc_ud);
}
