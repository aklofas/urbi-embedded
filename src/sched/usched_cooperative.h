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

/* Scheduler lifecycle */
void sched_init(UVM *vm, void *config);
void sched_destroy(UVM *vm);

/* Per-strand lifecycle */
void sched_strand_init(UStrand *s, void *attrs);
void sched_strand_destroy(UStrand *s);

/* Pick the head of the ready queue; returns NULL if queue is empty. */
static inline UStrand *
sched_pick_next(UVM *vm) {
    return vm->ready_head;
}

/* State transitions */
void sched_strand_make_runnable(UStrand *s);
void sched_strand_block(UStrand *s, uint8_t reason, uint64_t payload);
void sched_strand_yield(UStrand *s);
void sched_strand_unblock(UStrand *s);

/* Timer / quiescence queries */
uint64_t sched_earliest_wake_us(UVM *vm);
bool     sched_quiescent(UVM *vm);

/* Consume n opcodes from a strand's instruction budget, flooring at 0. */
static inline void
sched_consume_budget(UStrand *s, uint16_t n) {
    if (s->instruction_budget_remaining > n)
        s->instruction_budget_remaining -= n;
    else
        s->instruction_budget_remaining = 0;
}

/* GC root walker — stub at T5; T26 wires into root provider registry. */
void sched_walk_roots(UVM *vm, UGcRootCallback cb, void *ctx);

/* Dequeue the ready-queue head and decrement strand_runnable_count.
 * Must only be called when ready_head is non-NULL.  Sets the strand's
 * ready_next/ready_prev to NULL.  Caller is responsible for setting the
 * strand's state to USTRAND_STATE_RUNNING before dispatching.
 * T16 urbi_step driver calls this before each dispatch_loop_until_yield. */
void sched_dequeue_ready_head(UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* USCHED_COOPERATIVE_H */
