/* SPDX-License-Identifier: BSD-3-Clause */
/* URealm lifecycle: create, destroy, global accessor, liveness query.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * Allocation uses vm->alloc_fn (realloc semantics).
 * Zero-fill uses a byte loop (same pattern as ucleanup.c / uarena.c). */

#if __STDC_HOSTED__
#  include <assert.h>
#  define UREALM_ASSERT(x) assert(x)
#else
#  define UREALM_ASSERT(x) ((void)0)
#endif

#include <stddef.h>
#include <stdint.h>

#include "urealm.h"
#include "runtime/umacros.h"
#include "tag/utag.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"  /* urbi_tag_stop */
#include "sched/ustrand.h"    /* urbi_strand_destroy, UStrand.next_in_realm */
#include "sched/usched_cooperative.h"  /* sched_strand_unbind_from_ready_queue */
#include "gc/ugc_incremental.h"  /* gc_shade_gray — shade realm->tag */
#include "object/uobject.h"    /* urbi_object_alloc, URBI_ATOM_OBJECT */
#include "realm/urealm_globals.h"    /* urbi_populate_realm_globals */
#include "stdlib/temporal.h"         /* urbi_periodic_destroy_for_realm — v0.9.4 */

/* === urbi_realm_create ===
 *
 * Allocates a fresh URealm, assigns a unique ID, links to vm->realms_head,
 * creates an empty namespace, and returns it.
 *
 * Returns NULL on OOM.
 *
 * GC soundness (v0.13.2, refactor-3 TEST-GAP-01 discovery chain): the realm
 * is linked onto vm->realms_head BEFORE the first GC allocation, not after.
 * Pre-v0.13.2 the link happened after r->tag / r->bindings / r->global_object
 * were created — an un-rooted window in which a collection triggered by a
 * later allocation (e.g. the global_object alloc collecting while r->tag is
 * reachable only through the not-yet-linked realm) swept the earlier cells.
 * Normal builds never collect inside this window today (GC slices run at
 * strand safepoints / urbi_step, and realm bootstrap is host C code with no
 * strand), so the bug was latent — but URBI_GC_STRESS's collect-on-every-
 * alloc hook made every script run die at boot (gc_shade_gray on a swept
 * r->tag).  Link-first ROOTS the window by construction: each pointer is
 * reachable via realm_list_walk_roots the instant it is stored, and the
 * walkers (realm_list_walk_roots, unamespace_walk_roots, sched_walk_roots)
 * all NULL-guard the not-yet-populated fields of a fresh zeroed realm.
 *
 * Cleanup (REALM-009, REALM-021, reshaped at v0.13.2): once linked, every
 * failure path funnels through urbi_realm_destroy, which unlinks and is
 * NULL-safe on each partially-initialized field. */

URealm *
urbi_realm_create(struct UVM *vm)
{
    URealm *r;

    if (vm == NULL) return NULL;
    URBI_ASSERT_NOT_ISR(vm);

    r = (URealm *)vm->alloc_fn(NULL, sizeof(URealm), vm->alloc_ud);
    if (r == NULL) return NULL;
    urbi_zero(r, sizeof(URealm));

    r->vm    = vm;
    r->id    = ++vm->realm_id_seq;  /* per-VM counter; 0 means uninitialized */
    r->flags = 0;

    /* Link at head of VM's realm list FIRST (see GC-soundness note above):
     * everything stored into r->tag / r->bindings / r->global_object below
     * becomes a GC root the moment the pointer lands in the struct. */
    r->prev_in_vm = NULL;
    r->next_in_vm = vm->realms_head;
    if (vm->realms_head != NULL) {
        vm->realms_head->prev_in_vm = r;
    }
    vm->realms_head = r;

    /* tag: root cleanup boundary for all strands in this realm. */
    r->tag = utag_create(vm);
    if (r->tag == NULL) goto fail;

    /* Namespace (host-allocated, not GC-managed). */
    r->bindings = unamespace_create(vm);
    if (r->bindings == NULL) goto fail;

    /* Global object: fresh empty UObject to hold the realm's named slots.
     * Pre-M5 spec #5 §4.1 step 2.  Allocated as root-atom family
     * (URBI_ATOM_OBJECT) so it inherits nothing by default. */
    r->global_object = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
    if (r->global_object == NULL) goto fail;

    /* user_data: stays NULL (caller may set after create). */

    /* Populate the global object with the 15 built-in globals.
     * Partial population is acceptable per spec #5 §4.7 — the realm is
     * destroyed by the caller-side NULL check. */
    if (urbi_populate_realm_globals(vm, r) != URBI_OK) goto fail;

    return r;

fail:
    /* Unlinks from vm->realms_head + tears down whichever of tag/bindings/
     * global_object exist; NULL-safe on the rest. */
    urbi_realm_destroy(vm, r);
    return NULL;
}

