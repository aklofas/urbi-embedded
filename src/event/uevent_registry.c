/* SPDX-License-Identifier: BSD-3-Clause */
/* uevent_registry.c — internal name→event_id→UEvent map for Gap B (v0.7.1).
 *
 * Implements the UEventRegistry operations declared in uevent_registry.h.
 * Freestanding: no libc beyond the allocator hook in vm->alloc_fn. */

#include "event/uevent_registry.h"
#include "event/uevent.h"            /* urbi_event_create */
#include "event/uevent_native.h"     /* uvalue_from_event */
#include "vm/uvm.h"                  /* UVM, alloc_fn, last_error */
#include "realm/urealm.h"            /* URealm (forward used by urbi.h decls) */
#include "runtime/umacros.h"         /* urbi_zero, urbi_memeq, urbi_strlen */
#include "value/uintern.h"           /* ustr_intern */
#include "urbi/urbi.h"               /* urbi_realm_set_global_const, urbi_event_id_t */
#include "urbi/types.h"              /* URBI_EVENT_ID_INVALID, UErrCode */

#include <stddef.h>
#include <stdint.h>

/* Initial capacity when the first entry is added. */
#define REGISTRY_INIT_CAPACITY 8U

/* =========================================================================
 * uevent_registry_init
 * ========================================================================= */

void
uevent_registry_init(UEventRegistry *r)
{
    r->entries  = NULL;
    r->count    = 0;
    r->capacity = 0;
    r->next_id  = 0;
}

/* =========================================================================
 * uevent_registry_destroy
 * ========================================================================= */

void
uevent_registry_destroy(UEventRegistry *r, struct UVM *vm)
{
    if (r == NULL || r->entries == NULL) return;
    if (vm != NULL && vm->alloc_fn != NULL) {
        vm->alloc_fn(r->entries, 0, vm->alloc_ud);
    }
    r->entries  = NULL;
    r->count    = 0;
    r->capacity = 0;
    r->next_id  = 0;
}

/* =========================================================================
 * uevent_registry_lookup_by_id
 * ========================================================================= */

UEventRegistryEntry *
uevent_registry_lookup_by_id(UEventRegistry *r, urbi_event_id_t id)
{
    if (id == URBI_EVENT_ID_INVALID) return NULL;
    if ((size_t)id >= r->count) return NULL;
    return &r->entries[id];
}

/* =========================================================================
 * uevent_registry_lookup_by_name
 * ========================================================================= */

UEventRegistryEntry *
uevent_registry_lookup_by_name(UEventRegistry *r, const char *name,
                                size_t name_len)
{
    size_t i;
    if (r == NULL || name == NULL || r->entries == NULL) return NULL;
    for (i = 0; i < r->count; i++) {
        UEventRegistryEntry *e = &r->entries[i];
        if (e->name_len == name_len && urbi_memeq(e->name, name, name_len)) {
            return e;
        }
    }
    return NULL;
}

/* =========================================================================
 * uevent_registry_add
 * ========================================================================= */

UEventRegistryEntry *
uevent_registry_add(UEventRegistry *r, struct UVM *vm)
{
    UEventRegistryEntry *new_entries;
    size_t               new_cap;
    size_t               new_bytes;

    /* Reject if next_id would overflow the valid range. */
    if (r->next_id == URBI_EVENT_ID_INVALID) return NULL;

    /* Grow array when at capacity. */
    if (r->count >= r->capacity) {
        new_cap = (r->capacity == 0) ? REGISTRY_INIT_CAPACITY : r->capacity * 2U;
        new_bytes = new_cap * sizeof(UEventRegistryEntry);

        /* Allocate via vm's realloc-style hook:
         *   ptr != NULL, nbytes > 0 → realloc (or first alloc when ptr == NULL). */
        new_entries = (UEventRegistryEntry *)vm->alloc_fn(
                r->entries, new_bytes, vm->alloc_ud);
        if (new_entries == NULL) return NULL;

        /* Zero-initialize the newly allocated slots. */
        if (r->capacity < new_cap) {
            urbi_zero((unsigned char *)new_entries + r->capacity * sizeof(UEventRegistryEntry),
                      (new_cap - r->capacity) * sizeof(UEventRegistryEntry));
        }
        r->entries  = new_entries;
        r->capacity = new_cap;
    }

    /* Append new entry. */
    {
        UEventRegistryEntry *entry = &r->entries[r->count];
        urbi_zero(entry, sizeof(*entry));
        entry->id = r->next_id;
        r->count++;
        r->next_id++;
        return entry;
    }
}

