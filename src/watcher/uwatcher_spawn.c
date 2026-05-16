/* SPDX-License-Identifier: BSD-3-Clause */
/* Watcher body spawn — M5 implementation (spec #1 §5.2–§5.3).
 *
 * do_spawn_body_coroutine: the real three-step spawn sequence.
 *   1. urbi_strand_create — OOM → log warn, return; watcher stays installed.
 *   2. urbi_strand_attach_ambient_tags (owning_tag != realm->tag only) — on
 *      overflow the strand goes DEAD; detect, destroy, log warn, return.
 *   3. urbi_strand_arm_from_closure — OOM → destroy, log warn, return.
 *   4. Wire back-pointers: body->watcher_body_owner = w; w->body_strand = body.
 *   5. urbi_strand_start — DORMANT → READY (enqueues on run-queue).
 *
 * fire_context is the value / pattern context from the event that triggered
 * this spawn.  M5 baseline always passes NULL; spec #2 wires patterns later.
 *
 * spawn_body_coroutine: eval-pass entry (spec #1 §5.2).
 *   Called by watcher_eval_dirty when condition fires and w->body != NULL.
 *   In URBI_DEBUG builds, asserts: in_watcher_eval == 1 (eval-pass contract),
 *   AT/WHENEVER mode, ACTIVE flag, no PENDING_UNREGISTER, body and realm non-NULL.
 *   The test_watcher_fire_hook delegation for body-less watchers has moved to
 *   watcher_eval_dirty (test-only path; watcher_eval_dirty only calls this
 *   function when w->body != NULL).
 *
 * respawn_body_coroutine: completion-path entry (spec #1 §5.2).
 *   Called when a body strand reaches DEAD and pending_refire_count > 0.
 *   Shares do_spawn_body_coroutine but omits the in_watcher_eval assert
 *   (completion path runs outside eval, after strand-DEAD notification).
 *   Static in production builds; tests reach it via extern declaration. */

#include "watcher/uwatcher.h"
#include "sched/ustrand.h"
#include "vm/uvm.h"
#include "runtime/uclosure.h"              /* UClosure full definition — proto_inst field access */
#include "realm/urealm.h"          /* URealm — needed for w->realm->tag comparison */
#include "object/umodule_instance.h" /* UModuleInstance / UProtoInstanceArr — module_instance wiring */
#include "urbi/urbi.h"             /* URBI_ASSERT_NOT_ISR, URBI_LOG_WARN */
#include "runtime/umacros.h"               /* URBI_INTERNAL_ASSERT */
#include <stddef.h>
#include <stdint.h>

#ifdef URBI_DEBUG
/* uprotoinstance_arr_is_contiguous: debug-only invariant check.
 *
 * UProtoInstanceArr.entries[] is a C99 flexible array — its elements are
 * always contiguous in memory by language guarantee.  This helper makes
 * that invariant explicit at the call sites that depend on it (notably
 * the cross-module_instance pointer-range walk in
 * do_spawn_body_coroutine, WATCH-004).  Tautological by construction;
 * its purpose is documentation + future-proofing if entries[] ever
 * changes shape. */
static int
uprotoinstance_arr_is_contiguous(const UProtoInstanceArr *arr)
{
    if (arr == NULL) return 1;          /* vacuously contiguous */
    if (arr->n == 0) return 1;          /* empty array */
    /* Flexible-array contract: &entries[i] == &entries[0] + i for all i. */
    return (&arr->entries[arr->n - 1U] == &arr->entries[0] + (arr->n - 1U));
}
#endif

