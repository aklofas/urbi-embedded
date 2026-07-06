/* SPDX-License-Identifier: BSD-3-Clause */
/* Watcher table GC root walker.
 *
 * Per spec §6.6 as amended by refactor-3 GC-05: walks the whole watcher
 * POOL SLAB (every in-use slot), yielding closure + last_value_cache
 * UValues to the GC mark callback and shading owning_tag + event cells
 * directly.
 *
 * v1.x deferral:
 *  - read_set cells[] callback: cells[] wrapping into UValue requires the
 *    concrete cell types to embed UCell (UClosure/UNamespace/UString do
 *    today; promotion to GC-yielded roots tracked in v1.x backlog). */

#include "uwatcher.h"
#include "vm/uvm.h"
#include "gc/ugc.h"
#include "gc/ugc_incremental.h"   /* urbi_gc_shade_gray (owning_tag + event roots) */
#include <stddef.h>
#include <stdint.h>
#ifdef URBI_DEBUG
#include "sched/ustrand.h"
#include "realm/urealm.h"
#include "runtime/umacros.h"   /* URBI_INTERNAL_ASSERT */
#endif

void
urbi_gc_watcher_table_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx)
{
    /* No URBI_ASSERT_NOT_ISR — root walkers run from the GC slice path,
     * which itself ISR-asserts at its entry points.  (See urbi_gc_sched_walk_roots
     * for precedent — also no per-walker ISR assert.) */

    /* refactor-3 GC-05: POOL-WIDE walk.  Rooting is a property of "slot is
     * in use" (URBI_WATCHER_ACTIVE — set by uwatcher_pool_alloc, cleared
     * only by pool_free), NOT of list topology.  This subsumes the old
     * active-list + pending-onleave walks and — critically — covers
     * AT_EVENT/WHENEVER_EVENT watchers whose owning event has itself
     * become unreachable (previously their closures were rooted only via
     * walk_uevent, i.e. only while the event was independently alive —
     * GC-008 / v1.0-stm32f4-hang; this walk is now the load-bearing single
     * source of truth for that fix).  Legacy semantics restored: an event
     * is immortal while subscribed (w->event shaded below).
     * PENDING_UNREGISTER slots keep ACTIVE set until pool_free, so
     * drained-but-not-yet-freed watchers (whose onleave may still run via
     * urbi_watcher_drain_pending_onleave_queue) stay rooted.
     *
     * The walk must visit ALL URBI_WATCHER_POOL_SIZE slots — no tighter
     * bound exists: in-use slots are non-contiguous under freelist
     * recycling, and pool_high_water is a count, not a max index.
     *
     * Freelist aliasing: free slots reuse next_active as the freelist
     * link, but this walk keys on the ACTIVE flag and never touches
     * next_active, so freelist threading is invisible here.  Never-
     * allocated slots have flags == 0 (slab is urbi_zero'd at
     * uwatcher_pool_init).
     *
     * Note (spec #1 §7.1): body_strand / realm / waiter_strand are NOT
     * yielded here.  Body and waiter strands are reached via
     * realm->strands_head (sched walker yields each strand's frame
     * window).  Realms are host-allocated, not GC-managed at v1.0.
     *
     * read-set cells[]: v1.x deferral — see the file header. */
    if (vm->watchers == NULL || vm->watchers->pool_base == NULL) return;
    for (uint16_t i = 0U; i < (uint16_t)URBI_WATCHER_POOL_SIZE; i++) {
        struct UWatcher *w = &vm->watchers->pool_base[i];
        UValue tmp;

        if ((w->flags & URBI_WATCHER_ACTIVE) == 0U) continue;

        if (w->condition != NULL) {
            tmp.kind = UVAL_CLOSURE;
            tmp.v.p  = (void *)w->condition;
            cb(vm, &tmp, ctx);
        }
        if (w->body != NULL) {
            tmp.kind = UVAL_CLOSURE;
            tmp.v.p  = (void *)w->body;
            cb(vm, &tmp, ctx);
        }
        if (w->onleave != NULL) {
            tmp.kind = UVAL_CLOSURE;
            tmp.v.p  = (void *)w->onleave;
            cb(vm, &tmp, ctx);
        }
        cb(vm, &w->last_value_cache, ctx);

        /* refactor-3 GC-03: owning_tag IS a root — UTag has been
         * GC-managed (UTYPE_TAG via urbi_gc_alloc); the old
         * "host-managed" deferral note was stale. */
        if (w->owning_tag != NULL) {
            urbi_gc_shade_gray(vm, (UCell *)w->owning_tag);
        }
        /* refactor-3 GC-05: the subscribed event must outlive the watcher
         * (w->event is dereferenced at unregister/teardown). */
        if (w->event != NULL) {
            urbi_gc_shade_gray(vm, (UCell *)w->event);
        }
    }
}

#ifdef URBI_DEBUG
/* urbi_watcher_check_invariants: URBI_DEBUG-only bidirectional pointer check.
 * Called at urbi_vm_watcher_eval_dirty entry to validate that every active watcher
 * with a live body_strand satisfies:
 *   1. body_strand->watcher_body_owner == w  (back-pointer consistency).
 *   2. body_strand is reachable on w->realm->strands_head  (GC walker contract).
 * spec #1 §7.2. */
void
urbi_watcher_check_invariants(struct UVM *vm)
{
    for (struct UWatcher *w = vm->active_watchers_head; w != NULL; w = w->next_active) {
        if (w->body_strand != NULL) {
            URBI_INTERNAL_ASSERT(w->body_strand->watcher_body_owner == w);
            /* Body strand must still be on the realm strand list. */
            int found = 0;
            for (struct UStrand *s = w->realm->strands_head; s != NULL; s = s->next_in_realm) {
                if (s == w->body_strand) { found = 1; break; }
            }
            URBI_INTERNAL_ASSERT(found);
            (void)found;  /* read above only via URBI_INTERNAL_ASSERT, which
                           * collapses to ((void)0) in non-DEBUG builds; mark
                           * the variable as intentionally read to silence the
                           * cppcheck unreadVariable warning (CPPCHK-006). */
        }
    }
}
#endif /* URBI_DEBUG */
