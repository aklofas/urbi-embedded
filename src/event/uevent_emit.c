/* SPDX-License-Identifier: BSD-3-Clause */
/* Event emit primitives — async + sync + waituntil (spec #3 §5.2-§5.4, §7.1). */

#include "event/uevent_emit.h"
#include "event/uevent.h"
#include "sched/ustrand.h"
#include "vm/uvm.h"
#include "watcher/uwatcher.h"  /* do_spawn_body_coroutine, UWATCHER_AT_EVENT* */
#include "runtime/uscratch.h"  /* urbi_run_closure_on_scratch_with_payload */
#include "sched/usched_cooperative.h"  /* urbi_sched_strand_make_runnable, urbi_sched_strand_block */
#include "runtime/umacros.h"   /* URBI_INTERNAL_ASSERT (URBI_DEBUG-gated) */
#include "runtime/ulist.h"     /* URBI_SLIST_UNLINK, URBI_SLIST_FOREACH_SAFE */
#include "urbi/urbi.h"         /* URBI_ASSERT_NOT_ISR, URBI_LOG_WARN */
#include <stddef.h>

/* === uevent_waiter_unregister (spec #3 §6.4) ===
 *
 * Splice s out of e->waiters_head when a strand on USTRAND_WAIT_EVENT
 * transitions out for a non-emit reason (tag-stop, cancel, panic).
 *
 * Idempotent: if wait_event_target is NULL the strand is not on any waiter
 * chain (either never parked, or already woken by an emit), so return early.
 *
 * last_event_payload is left NIL — the caller resumes with NIL on
 * cancellation, which stdlib interprets as "wait interrupted". */
void
uevent_waiter_unregister(struct UStrand *s)
{
    struct UEvent *e;

    if (!s->wait_event_target) return;

    e = s->wait_event_target;
    /* s is guaranteed on the list when wait_event_target is set (cooperative
     * single-threaded invariant); URBI_SLIST_UNLINK is a no-op if somehow not
     * found, which is safe. */
    URBI_SLIST_UNLINK(e->waiters_head, s, next_event_waiter, struct UStrand);

    s->wait_event_target = NULL;
    s->next_event_waiter = NULL;
    /* last_event_payload stays NIL — caller resumes with NIL on cancellation. */
}

/* === wake_event_waiters (file-static) ===
 *
 * Walk e->waiters_head FIFO, deposit payload into each strand's
 * last_event_payload, clear the wait fields, and transition to RUNNABLE.
 * Shared by c_event_emit_async and c_event_emit_sync. */
static void
wake_event_waiters(struct UVM *vm, struct UEvent *e, UValue payload)
{
    struct UStrand *s, *ns;
    (void)vm;  /* reserved: preemptive-scheduler upgrade will use vm */
    URBI_SLIST_FOREACH_SAFE(s, ns, e->waiters_head, next_event_waiter) {
        s->last_event_payload = payload;
        s->wait_event_target  = NULL;
        s->next_event_waiter  = NULL;
        urbi_sched_strand_make_runnable(s);
    }
    e->waiters_head = NULL;
}

/* === c_event_emit_async (spec #3 §5.2) ===
 *
 * Fan out payload to all subscribers in FIFO registration order.
 *
 * at_watchers_head (AT_EVENT / AT_EVENT_SYNC modes): spawn body coroutine
 *   via do_spawn_body_coroutine.  fire_context is NULL at M5 baseline —
 *   the fire payload threads into body R[0] via T53's opcode binding.
 *
 * waiters_head (UStrand chain via next_event_waiter): deposit payload,
 *   clear wait fields, transition to RUNNABLE, enqueue. */
