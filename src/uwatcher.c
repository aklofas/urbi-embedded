/* SPDX-License-Identifier: BSD-3-Clause */
/* UWatcher pool lifecycle + install/unregister + observer_dirty.
 * Row 11 / T33.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * All allocation uses vm->alloc_fn (realloc semantics).
 * Zero-fill uses a volatile byte loop — no memset dependency. */

#include "uwatcher.h"
#include "uvm.h"
#include "ugc.h"            /* UTYPE_WATCHER */
#include "ugc_incremental.h" /* UGC_IS_FIXED, UGC_HAS_WATCHER_OBSERVER, current_white */
#include "utag.h"           /* UTag, member_watchers_head */
#include "urbi/urbi.h"           /* URBI_ASSERT_NOT_ISR */
#include "umacros.h"  /* URBI_INTERNAL_ASSERT */

/* === Internal helpers === */

/* watcher_pool_zero: volatile byte loop — mirrors arena_zero/strand_zero pattern.
 * Never optimised away by the compiler (volatile write barrier). */
static void
watcher_pool_zero(void *base, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)base;
    size_t i;
    for (i = 0; i < n; i++) p[i] = 0;
}

/* pool_alloc: pop one entry from the freelist.
 * Returns NULL if the pool is exhausted.
 * Initialises the common header and clears payload state. */
static UWatcher *
pool_alloc(struct UVM *vm)
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
 * Decrements in_use counter; does NOT touch high_water. */
static void
pool_free(struct UVM *vm, UWatcher *w)
{
    URBI_INTERNAL_ASSERT(w != NULL);
    URBI_INTERNAL_ASSERT(vm->watcher_pool_in_use > 0);

    w->next_active             = vm->watcher_pool_freelist;
    vm->watcher_pool_freelist  = w;
    vm->watcher_pool_in_use--;
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

    /* Zero the entire slab (freestanding: no memset). */
    watcher_pool_zero(slab, slab_bytes);

    /* Thread freelist: each slot's next_active points to the next slot;
     * the last slot terminates with NULL. */
    for (i = 0; i < (uint16_t)URBI_WATCHER_POOL_SIZE - 1u; i++) {
        slab[i].next_active = &slab[i + 1u];
    }
    slab[URBI_WATCHER_POOL_SIZE - 1u].next_active = NULL;

    /* Wire pool fields on the VM (defensive zero — uvm_init already did this,
     * but explicit is clearer for future readers). */
    vm->watcher_pool_base      = slab;
    vm->watcher_pool_freelist  = &slab[0];
    vm->active_watchers_head   = NULL;
    vm->watcher_pool_in_use    = 0u;
    vm->watcher_pool_high_water = 0u;

    return 0;
}

void
uwatcher_pool_destroy(struct UVM *vm)
{
    URBI_ASSERT_NOT_ISR(vm);

    if (vm->watcher_pool_base == NULL) return;
    if (vm->alloc_fn == NULL) return;

    vm->alloc_fn(vm->watcher_pool_base, 0, vm->alloc_ud);

    /* Defensive: zero all pool pointers. */
    vm->watcher_pool_base     = NULL;
    vm->watcher_pool_freelist = NULL;
    vm->active_watchers_head  = NULL;
}

/* === Install / unregister ===
 *
 * install: pool-alloc, wire read-set (cells[] + bit-6), tail-insert into
 *          active_watchers_head (row 12 §2.2 determinism), head-insert into
 *          owning_tag->member_watchers_head, bump watcher_active_count.
 *
 * unregister: scan-on-unregister to clear bit-6 per spec §5.4, unlink from
 *             tag member list, unlink from active list, pool_free, decrement
 *             watcher_active_count. */

UWatcher *
urbi_watcher_install_internal(
    struct UVM       *vm,
    uint8_t           mode,
    struct UTag      *owning_tag,
    UClosure         *condition,
    UClosure         *body,
    UClosure         *onleave,
    UCell           **read_set,
    size_t            read_set_count)
{
    UWatcher *w;
    size_t    i;

    URBI_ASSERT_NOT_ISR(vm);

    /* Guard: overflow check before pool_alloc to avoid wasting a slot. */
    if (read_set_count > (size_t)URBI_WATCHER_READSET_MAX) return NULL;

    w = pool_alloc(vm);
    if (w == NULL) return NULL;

    w->mode       = mode;
    w->owning_tag = owning_tag;
    w->condition  = condition;
    w->body       = body;
    w->onleave    = onleave;
    w->read_set_count = (uint8_t)read_set_count;

    /* Read-set capture: populate cells[] and set bit-6 on each observed cell
     * per spec §5.3.  Caller may pass read_set == NULL when read_set_count == 0. */
    for (i = 0; i < read_set_count; i++) {
        read_set[i]->gc_byte |= UGC_HAS_WATCHER_OBSERVER;
        w->cells[i] = read_set[i];
    }

    /* Tail-insert into active_watchers_head per row 12 §2.2 (install order =
     * eval order; determinism gate relies on this invariant). */
    w->next_active = NULL;
    if (vm->active_watchers_head == NULL) {
        vm->active_watchers_head = w;
    } else {
        UWatcher *tail = vm->active_watchers_head;
        while (tail->next_active != NULL) tail = tail->next_active;
        tail->next_active = w;
    }

    /* Head-insert into owning tag's member_watchers_head per spec §5.4.
     * NULL-guard: tests may pass owning_tag == NULL. */
    if (owning_tag != NULL) {
        w->next_in_tag             = owning_tag->member_watchers_head;
        owning_tag->member_watchers_head = w;
    } else {
        w->next_in_tag = NULL;
    }

    /* Track active count. */
    vm->watcher_active_count++;

    /* Seed last_value_cache with the current condition result per spec §6.3
     * ("at fires on transitions; not on initial truthy state").  The install-
     * time eval seeds the cache but does NOT fire the body: a subsequent dirty
     * pass that re-evaluates and finds new == old == truthy will not fire
     * (no rising edge for AT/AT_SYNC; WHENEVER fires on next dirty pass). */
    w->last_value_cache = invoke_condition_closure(vm, w);

    return w;
}

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

    /* Unlink from active_watchers_head via pointer-to-pointer walk. */
    pp = &vm->active_watchers_head;
    while (*pp != NULL) {
        if (*pp == w) {
            *pp = w->next_active;
            break;
        }
        pp = &(*pp)->next_active;
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
 * slot key is unnecessary at M3 — watcher_eval_dirty (T34) will visit every
 * active watcher whose read-set might be affected. */
void
observer_dirty(struct UVM *vm, UCell *cell, uint32_t key)
{
    (void)cell;
    (void)key;
    vm->watcher_dirty_count++;
}
