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

/* State transitions */
void sched_strand_make_runnable(UStrand *s);
void sched_strand_block(UStrand *s, uint8_t reason, uint64_t payload);
void sched_strand_yield(UStrand *s);
void sched_strand_unblock(UStrand *s);

/* Timer / quiescence queries */
uint64_t sched_earliest_wake_us(UVM *vm);
bool     sched_quiescent(const UVM *vm);

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

/* Dequeue the ready-queue head and decrement strand_runnable_count.
 * Must only be called when ready_head is non-NULL.  Sets the strand's
 * ready_next/ready_prev to NULL.  Caller is responsible for setting the
 * strand's state to USTRAND_STATE_RUNNING before dispatching.
 * T16 urbi_step driver calls this before each dispatch_loop_until_yield. */
void sched_dequeue_ready_head(UVM *vm);

/* CHSTR-031: decrement host_call_pending_count if s had a cross-strand stop
 * deposited.  Called by ustrand_destroy so the bookkeeping lives in the
 * scheduler rather than the strand teardown code. */
void sched_strand_account_destroy(UVM *vm, UStrand *s);

#ifdef __cplusplus
}
#endif

#endif /* USCHED_COOPERATIVE_H */
