/* SPDX-License-Identifier: BSD-3-Clause */
/* Cooperative scheduler implementation — row 9 §2 contract.
   Freestanding-safe: only <stdbool.h> and <stdint.h> (both C99 freestanding). */

/* === 5-counter liveness ownership (row 8 §3 Rule X) ===
 *
 * VM quiescence (sched_quiescent) AND's all five counters; M3 maintains 4
 * (strand_suspended_count is excluded — always 0 at M3). Each subsystem
 * owns one counter and maintains it at the relevant push/pop sites:
 *
 *   strand_runnable_count   — owned by sched_strand_make_runnable (++) /
 *                             sched_strand_block (--) /
 *                             T16 urbi_step driver (-- when dequeuing READY
 *                             strands; -- when dispatch returns DEAD).
 *                             Invariant: number of strands in READY or
 *                             RUNNING state (i.e. consuming or eligible to
 *                             consume CPU on this VM). The urbi_vm_run transient
 *                             strand is intentionally excluded: it bypasses
 *                             sched_strand_make_runnable and balances its own
 *                             READY-cycle increments at dequeue.
 *
 *   wakeup_pending_count    — owned by sleep_q_insert (++) /
 *                             sleep_q_remove (--).
 *                             Invariant: number of strands on the sleep queue.
 *                             Decremented only when removal actually removes
 *                             the strand (not when the strand was not found).
 *
 *   host_call_pending_count — owned by urbi_tag_stop cross-strand path (T31).
 *                             Invariant: number of pending host-injected
 *                             unwind events. M3 stub: always 0 until T31.
 *
 *   watcher_active_count    — owned by M5 (active watcher list).
 *                             Invariant: number of live watchers; M3 stub: 0.
 *
 *   event_queue_count       — owned by M5+ (event scheduler).
 *                             Invariant: number of pending events; M3 stub: 0.
 */

#include "usched_cooperative.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "realm/urealm.h"  /* URealm; realms_head → strands_head walk (T32) */
#include <stdbool.h>
#include <stdint.h>
#include "gc/ugc.h"
#include "runtime/uframe.h"
#include <stddef.h>

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
        vm->wakeup_pending_count++;
        return;
    }
    UStrand *cur = vm->sleep_q_head;
    while (cur->wait_next &&
           cur->wait_next->wait_payload.wake_us <= s->wait_payload.wake_us)
        cur = cur->wait_next;
    s->wait_next   = cur->wait_next;
    cur->wait_next = s;
    vm->wakeup_pending_count++;
}

