/* SPDX-License-Identifier: BSD-3-Clause */
/* Event emit primitives — async + sync + waituntil (spec #3 §5.2-§5.4, §7.1). */

#include "uevent_emit.h"
#include "uevent.h"
#include "ustrand.h"
#include "uvm.h"
#include "watcher/uwatcher.h"  /* do_spawn_body_coroutine, UWATCHER_AT_EVENT* */
#include "sched/usched_cooperative.h"  /* sched_strand_make_runnable, sched_strand_block */
#include "urbi/urbi.h"         /* URBI_ASSERT_NOT_ISR, URBI_LOG_WARN */

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
    struct UWatcher *w;
    struct UStrand  *s;
    struct UStrand  *ns;

    URBI_ASSERT_NOT_ISR(vm);

    /* Walk at_watchers_head FIFO: snapshot next before potential modification. */
    w = e->at_watchers_head;
    while (w) {
        struct UWatcher *next = w->next_in_event;   /* snapshot before any modification */
        if (w->mode == UWATCHER_AT_EVENT || w->mode == UWATCHER_AT_EVENT_SYNC) {
            /* M5 baseline: fire_context NULL; T53 wires payload into body R[0]. */
            do_spawn_body_coroutine(vm, w, NULL);
        }
        w = next;
    }

    /* Walk waiters_head FIFO: deposit payload + wake each waiter. */
    s = e->waiters_head;
    while (s) {
        ns = s->next_event_waiter;
        s->last_event_payload = payload;
        s->wait_event_target  = NULL;
        s->next_event_waiter  = NULL;
        /* sched_strand_make_runnable sets state = USTRAND_STATE_READY and
         * increments strand_runnable_count.  The strand was in USTRAND_WAIT_EVENT
         * (WAITING) — not on the run-queue — so no double-enqueue risk. */
        sched_strand_make_runnable(s);
        s = ns;
    }
    e->waiters_head = NULL;
}

/* === run_event_body_on_scratch (file-static, spec #3 §5.3) ===
 *
 * Run w->body synchronously on the watcher scratch frame with R[0] = payload.
 * Sets in_watcher_scratch for the duration; logs + suppresses throws.
 *
 * At M5 baseline the real scratch runner (urbi_run_closure_on_scratch) is not
 * yet wired — the R5 pass installs it.  The function correctly sets + clears
 * in_watcher_scratch, so the degrade-to-async guard in c_event_emit_sync works
 * even before R5 lands.  Body execution is a no-op until R5. */
static void
run_event_body_on_scratch(struct UVM *vm, struct UWatcher *w, UValue payload)
{
    (void)w;
    (void)payload;

    /* Guard: never re-enter scratch execution from within scratch. */
    if (vm->in_watcher_scratch) return;

    vm->in_watcher_scratch = 1;

    /* R5 will replace this no-op body with real bytecode dispatch:
     *   1. Setup vm->watcher_scratch_frame with w->body + R[0]=payload.
     *   2. Run until return/throw.
     *   3. On UEXEC_THROW: log URBI_LOG_WARN "at sync(e?) body threw; suppressed".
     *   4. Clear frame state. */

    vm->in_watcher_scratch = 0;
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
    struct UWatcher *w;
    struct UStrand  *s;
    struct UStrand  *ns;

    URBI_ASSERT_NOT_ISR(vm);

    if (vm->in_watcher_scratch || vm->in_watcher_eval || vm->in_watcher_install) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, URBI_LOG_WARN,
                "sync emit on event degraded to async (nested in scratch context)");
        c_event_emit_async(vm, e, payload);
        return;
    }

    /* Walk at_watchers_head: sync subs run inline; async subs spawn. */
    w = e->at_watchers_head;
    while (w) {
        struct UWatcher *next = w->next_in_event;
        if (w->mode == UWATCHER_AT_EVENT_SYNC) {
            run_event_body_on_scratch(vm, w, payload);
        } else if (w->mode == UWATCHER_AT_EVENT) {
            do_spawn_body_coroutine(vm, w, NULL);
        }
        w = next;
    }

    /* Wake waiters — identical to async path. */
    s = e->waiters_head;
    while (s) {
        ns = s->next_event_waiter;
        s->last_event_payload = payload;
        s->wait_event_target  = NULL;
        s->next_event_waiter  = NULL;
        sched_strand_make_runnable(s);
        s = ns;
    }
    e->waiters_head = NULL;
}

/* === c_event_waituntil (spec #3 §7.1) ===
 *
 * Tail-appends the calling strand to e->waiters_head, transitions to
 * USTRAND_WAIT_EVENT (0x33), and decrements strand_runnable_count.
 *
 * Callers that dispatch via the bytecode loop (T53 opcode binding) MUST
 * goto exit_strand after this call returns — the strand is now WAITING
 * and must not consume further opcodes until woken by c_event_emit_*.
 *
 * On wake: c_event_emit_* deposits last_event_payload before calling
 * sched_strand_make_runnable.  The T53 opcode binding reads the payload
 * from s->last_event_payload after the strand resumes execution.
 *
 * Scratch-context guard: calling from within a watcher scratch or eval
 * frame is undefined (can deadlock or corrupt scratch state).  Returns
 * NIL + URBI_LOG_WARN if in_watcher_scratch or in_watcher_eval is set. */
UValue
c_event_waituntil(struct UVM *vm, struct UEvent *e)
{
    struct UStrand *s;
    UValue payload;

    URBI_ASSERT_NOT_ISR(vm);

    /* Scratch / eval context guard (spec §7.1 safety note). */
    if (vm->in_watcher_scratch || vm->in_watcher_eval) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, URBI_LOG_WARN,
                "waituntil from scratch context — undefined; returning NIL");
        payload.kind = UVAL_NIL;
        payload.v.i  = 0;
        return payload;
    }

    s = vm->cur_strand;

    /* Initialise wait fields. */
    s->next_event_waiter  = NULL;
    s->wait_event_target  = e;
    s->last_event_payload.kind = UVAL_NIL;
    s->last_event_payload.v.i  = 0;

    /* Tail-append to waiters_head. */
    if (!e->waiters_head) {
        e->waiters_head = s;
    } else {
        struct UStrand *t = e->waiters_head;
        while (t->next_event_waiter) t = t->next_event_waiter;
        t->next_event_waiter = s;
    }

    /* Transition to WAIT_EVENT and decrement runnable count.
     * USTRAND_WAIT_EVENT (0x33) uses sub-code 0x03 — distinct from
     * USTRAND_REASON_EVENT (0x02) used by sched_strand_block.  Set directly
     * to preserve the intended encoding.  Decrement count only when the
     * strand is RUNNING (normal dispatch path) to avoid underflow.
     * The urbi_step loop re-increments when it sees USTRAND_IS_WAITING(s)
     * on return from dispatch_loop_until_yield, restoring balance. */
    if (s->state == USTRAND_STATE_RUNNING && vm->strand_runnable_count > 0)
        vm->strand_runnable_count--;
    s->state = USTRAND_WAIT_EVENT;

    /* Return last_event_payload (NIL at park time; meaningful value is
     * deposited by c_event_emit_* on wake and read by the T53 opcode
     * handler after the strand resumes at the next dispatch slice). */
    payload = s->last_event_payload;
    s->last_event_payload.kind = UVAL_NIL;
    s->last_event_payload.v.i  = 0;
    return payload;
}
