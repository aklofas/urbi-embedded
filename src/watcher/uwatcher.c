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
#include "runtime/uclosure.h"  /* UClosure full definition — proto field + URBI_WATCHER_OWNS_* free path */
#include "gc/ugc.h"            /* UTYPE_WATCHER */
#include "gc/ugc_incremental.h" /* UGC_IS_FIXED, UGC_HAS_WATCHER_OBSERVER, current_white */
#include "tag/utag.h"           /* UTag, member_watchers_head */
#include "urbi/urbi.h"           /* URBI_ASSERT_NOT_ISR */
#include "runtime/umacros.h"  /* URBI_INTERNAL_ASSERT */
#include "event/uevent_subscribe.h"   /* uevent_at_watchers_remove */
#include "module/umodule.h"
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

    if (vm->watcher_pool_freelist == NULL) return NULL;

    /* Pop from freelist head. */
    w = vm->watcher_pool_freelist;
    vm->watcher_pool_freelist = w->next_active;

    /* Re-init cell header and clear payload. */
    w->type_tag        = UTYPE_WATCHER;
    /* gc_byte: UGC_IS_FIXED always set; color from vm->current_white. */
    w->gc_byte         = (uint8_t)(vm->current_white | UGC_IS_FIXED);
    w->mode            = 0;
    w->exhaust_policy  = URBI_EXHAUST_QUEUE;
    w->flags           = URBI_WATCHER_ACTIVE;
    w->read_set_count  = 0;
    w->pad0            = 0;
    w->next_active     = NULL;
    w->next_in_tag     = NULL;
    w->owning_tag      = NULL;
    w->condition       = NULL;
    w->body            = NULL;
    w->onleave         = NULL;
    /* Re-init last_value_cache kind+value; UValue._pad bytes are zero on
     * first use (slab-zeroed at pool_init) and irrelevant on recycle since
     * they have no semantic meaning. */
    w->last_value_cache.kind  = UVAL_NIL;
    w->last_value_cache.v.i   = 0;
    /* Clear read-set entries. */
    for (i = 0; i < (uint16_t)URBI_WATCHER_READSET_MAX; i++) {
        w->cells[i] = NULL;
    }

    /* Update pool counters. */
    vm->watcher_pool_in_use++;
    if (vm->watcher_pool_in_use > vm->watcher_pool_high_water) {
        vm->watcher_pool_high_water = vm->watcher_pool_in_use;
    }

    return w;
}

/* pool_free: push one entry back onto the freelist.
 * Decrements in_use counter; does NOT touch high_water.
 * For each of URBI_WATCHER_OWNS_COND / _BODY / _ONLEAVE that is set, frees the
 * matching closure (and its detached proto) before recycling the slot.  These
 * flags are set by install_watcher_runtime / install_at_event_runtime when
 * they unlink the closures from the strand's pre-GC closure_list so
 * urbi_vm_run's post-run cleanup loop cannot free them prematurely.  The
 * three flags are independent — any subset (including none) may be set.
 *
 * WATCH-001 (v0.5.7): each free is structured as {free → null pointer →
 * clear OWNS bit} so the slot's flag byte accurately reflects post-free
 * ownership state.  This is defensive idempotency: if pool_free were ever
 * re-entered on the same slot before pool_alloc recycled it, the cleared
 * bit prevents a double-free; and if a future caller invariant changes to
 * permit closure aliasing across watchers, the cleared bit on the freeing
 * watcher accurately reports that this slot no longer claims ownership. */
