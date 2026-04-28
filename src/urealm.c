/* SPDX-License-Identifier: BSD-3-Clause */
/* URealm lifecycle: create, destroy, global accessor, liveness query.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * Allocation uses vm->alloc_fn (realloc semantics).
 * Zero-fill uses a byte loop (same pattern as ucleanup.c / uarena.c).
 * Row 8 / T14. */

#if __STDC_HOSTED__
#  include <assert.h>
#  define UREALM_ASSERT(x) assert(x)
#else
#  define UREALM_ASSERT(x) ((void)0)
#endif

#include <stddef.h>
#include <stdint.h>

#include "urealm.h"
#include "uvm.h"
#include "urbi.h"  /* urbi_tag_stop */

/* === Zero-fill helper === */

static void
realm_zero(void *dst, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)dst;
    size_t i;
    for (i = 0; i < n; i++) p[i] = 0;
}

/* === urbi_realm_create ===
 *
 * Allocates a fresh URealm, assigns a unique ID, creates an empty namespace,
 * links to vm->realms_head, and returns it.
 *
 * Returns NULL on OOM. */

URealm *
urbi_realm_create(struct UVM *vm)
{
    URealm *r;

    if (vm == NULL) return NULL;

    r = (URealm *)vm->alloc_fn(NULL, sizeof(URealm), vm->alloc_ud);
    if (r == NULL) return NULL;
    realm_zero(r, sizeof(URealm));

    r->vm    = vm;
    r->id    = ++vm->realm_id_seq;  /* per-VM counter; 0 means uninitialized */
    r->flags = 0;

    /* tag: stubbed to NULL at M3.
     * T29: utag_create(vm) lands here. */
    r->tag = NULL; /* T29: utag_create lands here. */

    /* reflective: UVAL_NIL at M3; populated at M4+. */
    r->reflective.kind = UVAL_NIL;
    r->reflective.v.i  = 0;

    /* Namespace. */
    r->bindings = unamespace_create(vm);
    if (r->bindings == NULL) {
        vm->alloc_fn(r, 0, vm->alloc_ud);
        return NULL;
    }

    /* user_data: stays NULL (caller may set after create). */

    /* Link at head of VM's realm list. */
    r->prev_in_vm = NULL;
    r->next_in_vm = vm->realms_head;
    if (vm->realms_head != NULL) {
        vm->realms_head->prev_in_vm = r;
    }
    vm->realms_head = r;

    return r;
}

/* === urbi_realm_destroy ===
 *
 * Destruction order per spec §4.4:
 *   1. Stop the realm's tag (no-op at M3; wires when T29-T31 land).
 *   2. Free namespace.
 *   3. Drop reflective (becomes unreachable; GC reclaims at M4+).
 *   4. Unlink from VM's realm list.
 *   5. Free the URealm struct itself.
 *
 * Safe to call with realm == NULL. */

void
urbi_realm_destroy(struct UVM *vm, URealm *realm)
{
    UValue nil;

    if (realm == NULL) return;
    if (vm == NULL) return;

    /* Step 1: Stop the realm's tag.
     * No-op at M3 because tag == NULL.  When T29-T31 land, this becomes a
     * real cross-strand walk. */
    nil.kind = UVAL_NIL;
    nil.v.i  = 0;
    if (realm->tag != NULL) {
        urbi_tag_stop(vm, realm->tag, nil);
    }

    /* Step 2: Free namespace. */
    unamespace_destroy(vm, realm->bindings);
    realm->bindings = NULL;

    /* Step 3: reflective — zero it (GC owns the object if non-nil at M4+). */
    realm->reflective.kind = UVAL_NIL;
    realm->reflective.v.i  = 0;

    /* Step 4: Unlink from VM realm list. */
    if (realm->prev_in_vm != NULL) {
        realm->prev_in_vm->next_in_vm = realm->next_in_vm;
    } else {
        /* realm was the head. */
        vm->realms_head = realm->next_in_vm;
    }
    if (realm->next_in_vm != NULL) {
        realm->next_in_vm->prev_in_vm = realm->prev_in_vm;
    }

    /* If this was the global realm, clear the VM's cached pointer. */
    if (vm->global_realm == realm) {
        vm->global_realm = NULL;
    }

    /* Step 5: Free struct. */
    realm->vm   = NULL;
    vm->alloc_fn(realm, 0, vm->alloc_ud);
}