static void
sleep_q_remove(UVM *vm, UStrand *s)
{
    if (vm->sleep_q_head == s) {
        vm->sleep_q_head = s->wait_next;
        s->wait_next     = NULL;
        if (vm->wakeup_pending_count > 0) vm->wakeup_pending_count--;
        return;
    }
    UStrand *cur = vm->sleep_q_head;
    while (cur && cur->wait_next != s)
        cur = cur->wait_next;
    if (cur) {
        cur->wait_next = s->wait_next;
        s->wait_next   = NULL;
        if (vm->wakeup_pending_count > 0) vm->wakeup_pending_count--;
    }
    /* If cur is NULL, s was not on the queue; do not decrement (no underflow). */
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

/* sched_strand_init: zero the scheduler-tracked queue links and seed the
 * instruction budget.  `attrs` is currently unused — held in the signature
 * as a forward-compatibility hold for the v1.x scheduler-class abstraction
 * (priority/deadline schedulers will pass per-strand attribute structs
 * through this slot; see USchedClass at include/urbi/sched.h).  The
 * cooperative scheduler ignores it and always assigns URBI_STRAND_BUDGET_MAX. */
void
sched_strand_init(UStrand *s, void *attrs)
{
    (void)attrs;  /* RESERVED v1.x — see header docstring */
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
sched_quiescent(const UVM *vm)
{
    /* Per row 8 §3 Rule X: 5 counters AND'd zero.
       strand_suspended_count is excluded (always 0 at M3). */
    return vm->strand_runnable_count  == 0
        && vm->watcher_active_count   == 0
        && vm->event_queue_count      == 0
        && vm->wakeup_pending_count   == 0
        && vm->host_call_pending_count == 0;
}

/* === CHSTR-031: sched_strand_account_destroy ===
 *
 * Decrement host_call_pending_count if s had a cross-strand stop deposited.
 * Called by ustrand_destroy so that scheduler-level bookkeeping for cross-
 * strand stop liveness stays in the scheduler, not in strand teardown code. */

void
sched_strand_account_destroy(UVM *vm, UStrand *s)
{
    /* T31: if urbi_tag_stop deposited a cross-strand stop on this strand,
       decrement the host_call_pending_count so sched_quiescent converges
       once all tagged strands have been destroyed. */
    if (s->cross_strand_stop_pending != 0) {
        if (vm->host_call_pending_count > 0)
            vm->host_call_pending_count--;
        s->cross_strand_stop_pending = 0U;
    }
}

/* === T16 step-driver helper === */

void
sched_dequeue_ready_head(UVM *vm)
{
    UStrand *s = vm->ready_head;
    if (!s) return;  /* underflow guard */
    vm->ready_head = s->ready_next;
    if (vm->ready_head != NULL)
        vm->ready_head->ready_prev = NULL;
    else
        vm->ready_tail = NULL;
    s->ready_next = NULL;
    s->ready_prev = NULL;
    if (vm->strand_runnable_count > 0)
        vm->strand_runnable_count--;
}

/* === strand_walk_roots (internal helper) ===
 *
 * Walk all GC roots for a single live strand.  Called by sched_walk_roots
 * for every non-DEAD strand in the ready and sleep queues.
 *
 * Root sources at M3 baseline:
 *   (1) Register window — conservative full-stack scan (see below).
 *   (2) Unwind state — unwind_value + fatal_value are UValue fields.
 *   (3) Cleanup stack — owning_tag (UTag*) and catch_pattern (UPattern*)
 *       are not yet UValues at M3 (they land at T29+); skipped with TODO.
 *   (4) Wait payload — event / join_parent involve M5 types; skipped with TODO.
 *
 * Register window strategy (row 10 §5.2 guidance):
 *   s->stack is a heap-allocated UVM_STACK_CAP-slot array.  The active
 *   register window spans frames[0..frame_count-1]; the topmost frame's
 *   extent requires bytecode metadata not available at M3.  We walk the
 *   entire allocated array (conservative over-mark; never under-marks).
 *   TODO(T26+ opt): tighten to active-frame register window when bytecode
 *   emits frame-extent metadata (proposed for M4/M5). */
static void
strand_walk_roots(UVM *vm, UStrand *s, UGcRootCallback cb, void *ctx)
{
    int i;

    /* Guard: DEAD strands have no live roots (spec §5.2). */
    if (USTRAND_GET_STATE(s) == USTRAND_DEAD) return;

    /* (1) Register window — conservative full-stack scan.
     *     s->stack may be NULL for a DORMANT strand not yet armed. */
    if (s->stack != NULL) {
        for (i = 0; i < UVM_STACK_CAP; i++) {
            cb(vm, &s->stack[i], ctx);
        }
    }

    /* (2) Unwind state (row 7 §4.4).
     *     unwind_value and fatal_value are UValue fields on the strand. */
    cb(vm, &s->unwind_value, ctx);
    cb(vm, &s->fatal_value,  ctx);

    /* (3) Cleanup-stack entries (row 7 §4.4).
     *     owning_tag (UTag*) and catch_pattern (UPattern*) are not UValues
     *     at M3 baseline — T29 will enroll UTags as GC roots.
     *     TODO(T29): walk cleanup_base[0..cleanup_depth-1].owning_tag +
     *     catch_pattern once those become GC-managed UValues. */

    /* (4) Wait payload (row 9 §4.3).
     *     UEvent and UStrand join_parent are not GC-managed UValues at M3.
     *     TODO(M5): walk s->wait_payload.event when UEvent becomes a GC cell. */

    /* (5) last_event_payload (spec #3 §7.1, T56).
     *     Written by c_event_emit_* before unblocking a waituntil strand.
     *     May hold a heap-bearing UValue (e.g. UVAL_OBJECT, UVAL_CLOSURE,
     *     UVAL_EVENT) between the emit and the strand's next dispatch turn.
     *     Route through cb so the mark callback applies the heap-bearing check. */
    cb(vm, &s->last_event_payload, ctx);
}

/* === GC root walker for the scheduler ===
 *
 * Called by the GC as a registered root provider (row 10 §5.2).
 * Iterates the realm hierarchy (vm->realms_head → realm.strands_head)
 * rather than scheduler-private ready/sleep queues, so that strands
 * blocked in any wait state (WAITING_JOIN, future WAITING_EVENT, …)
 * are visited.  DEAD-strand filter stays inside strand_walk_roots.
 *
 * Per pre-M4 GC strand-walker spec §4.2, §6.1:
 *   Every UStrand whose register window may contain GC-managed UValues
 *   MUST be reachable from vm->realms_head → realm.strands_head.
 *   Scheduler implementations are responsible for maintaining this
 *   invariant; the walker assumes it without re-verification. */
void
sched_walk_roots(UVM *vm, UGcRootCallback cb, void *ctx)
{
    URealm  *r;
    UStrand *s;

    /* Realm-hierarchy walk per pre-M4 GC strand-walker spec §4.2. Visits every
     * live strand regardless of state (READY/RUNNING/WAITING_*); DEAD-strand
     * filter stays in strand_walk_roots. */
    for (r = vm->realms_head; r != NULL; r = r->next_in_vm) {
        for (s = r->strands_head; s != NULL; s = s->next_in_realm) {
            strand_walk_roots(vm, s, cb, ctx);
        }
    }
}
