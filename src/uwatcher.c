/* SPDX-License-Identifier: BSD-3-Clause */
/* UWatcher pool lifecycle + minimal install/unregister stubs.
 * Row 11 / T32.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * All allocation uses vm->alloc_fn (realloc semantics).
 * Zero-fill uses a volatile byte loop — no memset dependency. */

#include "uwatcher.h"
#include "uvm.h"
#include "ugc.h"            /* UTYPE_WATCHER */
#include "ugc_incremental.h" /* UGC_IS_FIXED, current_white access */
#include "urbi.h"           /* URBI_ASSERT_NOT_ISR */
#include "urbi_internal.h"  /* URBI_INTERNAL_ASSERT */

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

/* === Install / unregister stubs (T32 scope) ===
 *
 * T32 minimal: allocate from pool + wire active_watchers_head.
 *
 * TODO(T33): wire read_set entries into cells[] + set bit-6
 *            (UGC_HAS_WATCHER_OBSERVER) on each observed cell.
 * TODO(T33): insert w into owning_tag->member_watchers_head via next_in_tag.
 * TODO(T33): walk read_set[] argument and populate w->cells[]. */

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

    (void)read_set;          /* T33 will wire this */
    (void)read_set_count;    /* T33 will wire this */

    URBI_ASSERT_NOT_ISR(vm);

    w = pool_alloc(vm);
    if (w == NULL) return NULL;

    w->mode       = mode;
    w->owning_tag = owning_tag;
    w->condition  = condition;
    w->body       = body;
    w->onleave    = onleave;
    /* read_set_count is stored but cells[] not populated here — T33's job. */
    w->read_set_count = (read_set_count <= (size_t)URBI_WATCHER_READSET_MAX)
                        ? (uint8_t)read_set_count
                        : (uint8_t)URBI_WATCHER_READSET_MAX;
    w->next_in_tag = NULL;  /* TODO(T33): insert into owning_tag->member_watchers_head */

    /* Insert at head of active_watchers_head list. */
    w->next_active           = vm->active_watchers_head;
    vm->active_watchers_head = w;

    return w;
}

void
urbi_watcher_unregister_internal(struct UVM *vm, struct UWatcher *w)
{
    struct UWatcher **pp;

    URBI_ASSERT_NOT_ISR(vm);
    URBI_INTERNAL_ASSERT(w != NULL);

    /* TODO(T33): unlink from owning_tag->member_watchers_head via next_in_tag. */
    /* TODO(T33): clear bit-6 (UGC_HAS_WATCHER_OBSERVER) from each cell in cells[]. */

    /* Unlink from active_watchers_head via pointer-to-pointer walk. */
    pp = &vm->active_watchers_head;
    while (*pp != NULL) {
        if (*pp == w) {
            *pp = w->next_active;
            break;
        }
        pp = &(*pp)->next_active;
    }

    pool_free(vm, w);
}