static void
pool_free(struct UVM *vm, UWatcher *w)
{
    URBI_INTERNAL_ASSERT(w != NULL);
    URBI_INTERNAL_ASSERT(vm->watcher_pool_in_use > 0);

    /* Free owned closures (and their detached protos) acquired via
     * install_watcher_runtime / install_at_event_runtime. */
    if ((w->flags & URBI_WATCHER_OWNS_COND) && w->condition != NULL) {
        if (w->condition->proto != NULL) {
            /* Proto was detached from module->nested[] by strand_closure_unlink;
             * free its sub-buffers then the struct itself. */
            umodule_destroy_proto_buffers(w->condition->proto,
                                          vm->alloc_fn, vm->alloc_ud);
            vm->alloc_fn(w->condition->proto, 0, vm->alloc_ud);
        }
        vm->alloc_fn(w->condition, 0, vm->alloc_ud);
        w->condition = NULL;
        w->flags = (uint8_t)(w->flags & ~(uint8_t)URBI_WATCHER_OWNS_COND);
    }
    if ((w->flags & URBI_WATCHER_OWNS_BODY) && w->body != NULL) {
        if (w->body->proto != NULL) {
            umodule_destroy_proto_buffers(w->body->proto,
                                          vm->alloc_fn, vm->alloc_ud);
            vm->alloc_fn(w->body->proto, 0, vm->alloc_ud);
        }
        vm->alloc_fn(w->body, 0, vm->alloc_ud);
        w->body = NULL;
        w->flags = (uint8_t)(w->flags & ~(uint8_t)URBI_WATCHER_OWNS_BODY);
    }
    if ((w->flags & URBI_WATCHER_OWNS_ONLEAVE) && w->onleave != NULL) {
        if (w->onleave->proto != NULL) {
            umodule_destroy_proto_buffers(w->onleave->proto,
                                          vm->alloc_fn, vm->alloc_ud);
            vm->alloc_fn(w->onleave->proto, 0, vm->alloc_ud);
        }
        vm->alloc_fn(w->onleave, 0, vm->alloc_ud);
        w->onleave = NULL;
        w->flags = (uint8_t)(w->flags & ~(uint8_t)URBI_WATCHER_OWNS_ONLEAVE);
    }

    /* Clear URBI_WATCHER_ACTIVE so a slab walk (uwatcher_pool_destroy,
     * WATCH-002 v0.5.7) can distinguish allocated-but-orphaned slots from
     * recycled ones.  Per-slot OWNS_* bits were already cleared above. */
    w->flags = (uint8_t)(w->flags & ~(uint8_t)URBI_WATCHER_ACTIVE);

    w->next_active             = vm->watcher_pool_freelist;
    vm->watcher_pool_freelist  = w;
    vm->watcher_pool_in_use--;
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
        vm->watcher_active_count = vm->watcher_active_count > 0
                                   ? vm->watcher_active_count - 1U : 0U;
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
    vm->watcher_pool_base      = slab;
    vm->watcher_pool_freelist  = &slab[0];
    vm->active_watchers_head   = NULL;
    vm->watcher_pool_in_use    = 0U;
    vm->watcher_pool_high_water = 0U;

    return 0;
}

