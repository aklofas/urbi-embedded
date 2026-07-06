/* SPDX-License-Identifier: BSD-3-Clause */
/* src/watcher/uwatcher_state.h — UWatcherState: watcher substate extracted
 * from struct UVM per audit-1 F8 (v0.10.4-vm-decomp Wave 5 W2). */

#ifndef URBI_WATCHER_STATE_H
#define URBI_WATCHER_STATE_H

#include <stdint.h>

struct UVM;
struct UWatcher;

/* === W2/v0.10.4: UWatcherState — extracted from struct UVM per audit-1 F8 === */
typedef struct UWatcherState {
    /* Liveness counters */
    uint32_t active_count;       /* was vm->watcher_active_count */
    uint32_t dirty_count;        /* was vm->watcher_dirty_count */

    /* Watcher pool */
    struct UWatcher *pool_base;       /* was vm->watcher_pool_base */
    struct UWatcher *pool_freelist;   /* was vm->watcher_pool_freelist */
    uint16_t pool_in_use;        /* was vm->watcher_pool_in_use */
    uint16_t pool_high_water;    /* was vm->watcher_pool_high_water */

    /* Re-entry guards */
    uint8_t  in_eval;            /* was vm->in_watcher_eval */
    uint8_t  in_scratch;         /* was vm->in_watcher_scratch */
    uint8_t  in_install;         /* was vm->in_watcher_install */

    /* SCHED-02 storm guard: when 1, urbi_vm_watcher_eval_dirty fires a WHENEVER only on
     * the rising edge (false->true) instead of level-triggered (every truthy
     * pass).  Set transiently by vm_reactive_drain's idle/boundary path
     * (bounded=1) so a self-re-dirtying level-whenever cannot spin while the VM
     * is otherwise quiescent; the active-dispatch drains leave it 0 so the
     * documented level-trigger semantics (and the existing whenever fixtures)
     * are preserved on the safepoint path.  Save/restore around the eval. */
    uint8_t  whenever_edge_only;

    /* Pass-generation counter for rescan idempotency (PENDING-cascade fix).
     * Incremented once at the top of urbi_vm_watcher_eval_dirty (wrap-around safe:
     * comparison uses ==).  Each UWatcher carries a matching eval_pass_gen
     * stamp; watchers already evaluated in the current pass are skipped on
     * rescan to prevent level-triggered WHENEVER double-fire. */
    uint8_t  eval_pass_gen;
} UWatcherState;

/* uwatcher_state_create: allocate and zero-initialize a UWatcherState.
 * Pool storage (pool_base / pool_freelist) is NOT allocated here — that
 * is done by uwatcher_pool_init (uwatcher.c).  Returns NULL on OOM. */
UWatcherState *uwatcher_state_create(struct UVM *vm);

/* uwatcher_state_destroy: free the UWatcherState struct.
 * Does NOT free the watcher pool slab — uwatcher_pool_destroy owns that.
 * NULL-tolerant. */
void           uwatcher_state_destroy(struct UVM *vm, UWatcherState *ws);

#endif /* URBI_WATCHER_STATE_H */
