/* SPDX-License-Identifier: BSD-3-Clause */
/* Cooperative scheduler interface — row 9 §2.2 contract.
   All 12 ops declared here; sched_pick_next and sched_consume_budget are
   static inline (zero overhead, no symbol generated).  The remaining 10
   are non-inline and implemented in usched_cooperative.c. */

#ifndef USCHED_COOPERATIVE_H
#define USCHED_COOPERATIVE_H

#include <stdbool.h>
#include <stdint.h>
#include "sched/ustrand.h"
#include "vm/uvm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Feature flags — cooperative carries none */
#define URBI_SCHED_HAS_PRIORITY  0
#define URBI_SCHED_HAS_DEADLINE  0
#define URBI_SCHED_HAS_AGING     0

/* === Interface ops (row 9 §2.2 contract, ~12 ops) === */

/* Scheduler lifecycle.
 *
 * sched_init: zero the per-VM scheduler queues (ready_head/tail, sleep_q_head)
 *   and reset strand_runnable_count.  Called once during urbi_vm_create
 *   before any strand is enqueued.  `config` is reserved for future
 *   scheduler-class configuration (priority/deadline) and is currently ignored.
 *
 * sched_destroy: clear the per-VM scheduler queue heads.  Strands are owned
 *   by their realms (URealm.strands_head) and freed via urbi_realm_destroy,
 *   so this op does no per-strand teardown.  Called during urbi_vm_destroy
 *   after all realms have been torn down. */
void sched_init(UVM *vm, void *config);
void sched_destroy(UVM *vm);

/* Per-strand lifecycle.
 *
 * sched_strand_init: zero the scheduler intrusive list links
 *   (ready_next/prev, wait_next) and seed instruction_budget_remaining to
 *   URBI_STRAND_BUDGET_MAX.  Called by ustrand_init for every strand on
 *   create.  The strand is left in DORMANT state (sched_strand_init does
 *   not touch the state byte).  `attrs` is reserved for the v1.x scheduler-
 *   class abstraction (priority/deadline schedulers); currently unused.
 *
 *   CHSTR-039 (T106): the caller MUST have set s->state before calling
 *   (DORMANT for newly-created strands, RUNNING for the urbi_vm_run
 *   transient path).  -DURBI_DEBUG asserts the precondition; production
 *   builds elide the check.  The function is otherwise unchecked and
 *   would silently de-stabilise queue accounting if called on a READY,
 *   WAITING, or DEAD strand (re-arming the budget while the strand sits
 *   on a queue).
 *
 * sched_strand_destroy: detach the strand from the ready/sleep queue lists
 *   (idempotent — safe to call on an already-detached strand).  Does not
 *   free the strand itself; that is the caller's responsibility. */
void sched_strand_init(UStrand *s, void *attrs);
void sched_strand_destroy(UStrand *s);

/* Pick the head of the ready queue; returns NULL if queue is empty. */
static inline UStrand *
sched_pick_next(const UVM *vm) {
    return vm->ready_head;
}

/* === Single-writer runnable-count ownership (refactor-3 SCHED-01/B10) ===
 *
 * Invariant: vm->strand_runnable_count == |ready queue| + (1 if a
 * non-transient strand is RUNNING else 0).  WAITING and SUSPENDED strands
 * are NOT counted; transient strands (is_transient_strand) never
 * participate.  These two helpers are the ONLY writers (init/destroy
 * zeroing aside).  Exposed for the scheduler-adjacent transition sites
 * that change a strand's RUNNING membership outside this TU
 * (urbi_strand_suspend's RUNNING arm in ustrand.c); everything else must
 * route through the sched_strand_* transition functions. */
void sched_runnable_inc(UVM *vm, const UStrand *s);
void sched_runnable_dec(UVM *vm, const UStrand *s);

/* State transitions */
void sched_strand_make_runnable(UStrand *s);
void sched_strand_block(UStrand *s, uint8_t reason, uint64_t payload);
void sched_strand_yield(UStrand *s);
void sched_strand_unblock(UStrand *s);