/* === urbi_realm_create_repl === (v0.9.0-repl)
 *
 * v0.9.1 update: auto-applies URBI_DEFAULT_REPL_BUDGET so REPL realms
 * are protected from untrusted-source compile bombs by default.  Embedders
 * may override post-create via urbi_realm_set_compile_budget. */
URealm *
urbi_realm_create_repl(struct UVM *vm)
{
    URealm *r = urbi_realm_create(vm);
    if (r != NULL) {
        r->flags |= REALM_REPL;
        r->compile_budget = URBI_DEFAULT_REPL_BUDGET;
        r->has_compile_budget = true;
    }
    return r;
}

/* === v0.9.1: per-realm writer + compile-budget setters ===
 *
 * URBI_DEFAULT_REPL_BUDGET: 256 / 100000 / 1 MiB per spec §3.4.
 * Defined here (not in a header) so external consumers link against the
 * canonical instance.  Embedders may copy + tweak per-realm. */
const UCompileBudget URBI_DEFAULT_REPL_BUDGET = {
    256U,                  /* max_parser_depth  — Lua-grade default */
    100000U,               /* max_ast_nodes     — comfortably above any sane REPL line */
    1U << 20               /* max_source_bytes  — 1 MiB hard cap on submitted source */
};

void
urbi_realm_set_writer(struct UVM *vm, URealm *realm,
                      urbi_writer_fn fn, void *ud)
{
    (void)vm;
    if (realm == NULL) return;
    realm->writer_fn = fn;
    realm->writer_ud = ud;
}

void
urbi_realm_set_compile_budget(struct UVM *vm, URealm *realm,
                              const UCompileBudget *budget)
{
    (void)vm;
    if (realm == NULL) return;
    if (budget == NULL) {
        realm->has_compile_budget = false;
        /* compile_budget contents intentionally left as-is; gated by flag. */
        return;
    }
    realm->compile_budget = *budget;
    realm->has_compile_budget = true;
}

const UCompileBudget *
urbi_realm_get_compile_budget(struct UVM *vm, const URealm *realm)
{
    (void)vm;
    if (realm == NULL || !realm->has_compile_budget) return NULL;
    return &realm->compile_budget;
}

