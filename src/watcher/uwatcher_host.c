/* SPDX-License-Identifier: BSD-3-Clause */
/* uwatcher_host.c — host-side reactive watcher table + public API.
 *
 * Implements:
 *   uhost_watcher_table_init / _destroy / _add / _walk_event (internal)
 *   urbi_register_watcher / urbi_unregister_watcher (public, Gap J v0.7.1)
 *
 * Dispatch ordering (spec §2.6): registered-path drain fires the script-side
 * UEvent (c_event_emit_async) first, then uhost_watcher_table_walk_event for
 * host watchers.  This means script-side `at(name?)` watcher bodies and host
 * watcher callbacks both observe the same event; script bodies are queued
 * first, host callbacks run immediately at drain time.
 *
 * Freestanding-safe: no <string.h>, <stdlib.h>, or hosted headers. */

#include "watcher/uwatcher_host.h"
#include "vm/uvm.h"                 /* UVM, vm->alloc_fn, vm->host_watcher_table */
#include "event/uevent_registry.h"  /* uevent_registry_lookup_by_id, UEventRegistryEntry */
#include "vm/uvm_error.h"           /* urbi_set_error_internal (Gap P) */
#include "urbi/types.h"             /* URBI_OK, URBI_ERR_INVALID_ARG, UErrCode */
#include "urbi/urbi.h"              /* urbi_watcher_fn, URBI_WATCHER_HANDLE_INVALID,
                                     * URBI_ERR_WATCHER_UNREGISTER */

#include <stddef.h>
#include <stdint.h>

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* Zero-fill helper: replaces memset(p, 0, n) — no hosted headers. */
static void
host_zero(void *p, size_t n)
{
    uint8_t *b = (uint8_t *)p;
    size_t i;
    for (i = 0U; i < n; i++) {
        b[i] = 0U;
    }
}

/* =========================================================================
 * uhost_watcher_table_init
 * ========================================================================= */

void
uhost_watcher_table_init(UHostWatcherTable *t)
{
    t->entries     = NULL;
    t->count       = 0U;
    t->capacity    = 0U;
    t->next_handle = 1;   /* 0 is URBI_WATCHER_HANDLE_INVALID; start at 1 */
}

/* =========================================================================
 * uhost_watcher_table_destroy
 * ========================================================================= */

void
uhost_watcher_table_destroy(UHostWatcherTable *t, struct UVM *vm)
{
    if (t->entries != NULL && vm != NULL && vm->alloc_fn != NULL) {
        vm->alloc_fn(t->entries, 0U, vm->alloc_ud);
    }
    t->entries  = NULL;
    t->count    = 0U;
    t->capacity = 0U;
}

/* =========================================================================
 * uhost_watcher_table_add
 * ========================================================================= */

UHostWatcher *
uhost_watcher_table_add(UHostWatcherTable *t, struct UVM *vm)
{
    UHostWatcher *entry;

    /* Grow if needed: double capacity (starting at 4). */
    if (t->count >= t->capacity) {
        size_t new_cap = (t->capacity == 0U) ? 4U : t->capacity * 2U;
        UHostWatcher *new_buf;

        if (vm->alloc_fn == NULL) return NULL;
        new_buf = (UHostWatcher *)vm->alloc_fn(
                NULL, new_cap * sizeof(UHostWatcher), vm->alloc_ud);
        if (new_buf == NULL) return NULL;

        host_zero(new_buf, new_cap * sizeof(UHostWatcher));

        /* Copy existing entries. */
        if (t->entries != NULL) {
            size_t i;
            for (i = 0U; i < t->count; i++) {
                new_buf[i] = t->entries[i];
            }
            vm->alloc_fn(t->entries, 0U, vm->alloc_ud);
        }

        t->entries  = new_buf;
        t->capacity = new_cap;
    }

    entry = &t->entries[t->count];
    host_zero(entry, sizeof(UHostWatcher));

    /* Assign handle: skip 0 (INVALID). */
    if (t->next_handle == 0) {
        t->next_handle = 1;
    }
    entry->handle = t->next_handle++;
    t->count++;

    return entry;
}

/* =========================================================================
 * uhost_watcher_table_walk_event
 * ========================================================================= */

