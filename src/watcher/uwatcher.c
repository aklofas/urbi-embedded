/* SPDX-License-Identifier: BSD-3-Clause */
/* UWatcher pool lifecycle + install/unregister + observer_dirty.
 * Reactive runtime landed in M5 (see docs/milestones/m5-reactive.md).
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * All allocation uses vm->alloc_fn (realloc semantics).
 * Zero-fill uses urbi_zero (volatile byte loop) — no memset dependency.
 *
 * HISTORICAL NOTE (WATCH-032 / WATCH-023): this TU previously exposed
 * `urbi_watcher_install_internal` as a test-only seam.  Removed in
 * v0.5.7-fixes (WATCH-023).  All install paths now go through
 * OP_AT_INSTALL / OP_AT_SYNC_INSTALL / OP_WHENEVER_INSTALL /
 * OP_WAITUNTIL_INSTALL (production) or `urbi_watcher_install_for_test`
 * (tests/unit/twatcher_install_helper.{c,h}, tests-only). */

#include "uwatcher.h"
#include "vm/uvm.h"
#include "runtime/uclosure.h"  /* UClosure full definition — w->condition/body/onleave pointer types */
#include "gc/ugc.h"            /* UTYPE_WATCHER */
#include "gc/ugc_incremental.h" /* UGC_IS_FIXED, UGC_HAS_WATCHER_OBSERVER, current_white */
#include "tag/utag.h"           /* UTag, member_watchers_head */
#include "urbi/urbi.h"           /* URBI_ASSERT_NOT_ISR */
#include "runtime/umacros.h"  /* URBI_INTERNAL_ASSERT */
#include "runtime/ulist.h"    /* URBI_SLIST_UNLINK */
#include "event/uevent_subscribe.h"   /* uevent_at_watchers_remove */
#include <stddef.h>
#include <stdint.h>

/* uwatcher_pool_alloc: pop one entry from the freelist.
 * Returns NULL if the pool is exhausted.
 * Initialises the common header and clears payload state. */
UWatcher *
uwatcher_pool_alloc(struct UVM *vm)
{
    UWatcher *w;
    uint16_t i;

    if (vm->watchers == NULL) return NULL;
    if (vm->watchers->pool_freelist == NULL) return NULL;

    /* Pop from freelist head. */
    w = vm->watchers->pool_freelist;
    vm->watchers->pool_freelist = w->next_active;

    /* Re-init cell header and clear payload. */
    w->type_tag        = UTYPE_WATCHER;
    /* gc_byte: UGC_IS_FIXED always set; color from vm->current_white. */
    w->gc_byte         = (uint8_t)(vm->current_white | UGC_IS_FIXED);
    w->mode            = 0;
    w->exhaust_policy        = URBI_EXHAUST_QUEUE;
    w->flags                 = URBI_WATCHER_ACTIVE;
    w->read_set_count        = 0;
    w->pending_refire_count  = 0;
    w->max_refire_queue      = URBI_WATCHER_REFIRE_QUEUE_DEFAULT;
    w->next_active     = NULL;
    w->next_in_tag     = NULL;
    w->owning_tag      = NULL;
    w->condition       = NULL;
    w->body            = NULL;
    w->onleave         = NULL;
    /* Clear the event-mode fields too: the pool-wide GC root walker shades
     * w->event on every ACTIVE slot (refactor-3 GC-05), so a recycled slot
     * must never resurface a stale event pointer even if a future teardown
     * path forgets to NULL it before pool_free. */
    w->next_in_event   = NULL;
    w->event           = NULL;
    /* Re-init last_value_cache kind+value; UValue._pad bytes are zero on
     * first use (slab-zeroed at pool_init) and irrelevant on recycle since
     * they have no semantic meaning. */
    w->last_value_cache.kind  = UVAL_NIL;
    w->last_value_cache.v.i   = 0;
    /* Reset eval-pass generation stamp.  A recycled slot retains the stamp
     * from its previous life; if the VM gen counter has since cycled back to
     * that value the new watcher would be skipped on its very first pass.
     * 0 is the sentinel "never stamped" value — watcher_eval_dirty skips
     * zero when incrementing vm->watchers->eval_pass_gen, so cur_pass_gen
     * is always >= 1 and a freshly allocated slot (stamp=0) is never equal
     * to it.  (slab-zeroed slots on first use are already safe; this clears
     * stale stamps on freelist reuse.) */
    w->eval_pass_gen          = 0;
    /* Clear read-set entries. */
    for (i = 0; i < (uint16_t)URBI_WATCHER_READSET_MAX; i++) {
        w->cells[i] = NULL;
    }

    /* Update pool counters. */
    vm->watchers->pool_in_use++;
    if (vm->watchers->pool_in_use > vm->watchers->pool_high_water) {
        vm->watchers->pool_high_water = vm->watchers->pool_in_use;
    }

    return w;
}

