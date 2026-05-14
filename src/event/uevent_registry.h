/* SPDX-License-Identifier: BSD-3-Clause */
/* uevent_registry.h — internal name→event_id→UEvent map for Gap B (v0.7.1).
 *
 * The registry maps interned name strings to (id, UEvent *, destructure_fn)
 * triples allocated by urbi_event_register.  urbi_inject_event routes ISR
 * ring entries through this map at drain time using the numeric id for O(1)
 * array-indexed lookup.  Name → id lookup (at registration) is a linear scan
 * over the entries[] array; registration is rare so O(n) is acceptable.
 *
 * Ownership:
 *   entries[]:  heap-allocated via vm->alloc_fn; owned by UEventRegistry.
 *   name:       interned string; owned by the vm intern table (freed at
 *               urbi_vm_destroy by uintern_destroy, not by the registry).
 *   event:      GC-managed UEvent cell; NOT freed by the registry.
 *
 * Freestanding: no <string.h>; comparisons use urbi_memeq from umacros.h. */

#ifndef UEVENT_REGISTRY_H
#define UEVENT_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#include "urbi/types.h"      /* urbi_event_id_t, URBI_EVENT_ID_INVALID, UValue */
#include "urbi/urbi.h"       /* urbi_event_payload_destructure_fn */

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations. */
struct UVM;
struct UEvent;

/* === Registry entry ===
 *
 * One entry per urbi_event_register call.  Entries are dense in entries[0..count).
 * The entry's id equals its index in the entries[] array (id == &entry - entries),
 * which enables O(1) lookup_by_id as a direct array dereference. */
typedef struct {
    urbi_event_id_t                       id;           /* 0 .. 0xFFFE; equals index */
    struct UEvent                        *event;        /* GC-managed UEvent */
    urbi_event_payload_destructure_fn     destruct_fn;  /* NULL = no args */
    void                                 *destruct_ud;
    const char                           *name;         /* interned; stable for VM lifetime */
    size_t                                name_len;
    uint8_t                               tombstoned;   /* 1 after urbi_event_unregister */
} UEventRegistryEntry;

/* === Registry container === */
typedef struct {
    UEventRegistryEntry *entries;   /* heap array; capacity >= count */
    size_t               count;     /* live entries (also == next_id value) */
    size_t               capacity;  /* allocated slots */
    urbi_event_id_t      next_id;   /* monotonically increasing; equals count */
} UEventRegistry;

/* uevent_registry_init: zero-initialize the registry fields.
 * Call before first use.  Does not allocate (entries starts NULL). */
void uevent_registry_init(UEventRegistry *r);

/* uevent_registry_destroy: free the entries[] array via vm->alloc_fn.
 * GC-managed UEvent cells are NOT freed here — the GC owns them.
 * Interned name strings are NOT freed here — the intern table owns them.
 * Safe to call on a zero-initialized registry (entries == NULL). */
void uevent_registry_destroy(UEventRegistry *r, struct UVM *vm);

/* uevent_registry_lookup_by_id: O(1) index into entries[].
 * Returns NULL if id >= count or id == URBI_EVENT_ID_INVALID. */
UEventRegistryEntry *uevent_registry_lookup_by_id(UEventRegistry *r,
                                                   urbi_event_id_t id);

/* uevent_registry_lookup_by_name: linear scan; registration is rare.
 * name / name_len need not be NUL-terminated (compared via urbi_memeq).
 * Returns NULL if not found. */
UEventRegistryEntry *uevent_registry_lookup_by_name(UEventRegistry *r,
                                                     const char *name,
                                                     size_t name_len);

/* uevent_registry_add: append a new entry to the registry.
 * Grows the entries[] array (doubling) when count reaches capacity.
 * Returns a pointer to the new (zero-initialized) entry on success —
 * caller MUST fill in event, destruct_fn, destruct_ud, name, name_len.
 * The entry's id and index are pre-filled to registry->next_id.
 * Returns NULL on OOM; the registry is left unchanged. */
UEventRegistryEntry *uevent_registry_add(UEventRegistry *r, struct UVM *vm);

/* uevent_registry_tombstone: mark entry[id] as tombstoned so it is skipped
 * by lookup_by_id, lookup_by_name, and drain routing.
 * Returns 0 on success, -1 if id is out of range or already tombstoned. */
int uevent_registry_tombstone(UEventRegistry *r, urbi_event_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* UEVENT_REGISTRY_H */
