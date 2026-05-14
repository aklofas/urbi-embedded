/* SPDX-License-Identifier: BSD-3-Clause */
/* uwatcher_host.h — internal structures and API for host-side reactive
 * watchers (Gap J, v0.7.1).
 *
 * UHostWatcher: one slot in the per-VM host watcher table.
 * UHostWatcherTable: growable array of UHostWatcher entries owned by UVM.
 *
 * Freestanding-safe: no hosted headers included here. */

#ifndef UWATCHER_HOST_H
#define UWATCHER_HOST_H

#include <stddef.h>
#include <stdint.h>

#include "urbi/types.h"   /* urbi_event_id_t, urbi_watcher_handle_t, UValue */
#include "urbi/urbi.h"    /* urbi_watcher_fn, URBI_WATCHER_HANDLE_INVALID */

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;

/* UHostWatcher: one registered host-side watcher entry.
 *
 * Fields:
 *   handle           — non-zero, unique per-VM identity (auto-increment).
 *   event_id         — the registered event this watcher listens to.
 *   cb               — the host callback invoked at drain.
 *   ud               — user-data forwarded to cb.
 *   pending_unregister — 1 if urbi_unregister_watcher or URBI_ERR_WATCHER_UNREGISTER
 *                       requested removal; 0 otherwise.  Entries with this flag
 *                       set are skipped during dispatch and compacted at drain-end.
 *   _pad             — alignment padding; always zero. */
typedef struct {
    urbi_watcher_handle_t handle;
    urbi_event_id_t       event_id;
    urbi_watcher_fn       cb;
    void                 *ud;
    uint8_t               pending_unregister;
    uint8_t               _pad[3];
} UHostWatcher;

/* UHostWatcherTable: growable array of UHostWatcher entries.
 *
 *   entries      — heap-allocated via vm->alloc_fn; NULL until first add.
 *   count        — number of live+pending entries in entries[].
 *   capacity     — allocated slot count.
 *   next_handle  — next handle to assign; starts at 1, increments on each add.
 *                  Wraps at INT_MAX and skips 0 (INVALID); overflow is benign
 *                  on any realistic system (billions of watchers). */
typedef struct {
    UHostWatcher         *entries;
    size_t                count;
    size_t                capacity;
    urbi_watcher_handle_t next_handle;
} UHostWatcherTable;

/* uhost_watcher_table_init: zero-initialize the table.
 * Must be called before any other operation.  Does not allocate. */
void uhost_watcher_table_init(UHostWatcherTable *t);

/* uhost_watcher_table_destroy: free the entries[] array via vm->alloc_fn.
 * Safe to call on a zero-initialized table (entries == NULL). */
void uhost_watcher_table_destroy(UHostWatcherTable *t, struct UVM *vm);

/* uhost_watcher_table_add: append a new zero-initialized entry.
 * Grows entries[] (capacity doubles) when needed.
 * Returns a pointer to the new entry (caller fills cb, ud, event_id; handle
 * is pre-assigned from next_handle).
 * Returns NULL on OOM; table is left unchanged. */
UHostWatcher *uhost_watcher_table_add(UHostWatcherTable *t, struct UVM *vm);

/* uhost_watcher_table_walk_event: walk entries[] and invoke cb for each
 * entry whose event_id matches and whose pending_unregister == 0.
 *
 * Multi-arg: all argc values from args[0..argc-1] are passed to cb.  This
 * closes the Sub-Bundle 2 multi-arg deferral for the host-watcher path.
 *
 * Auto-unregister: if cb returns URBI_ERR_WATCHER_UNREGISTER, the entry's
 * pending_unregister is set to 1.
 *
 * done_fn fanout (T70): after each cb invocation, calls
 * vm->watcher_body_done_fn(vm, handle, cb_result) if non-NULL.
 *
 * Compaction: at the end of the walk, entries with pending_unregister == 1
 * are compacted out (in-place shift; O(n) linear, registration is rare).
 *
 * args may be NULL when argc == 0. */
void uhost_watcher_table_walk_event(UHostWatcherTable *t, struct UVM *vm,
                                    urbi_event_id_t event_id,
                                    const UValue *args, int argc);

#ifdef __cplusplus
}
#endif

#endif /* UWATCHER_HOST_H */