/* pool_free: push one entry back onto the freelist.
 * Decrements in_use counter; does NOT touch high_water.
 * v0.8.4 Step C-3: URBI_WATCHER_OWNS_* flags deleted; UClosure lifetime is
 * GC-managed since Step C-2.  Condition/body/onleave pointers are NOT
 * cleared here — uwatcher_pool_alloc re-initializes every payload field on
 * recycle, and the GC root walker skips freed slots (ACTIVE cleared below). */
static void
pool_free(struct UVM *vm, UWatcher *w)
{
    URBI_INTERNAL_ASSERT(w != NULL);
    URBI_INTERNAL_ASSERT(vm->watchers->pool_in_use > 0);

    /* v0.8.4 Step C-3: URBI_WATCHER_OWNS_* flags and their free arms deleted.
     * UClosure lifetime is GC-managed since Step C-2; no closure free (or
     * pointer clearing) happens here — see the function header. */

    /* Clear URBI_WATCHER_ACTIVE so a slab walk (uwatcher_pool_destroy,
     * WATCH-002 v0.5.7) can distinguish allocated-but-orphaned slots from
     * recycled ones. */
    w->flags = (uint8_t)(w->flags & ~(uint8_t)URBI_WATCHER_ACTIVE);

    w->next_active             = vm->watchers->pool_freelist;
    vm->watchers->pool_freelist  = w;
    vm->watchers->pool_in_use--;
}

/* drain_watcher_list: pop every watcher from *head (linked via next_active),
 * decrement watcher_active_count, and return each slot to the freelist via
 * pool_free.  Used by uwatcher_pool_destroy to drain both the active list
 * and the pending-onleave list with identical logic. */
static void
drain_watcher_list(struct UVM *vm, UWatcher **head)
{
    while (*head != NULL) {
        UWatcher *w = *head;
        *head = w->next_active;
        /* SCHED-06: no saturation — every armed watcher was counted at
         * install (cond AND event modes), so an underflow here is a
         * counting bug; masking it is what hid the install/unregister
         * asymmetry for nine milestones. */
        URBI_INTERNAL_ASSERT(vm->watchers->active_count > 0);
        vm->watchers->active_count--;
        pool_free(vm, w);
    }
}

/* === Pool lifecycle === */

int
uwatcher_pool_init(struct UVM *vm)
{
    size_t     slab_bytes;
    UWatcher  *slab;
    uint16_t   i;

    URBI_ASSERT_NOT_ISR(vm);

    /* W2/v0.10.4: vm->watchers is heap-allocated in urbi_vm_init before this
     * call.  If OOM during uwatcher_state_create, vm->watchers is NULL — bail
     * early so we don't deref a NULL pointer writing pool fields. */
    if (vm->watchers == NULL) return -1;

    slab_bytes = (size_t)URBI_WATCHER_POOL_SIZE * sizeof(UWatcher);

    slab = (UWatcher *)vm->alloc_fn(NULL, slab_bytes, vm->alloc_ud);
    if (slab == NULL) return -1;

    /* Zero the entire slab (WATCH-029).  Freestanding builds cannot use
     * memset (libc dep); urbi_zero (runtime/umacros.h) is the canonical
     * helper repeated across all subsystems that need zero-fill at
     * init/recycle time (FOUND-030: the pattern was de-duplicated in
     * Wave 2 / v0.5.4-decompose).  We use the helper here too — the
     * watcher pool slab is the freelist's backing store, allocated
     * fresh per VM init, so byte-zeroing it is correct (clears every
     * UWatcher header to a known-quiescent state including flags ==
     * 0 so uwatcher_pool_destroy's slab walk can tell never-allocated
     * slots from currently-allocated ones, WATCH-002 / WATCH-006). */
    urbi_zero(slab, slab_bytes);

    /* Thread freelist: each slot's next_active points to the next slot;
     * the last slot terminates with NULL. */
    for (i = 0; i < (uint16_t)URBI_WATCHER_POOL_SIZE - 1U; i++) {
        slab[i].next_active = &slab[i + 1U];
    }
    slab[URBI_WATCHER_POOL_SIZE - 1U].next_active = NULL;

    /* Wire pool fields on the VM (defensive zero — urbi_vm_init already did this,
     * but explicit is clearer for future readers). */
    vm->watchers->pool_base      = slab;
    vm->watchers->pool_freelist  = &slab[0];
    vm->active_watchers_head   = NULL;
    vm->watchers->pool_in_use    = 0U;
    vm->watchers->pool_high_water = 0U;

    return 0;
}