/* Timer / quiescence queries */
uint64_t sched_earliest_wake_us(UVM *vm);
bool     sched_quiescent(const UVM *vm);

/* Wake every sleep-queue strand whose wake_us <= now (vm->host_time_us).
 * Each woken strand is unblocked (removed from sleep_q, made runnable).
 * No-op if there is no sleep queue or no host clock installed.  Shared by
 * sched_post_dispatch (step 3) and urbi_step's pre-dispatch pump so a lone
 * expired sleeper is woken even when no other strand drives the dispatch loop
 * (design-risks v0.11.4-A). */
void sched_wake_due_sleepers(UVM *vm);

/* Consume n opcodes from a strand's instruction budget, flooring at 0. */
static inline void
sched_consume_budget(UStrand *s, uint16_t n) {
    if (s->instruction_budget_remaining > n)
        s->instruction_budget_remaining -= n;
    else
        s->instruction_budget_remaining = 0;
}

/* GC root walker — registered with the GC root-provider registry at
 * urbi_vm_create.  Iterates the realm hierarchy (vm->realms_head →
 * realm.strands_head) so every live strand's register window, unwind state
 * and event-wait payload is visited; DEAD strands are filtered inside
 * strand_walk_roots.  See pre-M4 GC strand-walker spec §4.2/§6.1 for the
 * realm-hierarchy invariant the scheduler maintains. */
void sched_walk_roots(UVM *vm, UGcRootCallback cb, void *ctx);

/* Dequeue the ready-queue head.  Count-NEUTRAL (refactor-3 SCHED-01): the
 * dequeued strand is about to become the RUNNING strand, and the runnable
 * count covers |READY| + |RUNNING|.  Sets the strand's ready_next/ready_prev
 * to NULL.  Caller is responsible for setting the strand's state to
 * USTRAND_STATE_RUNNING before dispatching.  T16 urbi_step driver calls
 * this before each dispatch_loop_until_yield. */
void sched_dequeue_ready_head(UVM *vm);

/* CHSTR-031: decrement host_call_pending_count if s had a cross-strand stop
 * deposited.  Called by ustrand_destroy so the bookkeeping lives in the
 * scheduler rather than the strand teardown code. */
void sched_strand_account_destroy(UVM *vm, UStrand *s);

/* SCHED-004: detach a strand from the sleep queue if it is on it.
 *
 * Idempotent: no-op when the strand is not on the queue (wait_next == NULL
 * is the typical guard, but the helper walks the queue anyway so callers
 * can pass any strand without first checking state).  Clears s->wait_next
 * and decrements vm->wakeup_pending_count exactly once if the strand was
 * actually present.
 *
 * Used by re-stamp paths (e.g. c_event_waituntil) that change a strand's
 * state byte from one WAITING reason to another.  Without this helper,
 * a SLEEP-blocked strand re-stamped to WAIT_EVENT would leave wait_next
 * pointing into the sleep queue and wakeup_pending_count stale. */
void sched_strand_unbind_from_sleep_queue(UStrand *s);

/* REALM-011 / T69: splice a strand out of the cooperative ready queue if
 * it is on it.  Idempotent (the strand's own ready_next/ready_prev guard
 * the work).  Decrements vm->strand_runnable_count exactly once if the
 * strand was actually present (via sched_runnable_dec, so transient
 * strands are skipped).  Safe to call whether the strand is on the
 * queue (state == READY) or not (DORMANT/RUNNING/WAITING/DEAD).
 *
 * Called from urbi_realm_destroy before each strand free so that the
 * vm->ready_head / ready_tail doubly-linked list never holds dangling
 * pointers into freed strand memory. */
void sched_strand_unbind_from_ready_queue(UStrand *s);

#ifdef __cplusplus
}
#endif

#endif /* USCHED_COOPERATIVE_H */