/* =========================================================================
 * urbi_event_register — public C API (Gap B)
 * ========================================================================= */

/* urbi_event_register: allocate a UEvent, install it as a const realm-global
 * under `name`, and record the (id, event, destruct_fn) triple in the VM's
 * event registry.  Returns an opaque urbi_event_id_t for use with
 * urbi_inject_event.  Returns URBI_EVENT_ID_INVALID on error.
 *
 * Error conditions (pre-Phase-8 last_error set):
 *   NULL vm/realm/name  → URBI_ERR_INVALID_ARG
 *   name already taken  → URBI_ERR_EVENT_NAME_TAKEN
 *   OOM                 → URBI_ERR_OOM
 *
 * Thread safety: MAIN. */
urbi_event_id_t
urbi_event_register(struct UVM *vm, struct URealm *realm,
                    const char *name,
                    urbi_event_payload_destructure_fn destruct_fn,
                    void *destruct_ud)
{
    const char           *interned;
    size_t                name_len;
    UEvent               *ev;
    UEventRegistryEntry  *entry;
    UValue                ev_value;
    int                   rc;

    /* --- Argument validation --- */
    if (vm == NULL || realm == NULL || name == NULL) {
        /* TODO Phase 8: set urbi_last_error before returning */
        return URBI_EVENT_ID_INVALID;
    }

    /* --- Intern the name --- */
    name_len = urbi_strlen(name);
    interned = ustr_intern(vm, name, name_len);
    if (interned == NULL) {
        /* OOM in intern table. */
        /* TODO Phase 8: set urbi_last_error before returning */
        return URBI_EVENT_ID_INVALID;
    }

    /* --- Duplicate-name check --- */
    if (uevent_registry_lookup_by_name(&vm->event_registry, interned, name_len) != NULL) {
        /* TODO Phase 8: set urbi_last_error before returning */
        return URBI_EVENT_ID_INVALID;
    }

    /* --- Allocate UEvent --- */
    ev = urbi_event_create(vm);
    if (ev == NULL) {
        /* OOM from GC allocator. */
        /* TODO Phase 8: set urbi_last_error before returning */
        return URBI_EVENT_ID_INVALID;
    }

    /* --- Add registry entry --- */
    entry = uevent_registry_add(&vm->event_registry, vm);
    if (entry == NULL) {
        /* OOM growing the entries[] array.  The UEvent is already allocated
         * and GC-managed; it will be collected naturally — no manual free. */
        /* TODO Phase 8: set urbi_last_error before returning */
        return URBI_EVENT_ID_INVALID;
    }
    entry->event       = ev;
    entry->destruct_fn = destruct_fn;
    entry->destruct_ud = destruct_ud;
    entry->name        = interned;
    entry->name_len    = name_len;

    /* --- Install as const realm-global --- */
    ev_value = uvalue_from_event(ev);
    rc = urbi_realm_set_global_const(vm, realm, name, name_len, ev_value);
    if (rc != URBI_OK) {
        /* Undo: remove the entry we just appended (shrink count). */
        vm->event_registry.count--;
        vm->event_registry.next_id--;
        /* TODO Phase 8: set urbi_last_error before returning */
        return URBI_EVENT_ID_INVALID;
    }

    return entry->id;
}
