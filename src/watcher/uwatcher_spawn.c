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
 *   Called when a body strand reaches DEAD and PENDING_REFIRE is set.
 *   Shares do_spawn_body_coroutine but omits the in_watcher_eval assert
 *   (completion path runs outside eval, after strand-DEAD notification).
 *   Static in production builds; tests reach it via extern declaration. */

#include "watcher/uwatcher.h"
#include "ustrand.h"
#include "uvm.h"
#include "realm/urealm.h"  /* URealm — needed for w->realm->tag comparison */
#include "urbi/urbi.h"     /* URBI_ASSERT_NOT_ISR, URBI_LOG_WARN */
#include "umacros.h"       /* URBI_INTERNAL_ASSERT */

void
do_spawn_body_coroutine(struct UVM *vm, struct UWatcher *w, void *fire_context)
{
    struct UStrand *body;

    URBI_ASSERT_NOT_ISR(vm);

    /* Step 1 (exhaust-policy check) deferred to T26 — happy path here. */

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