void
c_event_emit_async(struct UVM *vm, struct UEvent *e, UValue payload)
{
    URBI_TP(vm, URBI_TRACE_EVENT, URBI_LOG_DEBUG, URBI_TP_EVENT_EMIT,
            (uint32_t)(uintptr_t)e, 0);
    URBI_PERF_INC(vm, event_emits);

    /* EMITR-012: ISR re-entry would be unsafe here because do_spawn_body_coroutine
     * allocates a fresh UStrand from the scheduler's strand pool (potentially
     * via urbi_gc_alloc) and links it onto the runnable queue.  ISR-safe event
     * delivery uses the SPSC ring (uevent_ring) drained from the main loop,
     * which routes back through this function on the main thread.  An ISR
     * caller that hits this path would race with the main-thread strand
     * allocator and corrupt the runnable queue. */
    URBI_ASSERT_NOT_ISR(vm);

    /* SCHED-18 (refactor-3): pre-registered-subscribers-only pin.  Capture
     * the tail of at_watchers_head before the walk.  Any watcher appended
     * mid-emit (e.g. from a C callback or a sync-body hook) lands after this
     * tail and must NOT receive the in-flight emission.  The next pointer for
     * w==last is forced to NULL instead of reading w->next_in_event, which
     * could race with a concurrent append. */
    {
        struct UWatcher *last = e->at_watchers_head;
        if (last) while (last->next_in_event) last = last->next_in_event;

        /* Walk at_watchers_head FIFO up to (and including) last. */
        struct UWatcher *w = e->at_watchers_head;
        while (w) {
            struct UWatcher *next = (w == last) ? NULL : w->next_in_event;
            /* W2/v0.10.2 defence in depth: skip watchers whose tag-stop cascade
             * pushed them to the pending-onleave queue (URBI_WATCHER_PENDING_UNREGISTER
             * set in pending_onleave_queue_push Step 1).  The synchronous unlink in
             * Step 3b normally removes them from at_watchers_head before any emit
             * fires; this guard protects against future code paths that bypass the
             * synchronous unlink.  Closes reactive audit F2. */
            if (w->flags & URBI_WATCHER_PENDING_UNREGISTER) {
                w = next;
                continue;
            }
            /* SCHED-16 (refactor-3): use predicate so WHENEVER_EVENT is not
             * silently omitted from the dispatch check.
             * W9/v0.10.5: pass &payload so do_spawn_body_coroutine writes it
             * into body->R[0] before enqueue.  The body closure was compiled
             * with a 1-param function literal (param name == user's `var x` or
             * `__payload` default); R[0] is the payload parameter slot.
             * UWATCHER_WHENEVER_EVENT (W0/v0.10.2): same dispatch path as
             * AT_EVENT — body spawned as a coroutine.  The "re-fires on every
             * emission" semantic is automatic because WHENEVER_EVENT watchers
             * are never removed from at_watchers_head (no one-shot teardown). */
            if (UWATCHER_IS_EVENT_MODE(w->mode))
                do_spawn_body_coroutine(vm, w, (void *)&payload);
            w = next;
        }
    }

    /* Wake all waiters_head FIFO: deposit payload + transition to RUNNABLE. */
    wake_event_waiters(vm, e, payload);
}

/* === run_event_body_on_scratch (file-static, spec #3 §5.3) ===
 *
 * Run w->body synchronously on the watcher scratch frame with R[0] = payload.
 * Sets in_watcher_scratch for the duration; logs + suppresses throws.
 *
 * Throws cannot propagate from a sync emit — spec §5.4 contract is fail-soft
 * and warn (the emit caller is unaware of subscriber bodies and cannot
 * meaningfully handle their exceptions).
 *
 * Re-entry guard asymmetry vs. the eval-pass / drain wires:
 * This site sets vm->watchers->in_scratch explicitly, while the eval-pass
 * wires (invoke_body_inline / invoke_onleave_inline) and the drain wire
 * (run_watcher_onleave) rely on caller-owned vm->watchers->in_eval for
 * re-entry protection. Reason: c_event_emit_sync may be called from
 * contexts that haven't already entered watcher-eval (e.g., a synchronous
 * emit invoked from main code or from a host C callback), so this
 * function owns its own re-entry flag. The four wired sites otherwise
 * share the same urbi_run_closure_on_scratch[_with_payload] primitive. */
static void
run_event_body_on_scratch(struct UVM *vm, struct UWatcher *w, UValue payload)
{
    /* EMITR-003 contract assertion: the sole call site (c_event_emit_sync)
     * already short-circuits to async when in_watcher_scratch is set, so
     * this entry MUST observe in_watcher_scratch == 0 in correct builds.
     * The early-return below is defensive belt-and-suspenders against a
     * future second caller that does not pre-check; the assertion catches
     * such a regression in URBI_DEBUG builds before the silent skip. */
    URBI_INTERNAL_ASSERT(!vm->watchers->in_scratch);

    /* Defensive guard: never re-enter scratch execution from within scratch.
     * Load-bearing only when triggered (covered by the assert above in
     * URBI_DEBUG); kept in release for safety. */
    if (vm->watchers->in_scratch) return;

    vm->watchers->in_scratch = 1;

    /* Real bytecode dispatch on the scratch frame with R[0] = payload.
     * Throws are suppressed (sync emit caller cannot propagate; spec
     * §5.4 contract is fail-soft + warn). */
    UValue out = {0};
    int    threw = 0;
    (void)urbi_run_closure_on_scratch_with_payload(vm, w->body, payload,
                                                    &out, &threw);
    if (threw && vm->host_log_fn) {
        vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
            "at sync(e?) body threw; suppressed");
    }

    vm->watchers->in_scratch = 0;

    /* SCHED-18 test seam: fires after in_scratch is cleared so the hook can
     * call install_at_event_runtime without triggering the in_scratch guard.
     * NULL in production builds (UTestHooks fields zero-initialised). */
    if (vm->test_hooks && vm->test_hooks->after_sync_body)
        vm->test_hooks->after_sync_body(vm, w);
}