/* === urbi_realm_global ===
 *
 * Returns the VM-level global Realm singleton, creating it on first call.
 * Sets REALM_GLOBAL flag.  Returns NULL on OOM. */

URealm *
urbi_realm_global(struct UVM *vm)
{
    if (vm == NULL) return NULL;

    if (vm->global_realm != NULL) {
        return vm->global_realm;
    }

    vm->global_realm = urbi_realm_create(vm);
    if (vm->global_realm == NULL) return NULL;

    vm->global_realm->flags |= REALM_GLOBAL;
    return vm->global_realm;
}

/* === urbi_realm_has_live_work ===
 *
 * Reads VM-global liveness counters.
 * TODO(T15+): real per-realm walk via tag.member_strands_head once T29-T31 land.
 *
 * Returns true if there is any live work visible to this realm at M3.
 * out_strands, out_watchers, out_wakes may be NULL. */

int
urbi_realm_has_live_work(URealm *realm,
                         uint32_t *out_strands,
                         uint32_t *out_watchers,
                         uint32_t *out_wakes)
{
    uint32_t strands, watchers, wakes;

    if (realm == NULL || realm->vm == NULL) {
        if (out_strands)  *out_strands  = 0;
        if (out_watchers) *out_watchers = 0;
        if (out_wakes)    *out_wakes    = 0;
        return 0;
    }

    /* TODO(T15+): real per-realm walk via tag.member_strands_head once T29-T31 land.
     * At M3, counters are VM-global; all are attributed to every realm query. */
    strands  = realm->vm->strand_runnable_count;
    watchers = realm->vm->watcher_active_count;
    wakes    = realm->vm->wakeup_pending_count;

    if (out_strands)  *out_strands  = strands;
    if (out_watchers) *out_watchers = watchers;
    if (out_wakes)    *out_wakes    = wakes;

    return (strands > 0 || watchers > 0 || wakes > 0) ? 1 : 0;
}

/* === urealm_teardown_all ===
 *
 * Destroy all Realms still alive at uvm_destroy() time.
 * Called from uvm_destroy().  Safe to call on a VM with no Realms. */

void
urealm_teardown_all(struct UVM *vm)
{
    URealm *r;
    URealm *next;

    if (vm == NULL) return;

    r = vm->realms_head;
    while (r != NULL) {
        next = r->next_in_vm;
        /* Use internal destroy that unlinks and frees. */
        urbi_realm_destroy(vm, r);
        r = next;
    }
    /* realms_head and global_realm are zeroed by urbi_realm_destroy as it unlinks. */
}

/* === realm_list_walk_roots ===
 *
 * GC root provider: enumerates all UValues reachable from every Realm in
 * vm->realms_head linked list.  For each Realm:
 *   1. realm->reflective (UVAL_NIL at M3; UValue slot still walked).
 *   2. namespace entries (via unamespace_walk_roots).
 *   3. realm->tag — skipped at M3 (NULL); T29 walker enrolls UVAL_TAG. */

void
realm_list_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx)
{
    URealm *r;

    UREALM_ASSERT(vm != NULL);
    UREALM_ASSERT(cb != NULL);

    for (r = vm->realms_head; r != NULL; r = r->next_in_vm) {
        /* 1. reflective handle. */
        cb(vm, &r->reflective, ctx);

        /* 2. namespace bindings. */
        unamespace_walk_roots(r->bindings, cb, vm, ctx);

        /* 3. tag — T29: walker enrolls UVAL_TAG. */
        if (r->tag != NULL) {
            /* T29: walker enrolls UVAL_TAG here. */
        }
    }
}
