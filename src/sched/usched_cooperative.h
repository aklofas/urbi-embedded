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
 * that change a strand's counted-set membership outside this TU:
 *   - src/sched/ustrand.c — urbi_strand_suspend's RUNNING arm (dec:
 *     RUNNING -> SUSPENDED leaves the counted set);
 *   - src/vm/ustep.c — urbi_step's fatal-exit arm (dec: a fatal-DEAD
 *     strand never reaches sched_post_dispatch's step-1 decrement);
 *   - src/runtime/uunwind.c — run_cleanup_with_replace's blocked/yielded
 *     cleanup-body rebalance (inc: after urbi_sched_strand_unpark(s, 0) the
 *     strand re-enters the counted set as RUNNING before the fatal stamp
 *     — correct by construction under the single-writer scheme; closes
 *     design-risks v0.13.1-C).
 * Everything else must route through the sched_strand_* transition
 * functions. */
void urbi_sched_runnable_inc(UVM *vm, const UStrand *s);
void urbi_sched_runnable_dec(UVM *vm, const UStrand *s);

/* === Parked-strand counters (refactor-3 SCHED-13 / VM-12) ===
 *
 * vm->strand_waiting_count   == |WAITING non-transient strands|
 * vm->strand_suspended_count == |SUSPENDED non-transient strands|
 *
 * Same single-writer scheme and no-saturation discipline as the runnable
 * pair above (dec asserts > 0; transient strands are skipped).  Writers —
 * see the field declarations in vm/uvm.h for the full transition map:
 *   waiting:   inc in sched_strand_block; dec in sched_strand_make_runnable
 *              (WAITING entry state — urbi_sched_strand_unpark(s, 1) funnels
 *              there; a SCHED-08 gated wake also exits here, handing off
 *              into the suspended count), urbi_sched_strand_unpark(s, 0)
 *              (off-funnel exits: the cleanup-executor in
 *              src/runtime/uunwind.c and urbi_strand_panic), and
 *              ustrand_destroy.
 *   suspended: inc in urbi_strand_suspend (src/sched/ustrand.c,
 *              READY/RUNNING arms) and in sched_strand_make_runnable's
 *              gated-wake arm (SCHED-08); dec in sched_strand_make_runnable
 *              (SUSPENDED entry state), urbi_strand_panic's SUSPENDED arm,
 *              and ustrand_destroy.
 * Both feed urbi_vm_liveness()'s `armed` term — reported to the host but
 * excluded from QUIESCENT (owner decision 2026-06-11). */
void urbi_sched_waiting_inc(UVM *vm, const UStrand *s);
void urbi_sched_waiting_dec(UVM *vm, const UStrand *s);
void urbi_sched_suspended_inc(UVM *vm, const UStrand *s);
void urbi_sched_suspended_dec(UVM *vm, const UStrand *s);

/* State transitions */
void sched_strand_make_runnable(UStrand *s);
void sched_strand_block(UStrand *s, uint8_t reason, uint64_t payload);
void sched_strand_yield(UStrand *s);
void sched_strand_unblock(UStrand *s);

/* urbi_sched_strand_unpark — reason-dispatched third-party-link removal for a
 * WAITING strand (refactor-3 SCHED-05): the wake-side mirror of the
 * death-side scrub in strand_cleanup_observers.  Unlinks the strand from
 * its reason-specific external structure (SLEEP: sleep queue; EVENT:
 * waiter chain; JOIN: child->joiners_head; WATCHER: waituntil
 * waiter_strand scrub + watcher retire) BEFORE it leaves WAITING, so no
 * later waker can touch a strand that already moved on (or was freed).
 *
 * enqueue == 1: route through sched_strand_make_runnable (tag-stop /
 *   cancel wake; the funnel owns the strand_waiting_count exit).
 * enqueue == 0: leave the strand unqueued with its state byte untouched —
 *   the caller stamps the next state (cleanup-executor fail-soft stamps
 *   RUNNING; urbi_strand_panic stamps DEAD); the waiting-count exit
 *   happens inside this call (off-funnel writer; see the map above).
 *
 * Precondition: USTRAND_IS_WAITING(s).  Implemented in ustrand.c next to
 * strand_cleanup_observers so both sides share strand_unlink_park. */
void urbi_sched_strand_unpark(UStrand *s, int enqueue);

/* Timer / quiescence queries */
uint64_t sched_earliest_wake_us(const UVM *vm);
bool     sched_quiescent(const UVM *vm);

/* === urbi_vm_liveness — the ONE quiescence/liveness formula (refactor-3 SCHED-13) ===
 *
 * Callers: sched_quiescent, urbi_step's post-loop verdict ladder, and
 * urbi_vm_has_live_work.  Pre-fix those three each computed a different
 * formula (the audit's "three divergent quiescence definitions").
 *
 * Field semantics (owner decision 2026-06-11, option a):
 *   runnable     — strands the dispatcher can run right now (READY/RUNNING).
 *   pending      — internal work the next step will perform without any
 *                  external input: ISR-ring events, host-injected cross-
 *                  strand stops, dirty watcher evals, queued onleave drains.
 *                  The magnitude is NOT meaningful (it mixes 0/1 presence
 *                  flags with real counts) — compare against 0 only.
 *   armed        — external-input work: armed watchers (all modes) +
 *                  SUSPENDED (blocked/frozen) + WAITING (event/join/watcher-
 *                  parked) strands.  Reported, but does NOT block QUIESCENT —
 *                  host slot writes, injected events, or tag unblock/unfreeze
 *                  re-arm the VM.
 *   timed        — 1 iff a sleeper or live periodic has a future deadline;
 *                  next_wake_us then holds the earliest deadline (else
 *                  UINT64_MAX).
 *
 * QUIESCENT == (runnable + pending + timed == 0), armed notwithstanding. */
typedef struct UVmLiveness {
    uint32_t runnable;
    uint32_t pending;
    uint32_t armed;
    uint32_t timed;
    uint64_t next_wake_us;
} UVmLiveness;

void urbi_vm_liveness(const UVM *vm, UVmLiveness *out);

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
 * strand was actually present (via urbi_sched_runnable_dec, so transient
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
