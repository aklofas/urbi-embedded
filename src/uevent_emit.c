/* SPDX-License-Identifier: BSD-3-Clause */
/* Event emit primitives — async fan-out (spec #3 §5.2). */

#include "uevent_emit.h"
#include "uevent.h"
#include "ustrand.h"
#include "uvm.h"
#include "watcher/uwatcher.h"  /* do_spawn_body_coroutine, UWATCHER_AT_EVENT* */
#include "sched/usched_cooperative.h"  /* sched_strand_make_runnable */
#include "urbi/urbi.h"         /* URBI_ASSERT_NOT_ISR */

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