void
uwatcher_pool_destroy(struct UVM *vm)
{
    URBI_ASSERT_NOT_ISR(vm);

    if (vm->watcher_pool_base == NULL) return;
    if (vm->alloc_fn == NULL) return;

    /* Free owned closures (and their detached protos) for any watchers
     * that are still active.  pool_free recycles them onto the freelist,
     * which is fine — we free the whole slab below anyway.  Without this
     * step, any watcher that holds URBI_WATCHER_OWNS_COND / _BODY / _ONLEAVE
     * leaks those closures and protos when the slab is freed.
     *
     * urbi_tag_stop (called from urbi_realm_destroy during urealm_teardown_all,
     * which runs before uwatcher_pool_destroy) moves watchers from
     * active_watchers_head onto pending_onleave_head via
     * pending_onleave_queue_push.  Both lists must be drained here to release
     * all owned closures. */
    drain_watcher_list(vm, &vm->active_watchers_head);
    /* Drain watchers that were moved to the pending-onleave queue by
     * urbi_tag_stop / pending_onleave_queue_push.  These have already been
     * unlinked from active_watchers_head and owning_tag->member_watchers_head,
     * but still hold owned closures (OWNS_COND / OWNS_BODY / OWNS_ONLEAVE
     * flags are unchanged by the push).  pool_free frees those closures. */
    drain_watcher_list(vm, &vm->pending_onleave_head);
    vm->pending_onleave_tail = NULL;

    /* WATCH-002 + WATCH-006 (v0.5.7): walk the slab for tag-less
     * AT_EVENT / AT_EVENT_SYNC watchers that are still allocated.  Tagged
     * AT_EVENT watchers are torn down via the tag-stop cascade before this
     * function runs; tag-less ones (install_at_event_runtime called with
     * resolve_owning_tag returning NULL) are not on active_watchers_head
     * (only cond watchers walk there), not on pending_onleave_head, and
     * not on any tag's member chain.  Without this slab walk they remain
     * linked to event->at_watchers_head pointing at slab memory we are
     * about to free below.  Walk every slot, identify active AT_EVENT
     * mode watchers, unlink from event->at_watchers_head, and release the
     * slot.  Slots on the freelist have URBI_WATCHER_ACTIVE cleared by
     * pool_free; slots never allocated have flags == 0 (slab pre-zeroed). */
    {
        uint16_t i;
        for (i = 0; i < (uint16_t)URBI_WATCHER_POOL_SIZE; i++) {
            UWatcher *w = &vm->watcher_pool_base[i];
            if ((w->flags & URBI_WATCHER_ACTIVE) == 0U) continue;
            if (w->mode != UWATCHER_AT_EVENT &&
                w->mode != UWATCHER_AT_EVENT_SYNC) continue;
            if (w->event != NULL) {
                uevent_at_watchers_remove(w->event, w);
                w->event = NULL;
            }
            vm->watcher_active_count = vm->watcher_active_count > 0
                                       ? vm->watcher_active_count - 1U : 0U;
            pool_free(vm, w);
        }
    }

    vm->alloc_fn(vm->watcher_pool_base, 0, vm->alloc_ud);

    /* Defensive: zero all pool pointers.  pending_onleave_head/tail were
     * already NULL'd above (drain loop's *head = w->next_active terminator
     * + explicit tail = NULL), but explicit zeroing here keeps the invariant
     * robust against future refactors of drain_watcher_list (WATCH-003). */
    vm->watcher_pool_base      = NULL;
    vm->watcher_pool_freelist  = NULL;
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
    struct UWatcher **pp;
    size_t i;

    URBI_ASSERT_NOT_ISR(vm);
    URBI_INTERNAL_ASSERT(w != NULL);

    /* Scan-on-unregister: for each cell in this watcher's read-set, walk all
     * OTHER active watchers.  If none of them still observe the cell, clear
     * bit-6.  The scan happens before active-list unlink so the loop correctly
     * skips w itself via the (o == w) guard.  Per spec §5.4. */
    for (i = 0; i < (size_t)w->read_set_count; i++) {
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

    /* Unlink from owning tag's member list (pointer-to-pointer). */
    if (w->owning_tag != NULL) {
        UWatcher **prev = &w->owning_tag->member_watchers_head;
        while (*prev != NULL && *prev != w) prev = &(*prev)->next_in_tag;
        if (*prev != NULL) *prev = w->next_in_tag;
    }

    /* Unlink from the appropriate watcher list depending on mode.
     * AT_EVENT / AT_EVENT_SYNC watchers live on event->at_watchers_head,
     * not on vm->active_watchers_head (spec #3 §6.3). */
    if (w->mode == UWATCHER_AT_EVENT || w->mode == UWATCHER_AT_EVENT_SYNC) {
        if (w->event) {
            uevent_at_watchers_remove(w->event, w);
            w->event = NULL;
        }
    } else {
        /* Unlink from active_watchers_head via pointer-to-pointer walk. */
        pp = &vm->active_watchers_head;
        while (*pp != NULL) {
            if (*pp == w) {
                *pp = w->next_active;
                break;
            }
            pp = &(*pp)->next_active;
        }
    }

    vm->watcher_active_count--;
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
 * ISR re-entry guard (WATCH-009): observer_dirty mutates vm->watcher_dirty_count
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
    vm->watcher_dirty_count++;
}