void
uhost_watcher_table_walk_event(UHostWatcherTable *t, struct UVM *vm,
                               urbi_event_id_t event_id,
                               const UValue *args, int argc)
{
    size_t i;
    size_t write;

    if (t->entries == NULL || t->count == 0U) return;

    /* Walk and invoke matching, non-pending entries. */
    for (i = 0U; i < t->count; i++) {
        UHostWatcher *e = &t->entries[i];

        if (e->pending_unregister) continue;
        if (e->event_id != event_id) continue;
        if (e->cb == NULL) continue;

        {
            int result = e->cb(vm, event_id, args, argc, e->ud);

            /* T70: fire done_fn after cb returns. */
            if (vm->watcher_body_done_fn != NULL) {
                vm->watcher_body_done_fn(vm, e->handle, result);
            }

            if (result == URBI_ERR_WATCHER_UNREGISTER) {
                e->pending_unregister = 1U;
            }
        }
    }

    /* Compact: remove entries with pending_unregister == 1. */
    write = 0U;
    for (i = 0U; i < t->count; i++) {
        if (!t->entries[i].pending_unregister) {
            if (write != i) {
                t->entries[write] = t->entries[i];
            }
            write++;
        }
    }
    t->count = write;
}

/* =========================================================================
 * urbi_register_watcher (public API)
 * ========================================================================= */

urbi_watcher_handle_t
urbi_register_watcher(struct UVM *vm,
                      struct URealm *realm,
                      urbi_event_id_t event_id,
                      urbi_watcher_fn cb, void *ud)
{
    UHostWatcher *entry;

    (void)realm;   /* realm param: reserved for future per-realm scoping */

    if (vm == NULL || cb == NULL) {
        urbi_set_error_internal(vm, URBI_ERR_INVALID_ARG,
            "urbi_register_watcher: vm or cb is NULL",
            NULL, 0, "urbi_register_watcher");
        return URBI_WATCHER_HANDLE_INVALID;
    }
    if (event_id == URBI_EVENT_ID_INVALID) {
        urbi_set_error_internal(vm, URBI_ERR_INVALID_ARG,
            "urbi_register_watcher: event_id is URBI_EVENT_ID_INVALID",
            NULL, 0, "urbi_register_watcher");
        return URBI_WATCHER_HANDLE_INVALID;
    }

    /* Verify the event_id is registered (not tombstoned). */
    {
        UEventRegistryEntry *re = uevent_registry_lookup_by_id(
                &vm->event_registry, event_id);
        if (re == NULL) {
            urbi_set_error_internal(vm, URBI_ERR_INVALID_ARG,
                "urbi_register_watcher: event_id not registered in this VM",
                NULL, 0, "urbi_register_watcher");
            return URBI_WATCHER_HANDLE_INVALID;
        }
    }

    entry = uhost_watcher_table_add(&vm->host_watcher_table, vm);
    if (entry == NULL) {
        urbi_set_error_internal(vm, URBI_ERR_OOM,
            "urbi_register_watcher: OOM growing watcher table",
            NULL, 0, "urbi_register_watcher");
        return URBI_WATCHER_HANDLE_INVALID;
    }

    entry->event_id = event_id;
    entry->cb       = cb;
    entry->ud       = ud;
    entry->pending_unregister = 0U;

    return entry->handle;
}

/* =========================================================================
 * urbi_unregister_watcher (public API)
 * ========================================================================= */

int
urbi_unregister_watcher(struct UVM *vm, urbi_watcher_handle_t handle)
{
    size_t i;

    if (vm == NULL) {
        urbi_set_error_internal(vm, URBI_ERR_INVALID_ARG,
            "urbi_unregister_watcher: vm is NULL",
            NULL, 0, "urbi_unregister_watcher");
        return URBI_ERR_INVALID_ARG;
    }
    if (handle == URBI_WATCHER_HANDLE_INVALID) {
        urbi_set_error_internal(vm, URBI_ERR_INVALID_ARG,
            "urbi_unregister_watcher: handle is URBI_WATCHER_HANDLE_INVALID",
            NULL, 0, "urbi_unregister_watcher");
        return URBI_ERR_INVALID_ARG;
    }

    for (i = 0U; i < vm->host_watcher_table.count; i++) {
        UHostWatcher *e = &vm->host_watcher_table.entries[i];
        if (e->handle == handle) {
            if (e->pending_unregister) {
                /* Already pending removal: treat as not found. */
                urbi_set_error_internal(vm, URBI_ERR_INVALID_ARG,
                    "urbi_unregister_watcher: handle already pending removal",
                    NULL, 0, "urbi_unregister_watcher");
                return URBI_ERR_INVALID_ARG;
            }
            e->pending_unregister = 1U;
            return URBI_OK;
        }
    }

    urbi_set_error_internal(vm, URBI_ERR_INVALID_ARG,
        "urbi_unregister_watcher: handle not found",
        NULL, 0, "urbi_unregister_watcher");
    return URBI_ERR_INVALID_ARG;
}