void
uwatcher_pool_destroy(struct UVM *vm)
{
    URBI_ASSERT_NOT_ISR(vm);

    /* W2/v0.10.4: vm->watchers may be NULL on OOM partial-init. */
    if (vm->watchers == NULL) return;
    if (vm->watchers->pool_base == NULL) return;
    if (vm->alloc_fn == NULL) return;

    /* Drain active and pending-onleave watcher lists so pool_free marks them
     * recycled (WATCH-001/002 slab-walk invariants) before the slab is freed.
     * Step C-3: OWNS_* flags deleted; pool_free no longer calls alloc_fn on
     * closures — GC sweep handles UClosure lifetime.  Drain is still needed
     * to keep slab-walk accounting correct. */
    drain_watcher_list(vm, &vm->active_watchers_head);
    drain_watcher_list(vm, &vm->pending_onleave_head);
    vm->pending_onleave_tail = NULL;

    /* WATCH-002 + WATCH-006 (v0.5.7): walk the slab for tag-less event-mode
     * watchers (AT_EVENT / AT_EVENT_SYNC / WHENEVER_EVENT) still allocated.
     * Tagged AT_EVENT watchers are torn down via the tag-stop cascade before
     * this function runs; tag-less ones (install_at_event_runtime called with
     * resolve_owning_tag returning NULL) are not on active_watchers_head
     * (only cond watchers walk there), not on pending_onleave_head, and
     * not on any tag's member chain.  Without this slab walk they remain
     * linked to event->at_watchers_head pointing at slab memory we are
     * about to free below.  Walk every slot, identify active event-mode
     * watchers, unlink from event->at_watchers_head, and release the slot.
     * Slots on the freelist have URBI_WATCHER_ACTIVE cleared by pool_free;
     * slots never allocated have flags == 0 (slab pre-zeroed).
     * SCHED-16 (refactor-3): use UWATCHER_IS_EVENT_MODE predicate so that
     * WHENEVER_EVENT is not silently omitted. */
    {
        uint16_t i;
        for (i = 0; i < (uint16_t)URBI_WATCHER_POOL_SIZE; i++) {
            UWatcher *w = &vm->watchers->pool_base[i];
            if ((w->flags & URBI_WATCHER_ACTIVE) == 0U) continue;
            if (!UWATCHER_IS_EVENT_MODE(w->mode)) continue;
            if (w->event != NULL) {
                uevent_at_watchers_remove(w->event, w);
                w->event = NULL;
            }
            /* SCHED-06: assert instead of saturate — see drain_watcher_list. */
            URBI_INTERNAL_ASSERT(vm->watchers->active_count > 0);
            vm->watchers->active_count--;
            pool_free(vm, w);
        }
    }

    vm->alloc_fn(vm->watchers->pool_base, 0, vm->alloc_ud);

    /* Defensive: zero all pool pointers.  pending_onleave_head/tail were
     * already NULL'd above (drain loop's *head = w->next_active terminator
     * + explicit tail = NULL), but explicit zeroing here keeps the invariant
     * robust against future refactors of drain_watcher_list (WATCH-003). */
    vm->watchers->pool_base      = NULL;
    vm->watchers->pool_freelist  = NULL;
    vm->active_watchers_head   = NULL;
    vm->pending_onleave_head   = NULL;
    vm->pending_onleave_tail   = NULL;
}

