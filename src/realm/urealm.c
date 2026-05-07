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
#include "utag.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"  /* urbi_tag_stop */
#include "ustrand.h"    /* urbi_strand_destroy, UStrand.next_in_realm */
#include "gc/ugc_incremental.h"  /* gc_shade_gray — shade realm->tag */
#include "object/uobject.h"    /* urbi_object_alloc, URBI_ATOM_OBJECT */
#include "urealm_globals.h"    /* urbi_populate_realm_globals */

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
    URBI_ASSERT_NOT_ISR(vm);

    r = (URealm *)vm->alloc_fn(NULL, sizeof(URealm), vm->alloc_ud);
    if (r == NULL) return NULL;
    realm_zero(r, sizeof(URealm));

    r->vm    = vm;
    r->id    = ++vm->realm_id_seq;  /* per-VM counter; 0 means uninitialized */
    r->flags = 0;

    /* tag: root cleanup boundary for all strands in this realm. */
    r->tag = utag_create(vm);
    if (r->tag == NULL) {
        vm->alloc_fn(r, 0, vm->alloc_ud);
        return NULL;
    }

    /* reflective: UVAL_NIL at M3; populated at M4+. */
    r->reflective.kind = UVAL_NIL;
    r->reflective.v.i  = 0;

    /* Namespace. */
    r->bindings = unamespace_create(vm);
    if (r->bindings == NULL) {
        utag_destroy(vm, r->tag);
        vm->alloc_fn(r, 0, vm->alloc_ud);
        return NULL;
    }

    /* Global object: fresh empty UObject to hold the realm's named slots.
     * Pre-M5 spec #5 §4.1 step 2.  Allocated as root-atom family
     * (URBI_ATOM_OBJECT) so it inherits nothing by default. */
    r->global_object = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
    if (r->global_object == NULL) {
        unamespace_destroy(vm, r->bindings);
        utag_destroy(vm, r->tag);
        vm->alloc_fn(r, 0, vm->alloc_ud);
        return NULL;
    }

    /* user_data: stays NULL (caller may set after create). */

    /* Link at head of VM's realm list BEFORE populate so that
     * urbi_realm_destroy can safely unlink on populate failure. */
    r->prev_in_vm = NULL;
    r->next_in_vm = vm->realms_head;
    if (vm->realms_head != NULL) {
        vm->realms_head->prev_in_vm = r;
    }
    vm->realms_head = r;

    /* Populate the global object with the 15 built-in globals.
     * On failure, destroy (which unlinks + cleans up) and return NULL.
     * Partial population is acceptable per spec #5 §4.7 — the realm is
     * destroyed by the caller-side NULL check. */
    if (urbi_populate_realm_globals(vm, r) != URBI_OK) {
        urbi_realm_destroy(vm, r);
        return NULL;
    }

    return r;
}

/* === urbi_realm_destroy ===
 *
 * Destruction order per spec §4.4:
 *   1. Stop the realm's tag.
 *   2. Free namespace.
 *   3. Drop reflective (becomes unreachable; GC reclaims at M4+).
 *   4. Unlink from VM's realm list.
 *   5. Free the URealm struct itself.
 *
 * Precondition: All strands attached to this realm's tag must be dead
 * before calling this function. The urbi_tag_stop call deposits TAG_STOP
 * on all member strands (row 11 §3.5), but strands eventually fatal-escalate
 * rather than gracefully unwind at M3 (walker-side TAG_STOP absorption is
 * currently a pop-and-continue stub). Proper strand unwinding deferred to
 * when tag-scope absorption lands (row 11 §6).
 *
 * Safe to call with realm == NULL. */

void
urbi_realm_destroy(struct UVM *vm, URealm *realm)
{
    UValue nil;

    if (realm == NULL) return;
    if (vm == NULL) return;
    URBI_ASSERT_NOT_ISR(vm);

    nil.kind = UVAL_NIL;
    nil.v.i  = 0;

    /* Step 1 (T38): Free all heap-allocated strands registered in this realm.
     * Must happen BEFORE utag_destroy because each strand's cleanup stack
     * holds TAG_SCOPE entries that link back to realm->tag.  Destroying
     * strands first calls strand_unlink_from_tags which empties
     * tag->member_strands_head so that utag_destroy's invariant assertion
     * (member_strands_head == NULL) can pass.
     * Strands are threaded via UStrand.next_in_realm (head-inserted by
     * urbi_strand_create at the bottom of the list, so realm->tag's own
     * registration-strand is freed last). */
    {
        UStrand *strand = realm->strands_head;
        realm->strands_head = NULL;
        while (strand != NULL) {
            UStrand *next = strand->next_in_realm;
            strand->next_in_realm = NULL;
            urbi_strand_destroy(strand);
            strand = next;
        }
    }

    /* Step 2: Stop the realm's tag (row 11 §3.5: deposits TAG_STOP on all
     * member strands; they eventually fatal-escalate at M3).
     * After step 1, all realm strands are destroyed and unlinked, so the
     * tag's member list is empty and urbi_tag_stop is a no-op here at M3.
     * utag_destroy can then assert member_strands_head == NULL safely. */
    if (realm->tag != NULL) {
        urbi_tag_stop(vm, realm->tag, nil);
        utag_destroy(vm, realm->tag);
        realm->tag = NULL;
    }

    /* Step 3: Free namespace. */
    unamespace_destroy(vm, realm->bindings);
    realm->bindings = NULL;

    /* Step 3b: Drop global_object pointer (GC manages the cell).
     * The GC will reclaim the UObject at the next sweep pass once
     * the realm-root-provider no longer shades it. */
    realm->global_object = NULL;

    /* Step 4: reflective — zero it (GC owns the object if non-nil at M4+). */
    realm->reflective.kind = UVAL_NIL;
    realm->reflective.v.i  = 0;

    /* Step 5: Unlink from VM realm list. */
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

    /* Step 6: Free struct. */
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
    URBI_ASSERT_NOT_ISR(vm);

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
 * TODO(M5+): per-realm walk via realm->tag.member_strands_head — strands now linked
 * but counter partition still VM-wide at M3; partitioning deferred for M5+ work.
 *
 * Returns true if there is any live work visible to this realm at M3.
 * out_strands, out_watchers, out_wakes may be NULL. */

bool
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
        return false;
    }

    /* TODO(M5+): real per-realm walk via realm->tag.member_strands_head.
     * At M3, counters are VM-global; all are attributed to every realm query. */
    strands  = realm->vm->strand_runnable_count;
    watchers = realm->vm->watcher_active_count;
    wakes    = realm->vm->wakeup_pending_count;

    if (out_strands)  *out_strands  = strands;
    if (out_watchers) *out_watchers = watchers;
    if (out_wakes)    *out_wakes    = wakes;

    return (strands > 0 || watchers > 0 || wakes > 0);
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
 *   1. realm->reflective (UVAL_NIL at M5; UValue slot still walked).
 *   2. namespace entries (via unamespace_walk_roots).
 *   3. realm->tag — GC-managed at M5 (T18: urbi_gc_alloc, UTYPE_TAG).
 *      Shaded via gc_shade_gray so the UTYPE_TAG walker runs and yields
 *      name + enter_event + leave_event + member_watchers_head chain. */

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

        /* 3. global_object — GC-managed UObject; shade so slot walker runs. */
        if (r->global_object != NULL) {
            gc_shade_gray(vm, (UCell *)r->global_object);
        }

        /* 4. tag — GC-managed at M5 T18; shade so the UTag walker runs. */
        if (r->tag != NULL) {
            gc_shade_gray(vm, (UCell *)r->tag);
        }
    }
}