/* === c_event_emit_sync (spec #3 §5.3 + §5.4) ===
 *
 * Synchronous variant: AT_EVENT_SYNC subscribers run inline on the scratch
 * frame before this function returns.  AT_EVENT subscribers spawn strands.
 * Waiters are woken identically to the async path.
 *
 * Top-of-function guard: if any scratch-context flag is set, the entire call
 * degrades to async with a one-shot URBI_LOG_WARN (spec §5.4 / S23 contract). */
void
c_event_emit_sync(struct UVM *vm, struct UEvent *e, UValue payload)
{
    /* EMITR-012: ISR re-entry would be unsafe here because the sync path
     * may run subscriber bodies inline on the watcher scratch frame
     * (run_event_body_on_scratch), which dispatches arbitrary bytecode
     * including allocator-touching opcodes (OP_NEW_OBJECT, OP_GETSLOT
     * slow-path) and may call host_log_fn.  ISR contexts must use the
     * uevent_ring SPSC enqueue path; the main-loop drainer eventually
     * reaches this function with no ISR re-entry possible. */
    URBI_ASSERT_NOT_ISR(vm);

    if (vm->watchers->in_scratch || vm->watchers->in_eval || vm->watchers->in_install) {
        /* EMITR-005: one-shot warn (mirrors urbi_emit_slot_change_slow's
         * slot_change_reentrancy_warned shape).  Pre-fix the warn fired on
         * every degraded call; in a tight loop that flooded host_log_fn. */
        if (!vm->event_sync_degradation_warned) {
            vm->event_sync_degradation_warned = 1;
            if (vm->host_log_fn)
                vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                    "sync emit on event degraded to async (nested in scratch context)");
        }
        c_event_emit_async(vm, e, payload);
        return;
    }

    /* Walk at_watchers_head: sync subs run inline; async subs spawn.
     * UWATCHER_WHENEVER_EVENT (W0/v0.10.2) follows the AT_EVENT async path:
     * spawn a body coroutine.  It is never sync (urbi_parse_whenever disallows
     * the sync modifier).  Re-fire on every emission is automatic because
     * WHENEVER_EVENT watchers are never removed from at_watchers_head.
     *
     * SCHED-18 (refactor-3): pre-registered-subscribers-only pin.  Capture
     * the tail of at_watchers_head before the walk so that any watcher
     * installed mid-emit (from a sync body via run_event_body_on_scratch →
     * after_sync_body hook, or from an AT_EVENT_SYNC subscriber body) does
     * NOT receive the in-flight emission. */
    {
        struct UWatcher *last = e->at_watchers_head;
        if (last) while (last->next_in_event) last = last->next_in_event;

        struct UWatcher *w = e->at_watchers_head;
        while (w) {
            struct UWatcher *next = (w == last) ? NULL : w->next_in_event;
            /* W2/v0.10.2 defence in depth: skip watchers pending tag-stop drain.
             * Mirrors the same guard in c_event_emit_async.  Closes reactive F2. */
            if (w->flags & URBI_WATCHER_PENDING_UNREGISTER) {
                w = next;
                continue;
            }
            /* SCHED-16 (refactor-3): use IS_EVENT_MODE predicate.
             * AT_EVENT_SYNC runs inline; AT_EVENT and WHENEVER_EVENT spawn.
             * W9/v0.10.5: pass &payload for body R[0] delivery. */
            if (w->mode == UWATCHER_AT_EVENT_SYNC) {
                run_event_body_on_scratch(vm, w, payload);
            } else if (UWATCHER_IS_EVENT_MODE(w->mode)) {
                do_spawn_body_coroutine(vm, w, (void *)&payload);
            }
            w = next;
        }
    }

    /* Wake waiters — identical to async path. */
    wake_event_waiters(vm, e, payload);
}