/* === Unregister ===
 *
 * unregister: scan-on-unregister to clear bit-6 per spec §5.4, unlink from
 *             tag member list, unlink from active list, pool_free, decrement
 *             watcher_active_count.
 *
 * The companion `install` primitive lives in production code as
 * `install_watcher_runtime` / `install_at_event_runtime`
 * (src/watcher/uwatcher_install.c).  See file-header HISTORICAL NOTE
 * for the WATCH-023 test-seam removal. */

void
urbi_watcher_unregister_internal(struct UVM *vm, struct UWatcher *w)
{
    URBI_ASSERT_NOT_ISR(vm);
    URBI_INTERNAL_ASSERT(w != NULL);

    /* Scan-on-unregister: for each cell in this watcher's read-set, walk all
     * OTHER active watchers.  If none of them still observe the cell, clear
     * bit-6.  The scan happens before active-list unlink so the loop correctly
     * skips w itself via the (o == w) guard.  Per spec §5.4. */
    for (size_t i = 0; i < (size_t)w->read_set_count; i++) {
        UCell   *c             = w->cells[i];
        bool     still_observed = false;
        UWatcher *o;

        for (o = vm->active_watchers_head; o != NULL && !still_observed;
             o = o->next_active) {
            uint8_t j;
            if (o == w) continue;
            for (j = 0; j < o->read_set_count; j++) {
                if (o->cells[j] == c) { still_observed = true; break; }
            }
        }
        if (!still_observed) {
            c->gc_byte = (uint8_t)(c->gc_byte & ~(uint8_t)UGC_HAS_WATCHER_OBSERVER);
        }
    }

    /* Unlink from owning tag's member list. */
    if (w->owning_tag != NULL) {
        URBI_SLIST_UNLINK(w->owning_tag->member_watchers_head,
                          w, next_in_tag, UWatcher);
    }

    /* Unlink from the appropriate watcher list depending on mode.
     * Event-mode watchers (AT_EVENT / AT_EVENT_SYNC / WHENEVER_EVENT) live
     * on event->at_watchers_head, not on vm->active_watchers_head (spec #3
     * §6.3).  SCHED-16 (refactor-3): use predicate so WHENEVER_EVENT is not
     * silently omitted. */
    if (UWATCHER_IS_EVENT_MODE(w->mode)) {
        if (w->event) {
            uevent_at_watchers_remove(w->event, w);
            w->event = NULL;
        }
    } else {
        URBI_SLIST_UNLINK(vm->active_watchers_head, w, next_active, UWatcher);
    }

    /* SCHED-06: the count covers ALL armed watchers (cond + event modes,
     * since install_at_event_runtime bumps it too).  Assert > 0 so an
     * install/unregister asymmetry fail-fasts instead of wrapping. */
    URBI_INTERNAL_ASSERT(vm->watchers->active_count > 0);
    vm->watchers->active_count--;
    pool_free(vm, w);
}

/* === observer_dirty — watcher dirty-set hook ===
 *
 * Called by the write barriers in ugc_incremental.h whenever a cell with
 * bit-6 set (UGC_HAS_WATCHER_OBSERVER) is written.  Increments the dirty
 * counter so the scheduler knows to call watcher_eval_dirty on the next
 * safepoint turn.
 *
 * Per spec §5.5: walk-all eval at safepoint; identifying the specific cell or
 * slot key is unnecessary — watcher_eval_dirty visits every active watcher
 * whose read-set might be affected.
 *
 * ISR re-entry guard (WATCH-009): observer_dirty mutates vm->watchers->dirty_count
 * non-atomically; any ISR re-entry that triggers a slot write on a bit-6 cell
 * would corrupt the count under read-modify-write interleaving.  Slot writes
 * are not allowed from ISR context per the URBI_ASSERT_NOT_ISR contract that
 * guards every public-API entry point that mutates state — this assertion
 * is the dirty-set hot-path mirror of that contract. */
void
observer_dirty(struct UVM *vm, UCell *cell, uint32_t key)
{
    URBI_ASSERT_NOT_ISR(vm);
    (void)cell;
    (void)key;
    vm->watchers->dirty_count++;
}