void
do_spawn_body_coroutine(struct UVM *vm, struct UWatcher *w, void *fire_context)
{
    struct UStrand *body;

    URBI_ASSERT_NOT_ISR(vm);

    /* Step 1 (spec #1 §5.3 step 1): exhaust-policy gate.
     * If a body strand is already running, queue a pending refire (bounded
     * by w->max_refire_queue) or drop, depending on exhaust_policy.
     *
     * The counter is incremented up to the cap; firings beyond the cap are
     * silently dropped — same end-state as URBI_EXHAUST_DROP for the
     * excess events.  At body completion, urbi_watcher_body_completed
     * decrements the counter and respawns once; this drains the queue at
     * the pace of body execution.  Counter started life as a single
     * URBI_WATCHER_PENDING_REFIRE flag bit and was widened in v0.7.x to
     * a uint8_t to fix the "delta-2-per-drain" cap that surfaced when
     * uevent_ring_drain delivered a burst of N events in a single pass. */
    if (w->body_strand != NULL) {
        if (w->exhaust_policy == URBI_EXHAUST_QUEUE) {
            if (w->pending_refire_count < w->max_refire_queue) {
                w->pending_refire_count++;
            }
            /* else: silent saturation; next refire happens on drain via
             * the queued bodies that DO get to run.  An overflow counter
             * could go here in the future for embedder-visible diagnostics. */
        }
        /* URBI_EXHAUST_DROP: silent drop — fall through to return. */
        return;
    }

    /* Step 2: allocate body strand (DORMANT). */
    body = urbi_strand_create(w->realm, w->body);
    if (!body) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, URBI_LOG_WARN,
                "watcher body spawn: out of memory (strand alloc)");
        return;
    }

    /* Step 3: inherit owning_tag only when distinct from realm->tag.
     * urbi_strand_create already attaches realm->tag at depth 0; if
     * owning_tag == realm->tag there is nothing extra to push.
     * On cleanup-stack overflow, attach sets state = DEAD; detect and tear down. */
    if (w->owning_tag != NULL && w->owning_tag != w->realm->tag) {
        struct UTag *chain[1];
        chain[0] = w->owning_tag;
        urbi_strand_attach_ambient_tags(body, chain, 1);
        if (body->state == USTRAND_STATE_DEAD) {
            urbi_strand_destroy(body);
            if (vm->host_log_fn)
                vm->host_log_fn(vm, URBI_LOG_WARN,
                    "watcher body spawn: ambient-attach overflow");
            return;
        }
    }

    /* Step 4: arm — allocates register stack and wires pc/R/frame_count. */
    if (urbi_strand_arm_from_closure(body, w->body) != 0) {
        urbi_strand_destroy(body);
        if (vm->host_log_fn)
            vm->host_log_fn(vm, URBI_LOG_WARN,
                "watcher body spawn: out of memory (stack alloc)");
        return;
    }

    /* Step 4b: wire module_instance so OP_GETSLOT/SETSLOT at frame_count==0
     * can resolve the IC table for the body closure.
     *
     * The body closure was created by OP_CLOSURE during the watcher-install
     * script run, at which point OP_CLOSURE set cl->proto_inst to point at
     * an entry inside the installing script's UProtoInstanceArr.  That array
     * lives in a UModuleInstance on vm->module_instances_head (permanent for
     * the VM's lifetime).  Walk the list to find the owning UModuleInstance
     * by pointer-range comparison, then set body->module_instance to it.
     *
     * OP_GETSLOT at frame_count==0 reads
     *   s->module_instance->proto_instances->entries[0]
     * which is the root-chunk IC table for the body's module — correct for
     * any body closure that uses globals (Realm.x, Realm.fired, etc.). */
    if (w->body->proto_inst != NULL) {
        UModuleInstance *mi;
        for (mi = vm->module_instances_head; mi != NULL; mi = mi->next_in_vm) {
            UProtoInstanceArr *arr = mi->proto_instances;
            if (arr == NULL || arr->n == 0) continue;
            /* Cross-module-instance pointer comparison: the range walk
             * relies on UProtoInstance entries[] being contiguous within
             * each module_instance's array.  Flexible-array language
             * guarantee ensures this; the URBI_DEBUG assertion makes
             * the invariant explicit (WATCH-004).  v1.x backlog item
             * "UClosure.owning_mi field" removes the need for this walk. */
#ifdef URBI_DEBUG
            URBI_INTERNAL_ASSERT(uprotoinstance_arr_is_contiguous(arr));
#endif
            /* Check if w->body->proto_inst falls within arr->entries[0..n-1].
             * Both pointers are within the same GC-managed bulk allocation
             * so pointer comparison is valid. */
            const UProtoInstance *first = &arr->entries[0];
            const UProtoInstance *last  = &arr->entries[arr->n - 1U];
            if (w->body->proto_inst >= first && w->body->proto_inst <= last) {
                body->module_instance = mi;
                break;
            }
        }
    }

    /* Step 5: wire back-pointers. */
    (void)fire_context;   /* M5 baseline: NULL; spec #2 wires patterns */
    body->watcher_body_owner = w;
    w->body_strand           = body;

    /* Step 6: enqueue on run-queue (DORMANT → READY). */
    urbi_strand_start(body);
}