/* === urbi_realm_destroy ===
 *
 * Destruction order per spec §4.4:
 *   1. Stop the realm's tag.
 *   2. Free namespace.
 *   3. Unlink from VM's realm list.
 *   4. Free the URealm struct itself.
 *
 * Precondition: All strands attached to this realm's tag must be dead
 * before calling this function. The urbi_tag_stop call deposits TAG_STOP
 * on all member strands, but strands eventually fatal-escalate rather
 * than gracefully unwind at M3 (walker-side TAG_STOP absorption is
 * currently a pop-and-continue stub). Proper strand unwinding deferred
 * to when tag-scope absorption lands (deferred to v1.x; see
 * docs/urbi-embedded-design-risks.md).
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

    /* v0.9.4 Step 0: mark periodics owned by this realm for unregister
     * BEFORE the strand sweep frees their body strands.  Clears each
     * periodic's current_strand back-pointer + each body strand's
     * periodic_owner back-pointer so urbi_strand_destroy (called below)
     * does not leave any dangling references.  Periodic records are
     * freed at the end of this function (Step 4b) once all bodies are
     * gone and no further pump pass touches the list. */
    urbi_periodic_destroy_for_realm(vm, realm);

    /* Step 1: Free all heap-allocated strands registered in this realm.
     * Must happen BEFORE utag_destroy because each strand's cleanup stack
     * holds TAG_SCOPE entries that link back to realm->tag.  Destroying
     * strands first calls strand_unlink_from_tags which empties
     * tag->member_strands_head so that utag_destroy's invariant assertion
     * (member_strands_head == NULL) can pass.
     * Strands are threaded via UStrand.next_in_realm (head-inserted by
     * urbi_strand_create at the bottom of the list, so realm->tag's own
     * registration-strand is freed last).
     *
     * REALM-011: before calling urbi_strand_destroy on a strand, splice
     * it out of vm->ready_head / ready_tail's doubly-linked list so the queue
     * never holds dangling pointers into freed strand memory.  Without this
     * unbind step sched_strand_destroy zeroes only the strand's own
     * ready_next / ready_prev fields — neighbours retain stale pointers and
     * the next dispatch (or sched_walk_roots GC scan) trips use-after-free
     * under ASan.  The helper is idempotent on strands that are not on the
     * queue (DORMANT / RUNNING / WAITING / DEAD), so we can call it
     * unconditionally on every strand in the realm's list. */
    {
        UStrand *strand = realm->strands_head;
        realm->strands_head = NULL;
        while (strand != NULL) {
            UStrand *next = strand->next_in_realm;
            strand->next_in_realm = NULL;
            sched_strand_unbind_from_ready_queue(strand);
            urbi_strand_destroy(vm, strand);
            strand = next;
        }
    }

    /* Step 2: Stop the realm's tag (deposits TAG_STOP on all
     * member strands; they eventually fatal-escalate at M3).
     * After step 1, all realm strands are destroyed and unlinked, so the
     * tag's member list is empty and urbi_tag_stop is a no-op here at M3.
     * utag_destroy can then assert member_strands_head == NULL safely. */
    if (realm->tag != NULL) {
        urbi_tag_stop(vm, realm->tag, nil);
        utag_destroy(vm, realm->tag);
        realm->tag = NULL;
    }

    /* Step 2b (v0.9.0-repl Task 12): walk loaded_protos_head and unload
     * realm-owned modules.  Strands were stopped in step 1, so strand-bind
     * refcounts have dropped; closures from other realms that reference these
     * modules' protos survive via the root_proto-refcount rescue mechanism
     * (uchunk_destroy stashes them onto vm->rescued_protos).
     *
     * VM-owned overlays (stdlib, urobotics, future — flagged vm_owned at
     * registration, refactor-3 GC-18) are freed by urbi_vm_destroy, never
     * here.  Their back-pointers to this (about-to-be-freed) realm are
     * cleared in the same pass.  In-list-only clearing is sufficient:
     * urealm_register_module is the sole setter of owning_realm and sets the
     * back-pointer and the list link together (skipping entirely when
     * owning_realm != NULL — Task 5); the only writers that clear the pair
     * — urbi_unload plus the three uchunk_destroy clear sites in
     * uchunk_io.c (the rescue-path and no-vm-path unlinks in uchunk_destroy
     * and the one in uchunk_destroy_internal) — unlink from
     * loaded_protos_head and clear both fields together.  So
     * owning_realm == realm holds exactly for the protos on this realm's
     * loaded_protos_head, and a vm_owned module registered into a different
     * realm has nothing pointing at this one. */
    {
        UProto *p = realm->loaded_protos_head;
        while (p != NULL) {
            UProto *next = p->next_in_realm;
            if (p->vm_owned) {
                if (p->owning_realm == realm) {
                    p->owning_realm  = NULL;
                    p->next_in_realm = NULL;
                }
            } else {
                urbi_unload(vm, p);
            }
            p = next;
        }
        realm->loaded_protos_head = NULL;
    }

    /* Step 3: Free namespace. */
    unamespace_destroy(vm, realm->bindings);
    realm->bindings = NULL;

    /* Step 3b: Drop global_object pointer (GC manages the cell).
     * The GC will reclaim the UObject at the next sweep pass once
     * the realm-root-provider no longer shades it. */
    realm->global_object = NULL;

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
    URBI_ASSERT_NOT_ISR(vm);

    if (vm->global_realm != NULL) {
        return vm->global_realm;
    }

    vm->global_realm = urbi_realm_create(vm);
    if (vm->global_realm == NULL) return NULL;

    vm->global_realm->flags |= REALM_GLOBAL;
    return vm->global_realm;
}