/* === c_event_waituntil (spec #3 §7.1) ===
 *
 * Tail-appends the calling strand to e->waiters_head and parks it via
 * urbi_sched_strand_block(USTRAND_REASON_EVENT) — block owns the WAIT_EVENT
 * (0x33) transition and the runnable-count decrement (SCHED-01).
 *
 * Callers that dispatch via the bytecode loop (T53 opcode binding) MUST
 * goto exit_strand after this call returns — the strand is now WAITING
 * and must not consume further opcodes until woken by c_event_emit_*.
 *
 * On wake: c_event_emit_* deposits last_event_payload before calling
 * urbi_sched_strand_make_runnable.  The T53 opcode binding reads the payload
 * from s->last_event_payload after the strand resumes execution.
 *
 * Scratch-context guard: calling from within a watcher scratch or eval
 * frame is undefined (can deadlock or corrupt scratch state).  Returns
 * NIL + URBI_LOG_WARN if in_watcher_scratch or in_watcher_eval is set. */
UValue
c_event_waituntil(struct UVM *vm, struct UEvent *e)
{
    struct UStrand *s;
    UValue payload = {0};   /* EMITR-007: file convention is `{0}` for NIL. */

    /* EMITR-012: ISR re-entry would be unsafe here because waituntil parks
     * the calling strand (vm->cur_strand) onto e->waiters_head and calls
     * urbi_sched_strand_block, which mutates the scheduler's wait queues and
     * decrements strand_runnable_count.  An ISR has no cur_strand (no
     * scripting context) so the read at the function body would NULL-deref;
     * even if guarded, mutating scheduler state from an ISR would race the
     * main-loop dispatcher.  Hosts that need event-driven wakes from ISR
     * use the uevent_ring path instead. */
    URBI_ASSERT_NOT_ISR(vm);

    /* Scratch / eval context guard (spec §7.1 safety note). */
    if (vm->watchers->in_scratch || vm->watchers->in_eval) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                "waituntil from scratch context — undefined; returning NIL");
        return payload;
    }

    s = vm->cur_strand;

    /* Initialise wait fields. */
    s->next_event_waiter  = NULL;
    s->wait_event_target  = e;
    s->last_event_payload  = payload;   /* NIL via the file-level convention */

    /* Tail-append to waiters_head. */
    if (!e->waiters_head) {
        e->waiters_head = s;
    } else {
        struct UStrand *t = e->waiters_head;
        while (t->next_event_waiter) t = t->next_event_waiter;
        t->next_event_waiter = s;
    }

    /* SCHED-004: defence-in-depth — if a strand somehow has stale sleep-queue
     * links at re-stamp time (would happen only if a buggy caller bypassed
     * the dispatch loop's unblock contract), splice it out before changing
     * the state byte so wait_next does not point into the sleep queue with
     * the wrong reason.  Idempotent for the normal path (RUNNING strand). */
    urbi_sched_strand_unbind_from_sleep_queue(s);

    /* Transition to WAIT_EVENT via urbi_sched_strand_block (refactor-3 SCHED-01:
     * block owns the runnable-count decrement under the single-writer
     * scheme; the pre-refactor manual `state = USTRAND_WAIT_EVENT` +
     * guarded decrement pair is gone).  block's REASON_EVENT arm also
     * records the event in wait_payload.event — the documented active
     * union arm for this reason (ustrand.h) — alongside the
     * wait_event_target back-pointer wired above. */
    urbi_sched_strand_block(s, USTRAND_REASON_EVENT, (uint64_t)(uintptr_t)e);

    /* EMITR-002: this return value is *always* NIL.  c_event_waituntil parks
     * the strand and returns to the caller (the T53 opcode dispatcher); it
     * does not block.  s->last_event_payload was just initialised to NIL
     * above (just before the tail-append) and no emit can have run between
     * then and here under the cooperative scheduler, so the value read
     * back is NIL by construction.  The meaningful payload is deposited
     * by c_event_emit_* into s->last_event_payload on wake and is consumed
     * by the T53 opcode handler after the strand resumes at the next
     * dispatch slice — not via this return value.  The read-then-clear
     * here is defence in depth against a future scheduler that might let
     * an emit fire before the park completes. */
    payload = s->last_event_payload;
    s->last_event_payload = (UValue){0};   /* EMITR-007: file convention */
    return payload;
}
