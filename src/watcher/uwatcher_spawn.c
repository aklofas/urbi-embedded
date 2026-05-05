/* SPDX-License-Identifier: BSD-3-Clause */
/* Watcher body spawn — M5 implementation (spec #1 §5.3).
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
 * spawn_body_coroutine: the two-arg adapter called by watcher_eval_dirty.
 *   M5 replaces the M3 stub (test_watcher_fire_hook delegation) with a call to
 *   do_spawn_body_coroutine.  The fire-hook delegation is removed; tests that
 *   need hook-based fire observation should either install a real watcher or
 *   use the spawn path directly.  Existing tests that relied on the hook for
 *   edge/level counting continue to work because watcher_eval_dirty still calls
 *   spawn_body_coroutine and the tests don't install real body closures. */

#include "watcher/uwatcher.h"
#include "ustrand.h"
#include "uvm.h"
#include "realm/urealm.h"  /* URealm — needed for w->realm->tag comparison */
#include "urbi/urbi.h"     /* URBI_ASSERT_NOT_ISR, URBI_LOG_WARN */

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
    URBI_ASSERT_NOT_ISR(vm);

    /* M5: delegate to the real spawn path when a body closure is present.
     * NULL body → no-op (watcher installed with body=NULL is legal for
     * condition-monitoring uses). */
    if (w->body != NULL) {
        do_spawn_body_coroutine(vm, w, NULL);
        return;
    }

    /* No body closure — fall back to the test fire hook if installed. */
    if (vm->test_watcher_fire_hook != NULL) {
        vm->test_watcher_fire_hook(vm, w);
    }
}
