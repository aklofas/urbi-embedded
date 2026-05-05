/* SPDX-License-Identifier: BSD-3-Clause */
/* Watcher table GC root walker.  Row 11.
 *
 * Per spec §6.6: walks vm->active_watchers_head + vm->pending_onleave_head
 * yielding closure + last_value_cache UValues to the GC mark callback.
 *
 * M3 deferrals:
 *  - owning_tag callback: UTag is host-managed via vm->alloc_fn (not GC-managed);
 *    UVAL_TAG kind doesn't exist until M5/M6 promotes it.
 *  - read_set cells[] callback: UCell wrapping into UValue requires concrete
 *    cell types (M4 owns those — UClosure/UNamespace/UString embed UCell). */

#include "uwatcher.h"
#include "uvm.h"
#include "gc/ugc.h"
#ifdef URBI_DEBUG
#include "ustrand.h"
#include "realm/urealm.h"
#include "umacros.h"   /* URBI_INTERNAL_ASSERT */
#endif

void
watcher_table_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx)
{
    /* No URBI_ASSERT_NOT_ISR — root walkers run from GC slice path which
     * is itself ISR-asserted at T26 entry points.  (See sched_walk_roots
     * for precedent — also no per-walker ISR assert.) */

    /* Note (spec #1 §7.1): body_strand and realm are NOT yielded here.
     * Body strands are reached via realm->strands_head (row 10 §3.5).
     * Realms are host-allocated, not GC-managed at v1.0. */

    /* Active watchers */
    for (struct UWatcher *w = vm->active_watchers_head; w != NULL; w = w->next_active) {
        UValue tmp;

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
        /* M5/M6: owning_tag callback once UVAL_TAG kind exists.
         * UTag is host-managed at M3; M5/M6 promotes to UVAL_TAG + GC enrolls. */

        cb(vm, &w->last_value_cache, ctx);

        /* M4: read-set cells callback once concrete cell types embed UCell.
         * At M3, only UVAL_CLOSURE is heap-bearing; cells[] wrapping is moot
         * since no real watchers can be installed from urbiscript (no `at` syntax). */
    }

    /* Pending-onleave queue (watchers being torn down still need their
     * fields walked until the cleanup completes; their onleave closure
     * may still run via drain_pending_onleave_queue). */
    for (struct UWatcher *w = vm->pending_onleave_head; w != NULL; w = w->next_active) {
        UValue tmp;

        if (w->onleave != NULL) {
            tmp.kind = UVAL_CLOSURE;
            tmp.v.p  = (void *)w->onleave;
            cb(vm, &tmp, ctx);
        }
        /* M5/M6: owning_tag, M4: read-set cells. */
        cb(vm, &w->last_value_cache, ctx);
    }
}

#ifdef URBI_DEBUG
/* urbi_watcher_check_invariants: URBI_DEBUG-only bidirectional pointer check.
 * Called at watcher_eval_dirty entry to validate that every active watcher
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
        }
    }
}
#endif /* URBI_DEBUG */