/* === urbi_vm_has_live_work ===
 *
 * Inclusive host-facing liveness query: "is anything at all alive?".
 * Renamed from urbi_realm_has_live_work at v0.6.0 (REALM-017): the function
 * never read per-realm state.  Per-realm partitioning is a v1.x deferral
 * (urbi-embedded-design-risks.md).
 *
 * v0.13.3 (refactor-3 SCHED-13): computed via urbi_vm_liveness(), the one
 * quiescence/liveness formula.  Unlike urbi_step's QUIESCENT verdict —
 * which excludes `armed` (watchers + SUSPENDED/WAITING strands, owner
 * decision 2026-06-11) — this query stays INCLUSIVE: an armed-but-idle VM
 * reports true so the host can distinguish it from a fully-dead one.
 *
 * Out-params keep their historical meaning (any may be NULL):
 *   out_strands  — runnable strands (|READY| + |RUNNING non-transient|).
 *   out_watchers — armed watchers, all modes (vm->watchers->active_count).
 *   out_wakes    — sleep-queue population (wakeup_pending_count). */

bool
urbi_vm_has_live_work(const struct UVM *vm,
                      uint32_t *out_strands,
                      uint32_t *out_watchers,
                      uint32_t *out_wakes)
{
    UVmLiveness lv;

    if (vm == NULL) {
        if (out_strands)  *out_strands  = 0;
        if (out_watchers) *out_watchers = 0;
        if (out_wakes)    *out_wakes    = 0;
        return false;
    }

    urbi_vm_liveness(vm, &lv);

    if (out_strands)  *out_strands  = lv.runnable;
    if (out_watchers) *out_watchers = vm->watchers->active_count;
    if (out_wakes)    *out_wakes    = vm->wakeup_pending_count;

    return (lv.runnable > 0 || lv.pending > 0 || lv.timed > 0 || lv.armed > 0);
}

/* === urealm_teardown_all ===
 *
 * Destroy all Realms still alive at urbi_vm_destroy() time.
 * Called from urbi_vm_destroy().  Safe to call on a VM with no Realms. */

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
 *   1. namespace entries (via unamespace_walk_roots).
 *   2. realm->global_object — GC-managed UObject; shade so slot walker runs.
 *   3. realm->tag — GC-managed since M5 via urbi_gc_alloc / UTYPE_TAG.
 *      Shaded via gc_shade_gray so the UTYPE_TAG walker runs and yields
 *      name + enter_event + leave_event + parent (member watchers are
 *      rooted by the pool-wide provider in uwatcher_gc.c, GC-05).
 *
 * REALM-008: the (UCell *)r->tag cast below depends on UTag laying out a
 * UCell-compatible header (type_tag at byte 0, gc_byte at byte 1) as its
 * first two bytes.  Pinned with URBI_STATIC_ASSERT so any reordering of UTag's
 * leading fields fails at compile time rather than producing a silently
 * miscoloured cell at runtime. */

URBI_STATIC_ASSERT(offsetof(UTag, type_tag) == 0,
               "UTag.type_tag must alias UCell.type_tag at offset 0 "
               "(realm_list_walk_roots casts (UCell *)r->tag)");
URBI_STATIC_ASSERT(offsetof(UTag, gc_byte) == 1,
               "UTag.gc_byte must alias UCell.gc_byte at offset 1 "
               "(realm_list_walk_roots casts (UCell *)r->tag)");

void
realm_list_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx)
{
    URealm *r;

    UREALM_ASSERT(vm != NULL);
    UREALM_ASSERT(cb != NULL);

    for (r = vm->realms_head; r != NULL; r = r->next_in_vm) {
        /* 1. namespace bindings. */
        unamespace_walk_roots(r->bindings, cb, vm, ctx);

        /* 2. global_object — GC-managed UObject; shade so slot walker runs. */
        if (r->global_object != NULL) {
            gc_shade_gray(vm, (UCell *)r->global_object);
        }

        /* 3. tag — GC-managed since M5; shade so the UTag walker runs. */
        if (r->tag != NULL) {
            gc_shade_gray(vm, (UCell *)r->tag);
        }
    }
}
