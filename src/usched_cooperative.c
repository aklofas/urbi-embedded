/* SPDX-License-Identifier: BSD-3-Clause */
/* Cooperative scheduler implementation — row 9 §2 contract.
   Freestanding-safe: only <stdbool.h> and <stdint.h> (both C99 freestanding). */

#include "usched_cooperative.h"
#include "uvm.h"
#include "ustrand.h"
#include <stdbool.h>
#include <stdint.h>

/* Maximum instruction budget assigned to a strand on sched_strand_init.
   Can be overridden at compile time (e.g. -DURBI_STRAND_BUDGET_MAX=500). */
#ifndef URBI_STRAND_BUDGET_MAX
#  define URBI_STRAND_BUDGET_MAX  1000
#endif

/* === Private helpers: sleep queue (sorted singly-linked list by wake_us) === */

static void
sleep_q_insert(UVM *vm, UStrand *s)
{
    /* Insert s into the sleep queue sorted ascending by wake_us. */
    if (!vm->sleep_q_head ||
        vm->sleep_q_head->wait_payload.wake_us > s->wait_payload.wake_us) {
        s->wait_next     = vm->sleep_q_head;
        vm->sleep_q_head = s;
        return;
    }
    UStrand *cur = vm->sleep_q_head;
    while (cur->wait_next &&
           cur->wait_next->wait_payload.wake_us <= s->wait_payload.wake_us)
        cur = cur->wait_next;
    s->wait_next   = cur->wait_next;
    cur->wait_next = s;
}

static void
sleep_q_remove(UVM *vm, UStrand *s)
{
    if (vm->sleep_q_head == s) {
        vm->sleep_q_head = s->wait_next;
        s->wait_next     = NULL;
        return;
    }
    UStrand *cur = vm->sleep_q_head;
    while (cur && cur->wait_next != s)
        cur = cur->wait_next;
    if (cur) {
        cur->wait_next = s->wait_next;
        s->wait_next   = NULL;
    }
}

/* === Scheduler lifecycle === */

void
sched_init(UVM *vm, void *config)
{
    (void)config;
    vm->ready_head             = NULL;
    vm->ready_tail             = NULL;
    vm->sleep_q_head           = NULL;
    vm->strand_runnable_count  = 0;
}

void
sched_destroy(UVM *vm)
{
    /* Strands are owned by their realms; nothing to free here. */
    vm->ready_head   = NULL;
    vm->ready_tail   = NULL;
    vm->sleep_q_head = NULL;
}

/* === Per-strand lifecycle === */

void
sched_strand_init(UStrand *s, void *attrs)
{
    (void)attrs;
    s->ready_next                   = NULL;
    s->ready_prev                   = NULL;
    s->wait_next                    = NULL;
    s->instruction_budget_remaining = (uint16_t)URBI_STRAND_BUDGET_MAX;
}

void
sched_strand_destroy(UStrand *s)
{
    /* Detach from any list (idempotent). */
    s->ready_next = NULL;
    s->ready_prev = NULL;
    s->wait_next  = NULL;
}

/* === State transitions === */

void
sched_strand_make_runnable(UStrand *s)
{
    /* Tail-insertion into the FIFO ready queue.
       Per row 12 §3: single entry point for DORMANT/WAITING → READY. */
    UVM *vm = s->vm;
    s->state      = USTRAND_STATE_READY;
    s->ready_next = NULL;
    s->ready_prev = vm->ready_tail;
    if (vm->ready_tail)
        vm->ready_tail->ready_next = s;
    else
        vm->ready_head = s;
    vm->ready_tail = s;
    vm->strand_runnable_count++;
}

void
sched_strand_yield(UStrand *s)
{
    /* RUNNING → READY tail: same path as make_runnable. */
    sched_strand_make_runnable(s);
}

void
sched_strand_block(UStrand *s, uint8_t reason, uint64_t payload)
{
    UVM *vm = s->vm;
    /* RUNNING strands are not on the ready queue; decrement the counter. */
    if (s->state == USTRAND_STATE_RUNNING) {
        if (vm->strand_runnable_count > 0)
            vm->strand_runnable_count--;
    }
    s->state = (uint8_t)(USTRAND_WAITING | (reason & USTRAND_REASON_MASK));
    switch (reason) {
        case USTRAND_REASON_SLEEP:
            s->wait_payload.wake_us = payload;
            sleep_q_insert(vm, s);
            break;
        case USTRAND_REASON_EVENT:
            s->wait_payload.event = (struct UEvent *)(uintptr_t)payload;
            break;
        case USTRAND_REASON_JOIN:
            s->wait_payload.join_parent = (UStrand *)(uintptr_t)payload;
            break;
        default:
            break;
    }
}

void
sched_strand_unblock(UStrand *s)
{
    UVM *vm = s->vm;
    /* Remove from the sleep queue if this was a sleep-wait. */
    if (USTRAND_GET_REASON(s) == USTRAND_REASON_SLEEP)
        sleep_q_remove(vm, s);
    sched_strand_make_runnable(s);
}

/* === Timer / quiescence queries === */

uint64_t
sched_earliest_wake_us(UVM *vm)
{
    if (!vm->sleep_q_head) return UINT64_MAX;
    return vm->sleep_q_head->wait_payload.wake_us;
}

bool
sched_quiescent(UVM *vm)
{
    /* Per row 8 §3 Rule X: 5 counters AND'd zero.
       strand_suspended_count is excluded (always 0 at M3). */
    return vm->strand_runnable_count  == 0
        && vm->watcher_active_count   == 0
        && vm->event_queue_count      == 0
        && vm->wakeup_pending_count   == 0
        && vm->host_call_pending_count == 0;
}

/* === GC root walker (stub — T26 fills) === */

void
sched_walk_roots(UVM *vm, UGcRootCallback cb, void *ctx)
{
    /* T26 wires this to walk register windows, cleanup-stack entries,
       and wait_payload pointers for all live strands.  Stub for now. */
    (void)vm;
    (void)cb;
    (void)ctx;
}