void
spawn_body_coroutine(struct UVM *vm, struct UWatcher *w)
{
#ifdef URBI_DEBUG
    URBI_ASSERT_NOT_ISR(vm);
    URBI_INTERNAL_ASSERT(vm->in_watcher_eval == 1);
    URBI_INTERNAL_ASSERT(w != NULL);
    URBI_INTERNAL_ASSERT(w->mode == UWATCHER_AT || w->mode == UWATCHER_WHENEVER);
    URBI_INTERNAL_ASSERT((w->flags & URBI_WATCHER_ACTIVE) != 0);
    URBI_INTERNAL_ASSERT((w->flags & URBI_WATCHER_PENDING_UNREGISTER) == 0);
    URBI_INTERNAL_ASSERT(w->body != NULL);
    URBI_INTERNAL_ASSERT(w->realm != NULL);
#endif
    do_spawn_body_coroutine(vm, w, NULL);
}

/* respawn_body_coroutine: completion-path entry (spec #1 §5.2).
 * Not in the public include/urbi/ API; tests reach it via extern declaration
 * in uwatcher.h or directly.  Not static — must be linkable from test code. */
void
respawn_body_coroutine(struct UVM *vm, struct UWatcher *w)
{
#ifdef URBI_DEBUG
    URBI_ASSERT_NOT_ISR(vm);
    /* No in_watcher_eval assert — completion path runs outside eval. */
    URBI_INTERNAL_ASSERT(w != NULL);
    URBI_INTERNAL_ASSERT(w->mode == UWATCHER_AT || w->mode == UWATCHER_WHENEVER);
    URBI_INTERNAL_ASSERT((w->flags & URBI_WATCHER_ACTIVE) != 0);
    URBI_INTERNAL_ASSERT((w->flags & URBI_WATCHER_PENDING_UNREGISTER) == 0);
    URBI_INTERNAL_ASSERT(w->body != NULL);
    URBI_INTERNAL_ASSERT(w->realm != NULL);
#endif
    do_spawn_body_coroutine(vm, w, NULL);
}

/* urbi_watcher_body_completed: strand-DEAD notification (spec #1 §6.2).
 * Called by the dispatcher when a body strand reaches DEAD state. */
void
urbi_watcher_body_completed(struct UVM *vm, struct UStrand *s)
{
#ifdef URBI_DEBUG
    URBI_ASSERT_NOT_ISR(vm);
#endif
    struct UWatcher *w = s->watcher_body_owner;
#ifdef URBI_DEBUG
    URBI_INTERNAL_ASSERT(w != NULL);
    URBI_INTERNAL_ASSERT(w->body_strand == s);
#endif

    if (s->fatal_status == UEXEC_THROW) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, URBI_LOG_WARN,
                "watcher body uncaught throw");
        /* TODO(M6): include throw value's string repr (Object.toString) */
    }
    /* TAG_STOP, CANCEL, UEXEC_OK: silent */

    /* Snapshot completion status before clearing back-pointers so the
     * host-callback hook (below) sees the original strand state. */
    int completion_status = (int)s->fatal_status;

    s->watcher_body_owner = NULL;
    w->body_strand        = NULL;

    /* T33 (v0.7.0 Wave 1): fire body-done callback if installed.  After
     * internal cleanup (back-pointers cleared) so observers see a
     * consistent state; before any re-spawn so re-spawn-aware embedders
     * can correlate the completion event with the subsequent fresh body
     * strand on a later callback.  Handle is a Wave 1 placeholder (0);
     * Wave 2 (ESP-IDF port) defines the real watcher-identity meaning. */
    if (vm->watcher_body_done_fn != NULL) {
        vm->watcher_body_done_fn(vm, /* handle */ 0, completion_status);
    }

    if ((w->flags & URBI_WATCHER_PENDING_UNREGISTER) != 0) {
        w->pending_refire_count = 0;
        return;
    }
    if (w->pending_refire_count > 0) {
        w->pending_refire_count--;
        respawn_body_coroutine(vm, w);
    }
}
