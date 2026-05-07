/* SPDX-License-Identifier: BSD-3-Clause */
/* UNamespace: open-addressed name→UValue map for a URealm.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * Allocation uses vm->alloc_fn (realloc semantics).
 * Zero-fill uses a byte loop (same pattern as ucleanup.c / uarena.c).
 * Precondition checks use a guarded <assert.h> on hosted targets.
 *
 * Key discipline: names are INTERNED pointers (from ustr_intern).
 * Lookup uses pointer equality — O(n) linear scan; adequate for
 * top-level namespaces which rarely exceed a few hundred entries.
 * Row 8 / T14. */

#if __STDC_HOSTED__
#  include <assert.h>
#  define UREALM_NS_ASSERT(x) assert(x)
#else
#  define UREALM_NS_ASSERT(x) ((void)0)
#endif

#include <stddef.h>
#include <stdint.h>

#include "urealm.h"
#include "runtime/umacros.h"
#include "vm/uvm.h"

/* === Internal entry layout === */

typedef struct {
    const char *name;   /* interned pointer; NULL = vacant slot */
    UValue      value;
} UNsEntry;

/* === UNamespace struct (opaque outside this file) === */

struct UNamespace {
    UNsEntry  *entries;
    uint32_t  count;
    uint32_t  cap;
};

/* Initial capacity — must be > 0. */
#define NS_INITIAL_CAP 16u

/* === unamespace_create === */

struct UNamespace *
unamespace_create(struct UVM *vm)
{
    struct UNamespace *ns;
    UNsEntry *entries;
    size_t entry_bytes;

    UREALM_NS_ASSERT(vm != NULL);

    ns = (struct UNamespace *)vm->alloc_fn(NULL, sizeof(struct UNamespace),
                                           vm->alloc_ud);
    if (ns == NULL) return NULL;
    urbi_zero(ns, sizeof(struct UNamespace));

    entry_bytes = (size_t)NS_INITIAL_CAP * sizeof(UNsEntry);
    entries = (UNsEntry *)vm->alloc_fn(NULL, entry_bytes, vm->alloc_ud);
    if (entries == NULL) {
        vm->alloc_fn(ns, 0, vm->alloc_ud);
        return NULL;
    }
    urbi_zero(entries, entry_bytes);

    ns->entries = entries;
    ns->count   = 0;
    ns->cap     = NS_INITIAL_CAP;
    return ns;
}

/* === unamespace_destroy === */

void
unamespace_destroy(struct UVM *vm, struct UNamespace *ns)
{
    if (ns == NULL) return;
    UREALM_NS_ASSERT(vm != NULL);

    if (ns->entries != NULL) {
        vm->alloc_fn(ns->entries, 0, vm->alloc_ud);
    }
    vm->alloc_fn(ns, 0, vm->alloc_ud);
}

/* === unamespace_set ===
 *
 * If name already exists, overwrites its value.
 * If the table is full, doubles capacity before inserting.
 * Returns 0 on success, -1 on OOM. */

int
unamespace_set(struct UVM *vm, struct UNamespace *ns,
               const char *name, UValue value)
{
    uint32_t i;
    UNsEntry *old_entries;
    UNsEntry *new_entries;
    size_t new_cap;
    size_t new_bytes;

    UREALM_NS_ASSERT(vm != NULL);
    UREALM_NS_ASSERT(ns != NULL);
    UREALM_NS_ASSERT(name != NULL);

    /* Search for existing entry (pointer equality — interned names). */
    for (i = 0; i < ns->count; i++) {
        if (ns->entries[i].name == name) {
            ns->entries[i].value = value;
            return 0;
        }
    }

    /* Grow if at capacity. */
    if (ns->count >= ns->cap) {
        new_cap   = (size_t)ns->cap * 2u;
        new_bytes = new_cap * sizeof(UNsEntry);
        old_entries = ns->entries;
        new_entries = (UNsEntry *)vm->alloc_fn(old_entries, new_bytes,
                                              vm->alloc_ud);
        if (new_entries == NULL) return -1;
        /* Zero the new slots (realloc does not zero the extended region). */
        {
            size_t old_bytes = (size_t)ns->cap * sizeof(UNsEntry);
            size_t extra     = new_bytes - old_bytes;
            urbi_zero((unsigned char *)new_entries + old_bytes, extra);
        }
        ns->entries = new_entries;
        ns->cap     = (uint32_t)new_cap;
    }

    /* Append new entry. */
    ns->entries[ns->count].name  = name;
    /* TODO(M4): wire urbi_gc_slot_write here once UNamespace embeds UCell as first member. */
    ns->entries[ns->count].value = value;
    ns->count++;
    return 0;
}

/* === unamespace_get ===
 *
 * Returns pointer to stored UValue on hit, NULL on miss.
 * name must be an interned pointer. */

UValue *
unamespace_get(struct UNamespace *ns, const char *name)
{
    uint32_t i;

    UREALM_NS_ASSERT(ns != NULL);
    UREALM_NS_ASSERT(name != NULL);

    for (i = 0; i < ns->count; i++) {
        if (ns->entries[i].name == name) {
            return &ns->entries[i].value;
        }
    }
    return NULL;
}

/* === unamespace_walk_roots ===
 *
 * Invokes cb for every UValue stored in the namespace.
 * Used by the GC root-provider walker (row 10 / T26). */

void
unamespace_walk_roots(struct UNamespace *ns,
                      UGcRootCallback cb,
                      struct UVM *vm, void *ctx)
{
    uint32_t i;

    if (ns == NULL) return;

    for (i = 0; i < ns->count; i++) {
        cb(vm, &ns->entries[i].value, ctx);
    }
}
