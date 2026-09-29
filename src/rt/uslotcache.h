/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uslotcache.h — one cache entry per slot-access site.
 *
 * Keyed on the receiver OBJECT, not on a shape: an entry answers "the
 * last time this site saw this receiver, the slot was at owner[index]".
 * An own-slot entry (owner == recv) is checked against the live receiver
 * and so can never be stale.  An inherited entry is checked against the
 * VM's slot epoch; see uobj.h for what bumps it.
 *
 * Entries are weak.  The collector does not trace them; it bumps the
 * epoch instead, which retires every inherited entry at once. */

#ifndef URT_SLOTCACHE_H
#define URT_SLOTCACHE_H
#include "rt/uobj.h"
#include "chunk/uproto.h"

typedef struct USlotCache {
    UObject *recv;
    UObject *owner;
    uint32_t epoch;
    uint16_t index;
} USlotCache;

USlotCache *uslotcache_alloc(struct UVM *vm, UProto *p);
void        uslotcache_free_tree(struct UVM *vm, UProto *root);
/* Empties every entry in the tree without freeing the arrays. */
void        uslotcache_clear_tree(struct UVM *vm, UProto *root);

/* A collection that finds the epoch past this resets it to 1 and clears
 * every entry, so a stale inherited entry can never meet its own epoch
 * again after a wrap.  Half the range because the check runs only at
 * collections: the structural bumps between two collections cannot
 * plausibly reach 2^31, so the reset always fires before a wrap.  The
 * residual is a program making about 2^31 structural edits to cached
 * objects with no collection in between. */
#define USLOTCACHE_EPOCH_RESET 0x80000000u

static inline USlotCache *uslotcache_site(struct UVM *vm, UProto *p, uint16_t site) {
    USlotCache *a = (USlotCache *)p->site_cache;
    if (a == NULL) { a = uslotcache_alloc(vm, p); if (a == NULL) return NULL; }
    return &a[site];
}

static inline bool uslotcache_hit(const UObjStats *st, const USlotCache *e,
                                  const UObject *recv, const USym *name) {
    if (e->recv != recv) return false;
    if (e->owner == recv) return e->index < recv->count && recv->names[e->index] == name;
    return e->epoch == st->slot_epoch;
}

static inline void uslotcache_fill_own(struct UVM *vm, USlotCache *e, UObject *recv, uint16_t index) {
    e->recv = recv; e->owner = recv; e->index = index; e->epoch = 0;
    uvm_objstats(vm)->cache_fills++;
}

static inline void uslotcache_fill_inherited(struct UVM *vm, USlotCache *e, UObject *recv,
                                             const UObjSlotRef *ref) {
    UObjStats *st = uvm_objstats(vm);
    e->recv = recv; e->owner = ref->owner; e->index = (uint16_t)ref->index;
    e->epoch = st->slot_epoch;
    st->cache_fills++;
}

#if defined(URBI_SLOT_CACHE_VERIFY) && URBI_SLOT_CACHE_VERIFY
#define USLOTCACHE_VERIFY(vm, o, name, e) do { \
        UObjSlotRef vr_; \
        if (!uobj_resolve((vm), (o), (name), &vr_) || vr_.owner != (e)->owner \
            || vr_.index != (int)(e)->index) __builtin_trap(); \
    } while (0)
#else
#define USLOTCACHE_VERIFY(vm, o, name, e) do { } while (0)
#endif

#endif
