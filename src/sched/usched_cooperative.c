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
#include "sched/usched_post_dispatch.h"  /* sched_post_dispatch declaration */
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "realm/urealm.h"  /* URealm; realms_head → strands_head walk (T32) */
#include "runtime/umacros.h"  /* URBI_INTERNAL_ASSERT (SCHED-002/003) */
#include "urbi/urbi.h"          /* urbi_strand_destroy (sched_post_dispatch step 2) */
#include "stdlib/temporal.h"   /* urbi_periodic_pump (sched_post_dispatch step 4) */
#include <stdbool.h>
#include <stdint.h>
#include "gc/ugc.h"
#include "gc/ugc_incremental.h"  /* gc_shade_gray (Step C-1 closure/upval roots) */
#include "runtime/uclosure.h"   /* UClosure + UUpvalCell full defs (Step C-1) */
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
    /* Insert s into the sleep queue sorted ascending by wake_us.
     * CHSTR-025: every node on the sleep queue is in REASON_SLEEP, so
     * wait_payload.wake_us is the active union arm at every read below. */
    URBI_INTERNAL_ASSERT(USTRAND_GET_REASON(s) == USTRAND_REASON_SLEEP);
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
    /* Strands are owned by their realms; nothing to free here.
     *
     * SCHED-009: zero strand_runnable_count for symmetry with sched_init.
     * Pre-fix this counter survived destroy, so a destroy + stale-query
     * path would observe a non-zero value despite the queues being NULL.
     * The four scheduler-owned fields (ready_head, ready_tail, sleep_q_head,
     * strand_runnable_count) are now mirror-zeroed across init/destroy. */
    vm->ready_head            = NULL;
    vm->ready_tail            = NULL;
    vm->sleep_q_head          = NULL;
    vm->strand_runnable_count = 0;
}

/* === Per-strand lifecycle === */

/* sched_strand_init: zero the scheduler-tracked queue links and seed the
 * instruction budget.  `attrs` is currently unused — held in the signature
 * as a forward-compatibility hold for the v1.x scheduler-class abstraction
 * (priority/deadline schedulers will pass per-strand attribute structs
 * through this slot; see USchedClass at include/urbi/sched.h).  The
 * cooperative scheduler ignores it and always assigns URBI_STRAND_BUDGET_MAX.
 *
 * CHSTR-039 (T106): MUST NOT touch s->state.  The state byte is owned by
 * the strand-lifecycle layer (urbi_strand_create sets DORMANT,
 * urbi_strand_arm_from_closure / urbi_vm_run set RUNNING, and the
 * sched_strand_make_runnable / _block transitions advance it from there).
 * sched_strand_init runs from two paths in M3+ baselines:
 *   1. ustrand_init (during urbi_strand_create) — state already DORMANT.
 *   2. urbi_vm_run's transient-strand setup — state already RUNNING.
 * If sched_strand_init wrote a state byte here it would clobber the
 * caller's setup and either re-DORMANT a transient (rejecting the next
 * dispatch) or worse, drop a RUNNING strand into the ready queue with
 * stale state.  The contract is: caller wires state, sched_strand_init
 * wires queue links + budget. */
void
sched_strand_init(UStrand *s, void *attrs)
{
    (void)attrs;  /* RESERVED v1.x — see header docstring */
    /* CHSTR-039: callers must have set state to a legal initial value
     * (DORMANT for newly-created strands, RUNNING for the urbi_vm_run
     * transient path) before calling.  Detect violation in -DURBI_DEBUG
     * builds; production builds elide the check. */
    URBI_INTERNAL_ASSERT(USTRAND_GET_STATE(s) == USTRAND_DORMANT
                      || USTRAND_GET_STATE(s) == USTRAND_RUNNING);
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
       Per row 12 §3: single entry point for DORMANT/WAITING → READY.

       CHSTR-042 (T107): reject DEAD strands as a production fail-safe.
       A DEAD strand has had its register stack freed and its cleanup
       chain unwound; re-enqueueing one would dispatch into freed memory
       (and double-count strand_runnable_count, blocking quiescence).  In
       -DURBI_DEBUG the assert below trips at the call site.  In production
       the early return prevents the corruption silently — the strand
       simply stays DEAD and the caller's ++count is skipped.  No legitimate
       caller drives a DEAD → READY transition; the path is purely defensive
       against future refactors that lose track of strand state.

       SCHED-005: idempotence assertion — calling make_runnable on a strand
       already in READY state would tail-insert it a second time, producing
       a circular ready_next/ready_prev chain and double-counting
       strand_runnable_count (so sched_quiescent never converges).  Every
       legitimate caller transitions DORMANT → READY (urbi_strand_start) or
       WAITING → READY (sched_strand_unblock, event/watcher waker paths,
       uunwind wake).  No legitimate caller drives READY → READY.  The
       assertion fail-fasts in URBI_DEBUG; production builds elide it (the
       circular-chain corruption surfaces as a quiescence-stall bug). */
    URBI_INTERNAL_ASSERT(USTRAND_GET_STATE(s) != USTRAND_DEAD);
    URBI_INTERNAL_ASSERT(s->state != USTRAND_STATE_READY);
    if (USTRAND_GET_STATE(s) == USTRAND_DEAD) return;
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
    /* SCHED-003: yielding from a non-RUNNING state silently re-enqueues,
     * double-counting strand_runnable_count and producing a circular
     * ready_next/ready_prev chain (sched_strand_make_runnable unconditionally
     * tail-inserts).  Fix: assert entry state == RUNNING.  Yields from READY
     * are a programming error (the strand is already on the queue); yields
     * from WAITING bypass the proper unblock path and break the symmetric
     * counter contract documented at the top of this file. */
    URBI_INTERNAL_ASSERT(s->state == USTRAND_STATE_RUNNING);
    /* RUNNING → READY tail: same path as make_runnable. */
    sched_strand_make_runnable(s);
}

void
sched_strand_block(UStrand *s, uint8_t reason, uint64_t payload)
{
    UVM *vm = s->vm;
    /* SCHED-002: re-blocking an already-WAITING strand corrupts the queue
     * accounting (sleep_q_insert would re-insert and double-count
     * wakeup_pending_count; event/join paths leak prior payloads).  Entry
     * state must be RUNNING (the normal "I yielded by blocking") or READY
     * (race-window safety: a strand that had been re-queued but hasn't yet
     * been dispatched can still be diverted into a wait without harm).
     * If a future caller legitimately needs to retarget a WAITING strand,
     * route through an explicit sched_strand_rebind helper that handles
     * queue-removal-then-re-insert correctly. */
    URBI_INTERNAL_ASSERT(s->state == USTRAND_STATE_RUNNING ||
                         s->state == USTRAND_STATE_READY);
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
            /* payload is a uint64_t carrying a UEvent* (REASON_EVENT calling
             * convention from c_event_subscribe / event-emit handoff). The
             * uintptr_t round-trip is intentional — payload encoding is a
             * documented sched/strand contract. */
            s->wait_payload.event = (struct UEvent *)(uintptr_t)payload;  /* NOLINT(performance-no-int-to-ptr) — REASON_EVENT payload-encoding contract */
            break;
        case USTRAND_REASON_JOIN:
            /* payload is a uint64_t carrying the parent UStrand* (REASON_JOIN
             * calling convention from join_parent setup). */
            s->wait_payload.join_parent = (UStrand *)(uintptr_t)payload;  /* NOLINT(performance-no-int-to-ptr) — REASON_JOIN payload-encoding contract */
            break;
        default:
            /* SCHED-007: unknown reason byte — payload is dropped by design.
             * Entry contracts (Phase 5 of v0.5.7-fixes; see USTRAND_REASON_*
             * constants in src/sched/ustrand.h) reject unknown reasons
             * upstream at every legitimate caller (the c_event_*, sleep,
             * join paths all pass a literal USTRAND_REASON_*).  This
             * default arm is a defense-in-depth catch-all and should never
             * fire in production.  USTRAND_REASON_HOST (0x04, reserved)
             * and USTRAND_REASON_NONE (0x00) currently land here without a
             * payload field assignment — when those become live they need
             * their own case arm.  URBI_INTERNAL_ASSERT(0) under debug to
             * surface anyone who slipped past the upstream check. */
            URBI_INTERNAL_ASSERT(0);
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
    /* CHSTR-025: sleep_q_head is by construction in REASON_SLEEP. */
    URBI_INTERNAL_ASSERT(
        USTRAND_GET_REASON(vm->sleep_q_head) == USTRAND_REASON_SLEEP);
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
       once all tagged strands have been destroyed.

       CHSTR-013 (T101): the inner `if (host_call_pending_count > 0)` guard
       is load-bearing, not defensive-by-style.  An earlier strand-tear-down
       path (or a future scheduler that pre-rolls-back deposits before the
       strand is destroyed) can leave the strand-level
       cross_strand_stop_pending flag asserted while the VM-level counter
       is already at 0; without the guard the uint32_t counter wraps to
       UINT32_MAX and sched_quiescent never returns true again.  Pinned by
       the strand_destroy_does_not_underflow_host_call_pending unit test. */
    if (s->cross_strand_stop_pending != 0) {
        if (vm->host_call_pending_count > 0)
            vm->host_call_pending_count--;
        s->cross_strand_stop_pending = 0U;
    }
}

/* === SCHED-004: sched_strand_unbind_from_sleep_queue ===
 *
 * Splice s out of vm->sleep_q_head if present; clear s->wait_next; decrement
 * vm->wakeup_pending_count by one iff the strand was actually on the queue.
 * Idempotent: safe to call on a strand never inserted (no-op).
 *
 * Used by paths that re-stamp a strand's state from one WAITING reason to
 * another (e.g. c_event_waituntil — though under the cooperative scheduler
 * a strand cannot legitimately be on the sleep queue when it begins event-
 * waiting; the helper exists as defence-in-depth against future schedulers
 * or buggy callers that bypass the dispatch loop). */
void
sched_strand_unbind_from_sleep_queue(UStrand *s)
{
    sleep_q_remove(s->vm, s);
}

/* === REALM-011 / T69: sched_strand_unbind_from_ready_queue ===
 *
 * Splice s out of vm->ready_head / ready_tail's doubly-linked list if
 * present; clear s->ready_next / s->ready_prev; decrement
 * vm->strand_runnable_count by one iff the strand was actually present.
 *
 * Called from urbi_realm_destroy before freeing each strand so that the
 * scheduler's queue head/tail pointers + neighbouring strands' ready_*
 * links never reference freed memory.  Without this, sched_strand_destroy
 * only zeroes the strand's own pointers — neighbours still point at the
 * freed cell, and the next dispatch (or sched_walk_roots GC scan) trips
 * use-after-free under ASan.
 *
 * Idempotent: safe whether the strand is on the queue (state == READY)
 * or not.  Detection uses neighbour-pointer presence rather than state-
 * byte inspection so it remains correct for any future scheduler that
 * temporarily detaches READY strands without state churn.  Specifically:
 *   - If s == vm->ready_head AND s->ready_next == NULL AND ready_tail ==
 *     s → strand is the sole queue member.  Both head and tail go NULL.
 *   - If s == vm->ready_head → advance head to s->ready_next.
 *   - If s == vm->ready_tail → retreat tail to s->ready_prev.
 *   - If s->ready_prev != NULL → patch prev's next.
 *   - If s->ready_next != NULL → patch next's prev. */
void
sched_strand_unbind_from_ready_queue(UStrand *s)
{
    UVM *vm = s->vm;
    /* Quick guard: detect "definitely not on the queue" cheaply.  A strand
     * is on the doubly-linked queue iff it has a prev neighbour, OR a next
     * neighbour, OR it is the queue head (sole member case).  All other
     * strands have ready_next / ready_prev = NULL AND are not at head. */
    const bool on_queue = (s->ready_prev != NULL)
                       || (s->ready_next != NULL)
                       || (vm->ready_head == s);
    if (!on_queue) {
        return;
    }

    if (s->ready_prev != NULL) {
        s->ready_prev->ready_next = s->ready_next;
    } else {
        /* s was the head. */
        vm->ready_head = s->ready_next;
    }
    if (s->ready_next != NULL) {
        s->ready_next->ready_prev = s->ready_prev;
    } else {
        /* s was the tail. */
        vm->ready_tail = s->ready_prev;
    }

    s->ready_next = NULL;
    s->ready_prev = NULL;
    if (vm->strand_runnable_count > 0) {
        vm->strand_runnable_count--;
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
 * Root sources at v0.5.x baseline (M5 shipped):
 *   (1) Register window — conservative full-stack scan (see below).
 *   (2) Unwind state — unwind_value + fatal_value are UValue fields.
 *   (3) Cleanup stack — owning_tag (UTag*) and catch_pattern (UPattern*)
 *       are NOT yielded as direct UValue roots here.  Reachability is
 *       provided indirectly: UTag was GC-promoted at M5 and is reached via
 *       the realm strand walker plus the closure references that captured
 *       it; member_strands_head's back-pointer to the cleanup entry sits
 *       on the strand stack which (1) walks.  See SCHED-012 v1.x carry-
 *       forward note below.
 *   (4) Wait payload — wait_payload.event / wait_payload.join_parent are
 *       NOT yielded here.  UEvent became a GC cell at M5, but every UEvent
 *       a strand can be waiting on is reachable through another path (realm
 *       globals for stdlib events; object's changed_events_head for
 *       slot-change events; tag's enter_event/leave_event fields for tag-
 *       scoped events).  join_parent points at another live strand reached
 *       via realm.strands_head.
 *
 * Register window strategy (row 10 §5.2 guidance):
 *   s->stack is a heap-allocated UVM_STACK_CAP-slot array.  The active
 *   register window spans frames[0..frame_count-1]; the topmost frame's
 *   extent requires bytecode metadata not available at M3.  We walk the
 *   entire allocated array (conservative over-mark; never under-marks).
 *   TODO(v1.x — frame-extent metadata): tighten to active-frame register
 *   window when bytecode emits per-frame extent metadata.  Tracked under
 *   docs/urbi-embedded-design-risks.md row "v1.x — preemptive scheduling
 *   readiness" (cooperative GC has no urgency to optimize here).
 *
 * Scratch-strand coverage (closes GC-006 + GC-038):
 *   The watcher cond/body/onleave scratch path (urbi_run_closure_on_scratch
 *   in src/watcher/uwatcher_scratch.c) builds a transient UStrand on the C
 *   stack and threads it onto vm->global_realm->strands_head BEFORE entering
 *   dispatch.  That strand's register window is therefore visited here just
 *   like any persistent strand — no separate "scratch frame" walker is
 *   required.  The audit IDs GC-006 and GC-038 were filed against an earlier
 *   (pre-v0.5.1) design that used a vm->watcher_scratch_frame field; the
 *   transient-strand architecture closes both findings by construction.  The
 *   structural invariant is regression-pinned by
 *   tests/unit/test_gc_scratch_rooting.c. */
static void
strand_walk_roots(UVM *vm, UStrand *s, UGcRootCallback cb, void *ctx)
{
    /* Guard: DEAD strands have no live roots (spec §5.2). */
    if (USTRAND_GET_STATE(s) == USTRAND_DEAD) return;

    /* (1) Register window — conservative full-stack scan.
     *     s->stack may be NULL for a DORMANT strand not yet armed. */
    if (s->stack != NULL) {
        for (int i = 0; i < UVM_STACK_CAP; i++) {
            cb(vm, &s->stack[i], ctx);
        }
    }

    /* (2) Unwind state (row 7 §4.4).
     *     unwind_value and fatal_value are UValue fields on the strand. */
    cb(vm, &s->unwind_value, ctx);
    cb(vm, &s->fatal_value,  ctx);

    /* (3) Cleanup-stack entries (row 7 §4.4).
     *     owning_tag (UTag*) was GC-promoted at M5 but is reached indirectly:
     *     a tag visible to user code is captured in the lexical closures the
     *     watcher table walker visits and / or pinned via the strand register
     *     window walked at (1).  catch_pattern (UPattern*) remains host-
     *     allocated at v1.0 (UPattern is not GC-managed).  No direct callback
     *     is needed here.
     *     TODO(v1.x — cleanup-stack walker): if a future audit identifies a
     *     UTag reachable only via cleanup_base[i].owning_tag, this loop must
     *     yield those UTag pointers as UVAL_TAG roots.  Today no such audit
     *     exists; the M5+ test corpus has not surfaced a reachability gap
     *     (see test_gc_strand_walker.c + test_gc_scratch_rooting.c). */

    /* (4) Wait payload (row 9 §4.3).
     *     wait_payload.event (struct UEvent*) and wait_payload.join_parent
     *     (UStrand*) are reached indirectly: every UEvent a strand can wait
     *     on is also reachable via realm globals (stdlib events), the
     *     owning object's changed_events_head (slot-change events), or the
     *     owning tag's enter_event/leave_event fields.  join_parent is
     *     another live strand reached via realm.strands_head.  No direct
     *     callback is needed here at v1.0. */

    /* (5) last_event_payload (spec #3 §7.1, T56).
     *     Written by c_event_emit_* before unblocking a waituntil strand.
     *     May hold a heap-bearing UValue (e.g. UVAL_OBJECT, UVAL_CLOSURE,
     *     UVAL_EVENT) between the emit and the strand's next dispatch turn.
     *     Route through cb so the mark callback applies the heap-bearing check. */
    cb(vm, &s->last_event_payload, ctx);

    /* (6) Strand entry closure (v0.8.4 Option B Step C-1).
     *     s->entry_closure is a raw UClosure* set at strand creation.
     *     Once Step C-2 promotes UClosure to urbi_gc_alloc, this pointer must
     *     keep the closure alive — without this yield, GC would sweep it.
     *     UClosure embeds UCell at offset 0; shade directly via gc_shade_gray.
     *     Today this yield is dormant: UClosure cells are not on all_cells_head,
     *     so gc_shade_gray sets the color byte and returns early (NULL sidecar
     *     path, per the GC-009 contract in ugc_incremental.c). */
    if (s->entry_closure != NULL) {
        gc_shade_gray(vm, (UCell *)&s->entry_closure->cell);
    }

    /* (7) Per-frame closures (v0.8.4 Option B Step C-1).
     *     Each active call frame holds a raw UClosure* (frames[i].closure).
     *     frame_count is the count of populated frames.  Frame 0 may have
     *     closure == NULL (top-level call into the strand entry); guard each.
     *     Same dormant-harmless property as (6). */
    {
        int i;
        for (i = 0; i < s->frame_count; i++) {
            if (s->frames[i].closure != NULL) {
                gc_shade_gray(vm, (UCell *)&s->frames[i].closure->cell);
            }
        }
    }

    /* (8) Open upvalue cells (v0.8.4 Option B Step C-1).
     *     s->open_upvals is a chain of UUpvalCell* with on_heap == false
     *     (cells still pointing into the strand's register window).  Once
     *     Step C-2 promotes UUpvalCell to urbi_gc_alloc, the chain itself is
     *     the only persistent reference to these cells until OP_CLOSE
     *     transfers ownership into a closure's upvals[].  Walk + shade each.
     *     Each cell embeds UCell at offset 0.  Same dormant-harmless property
     *     as (6): cells not on all_cells_head, gc_shade_gray is a safe no-op. */
    {
        UUpvalCell *uc = s->open_upvals;
        while (uc != NULL) {
            gc_shade_gray(vm, (UCell *)&uc->cell);
            uc = uc->next;
        }
    }
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

/* === sched_post_dispatch: consolidated post-dispatch fix-up helper ===
 *
 * Scheduler audit F3: previously all four fix-up steps lived exclusively in
 * urbi_step, so any alternative driver had to replicate them or silently lose
 * forward progress, leak memory, or corrupt the runnable count.  This helper
 * centralises them.
 *
 * The four steps (see usched_post_dispatch.h for full documentation):
 *   1. Runnable-count re-increment if strand is WAITING (double-decrement fix).
 *   2. Eager DEAD-strand reap via urbi_strand_destroy.
 *   3. Sleep-queue wake for any strand whose wake_us <= now.
 *   4. Periodic pump to re-arm any every()-body that just completed.
 *
 * Precondition: s->fatal_status was checked by the driver before this call;
 * FATAL strands must never reach here (driver returns URBI_STEP_FATAL first).
 * vm->cur_strand must be NULL on entry (driver clears it after dispatch). */
void
sched_post_dispatch(UVM *vm, UStrand *s)
{
    /* Step 1: Runnable-count re-increment (non-transient strands only).
     *
     * sched_dequeue_ready_head decremented strand_runnable_count when the strand
     * was picked.  If the strand then transitioned to WAITING inside dispatch
     * (via sched_strand_block, which also decrements for state==RUNNING), the
     * counter was double-decremented.  Re-increment to restore symmetry so the
     * scheduler does not lose track of the fact that the strand is waiting rather
     * than gone entirely.  Any future blocking opcode that calls sched_strand_block
     * from inside the dispatch loop must rely on this re-increment.
     *
     * is_transient_strand guard: urbi_vm_run transient strands bypass
     * sched_strand_make_runnable and sched_dequeue_ready_head; they manage their
     * own READY-cycle increments at the dequeue site inside the vm_run loop.
     * There is no double-decrement issue for transients; incrementing here would
     * spuriously inflate strand_runnable_count. */
    if (USTRAND_IS_WAITING(s) && !s->is_transient_strand) {
        vm->strand_runnable_count++;
    }

    /* Step 2: Eager DEAD-strand reap (heap-allocated strands only).
     *
     * Heap-allocated strands (urbi_strand_create — used for watcher bodies, fork
     * children, and any script-spawned strand) sit on realm->strands_head until
     * urbi_realm_destroy would otherwise reap them at VM teardown.  Without eager
     * reap, DEAD strands accumulate indefinitely; each carries a register stack of
     * UVM_STACK_CAP * sizeof(UValue) (~32 KB at default) so the leak climbs into
     * the multi-MB range at moderate event rates.  Surfaced on ESP-IDF eye_demo as
     * a hard wedge at ~200 body completions (v0.7.x).
     *
     * Safe to reap here because: watcher_body_owner was cleared by
     * urbi_watcher_body_completed in exit_strand; joiners were woken by
     * fork_wake_joiners; ready/sleep queues were already unbound; vm->cur_strand
     * was cleared after dispatch returned.  The FATAL path returns before reaching
     * this helper, so we never reap a strand the host still wants to inspect.
     *
     * is_transient_strand guard: urbi_vm_run creates a stack-local transient
     * strand (T33 discriminator) and manages its own teardown via ustrand_destroy
     * at function exit.  Calling urbi_strand_destroy on a stack address would
     * free a pointer that was never heap-allocated, causing UB.  Transient
     * strands are explicitly excluded from eager reap here; their lifetime is
     * bounded by the urbi_vm_run call frame.
     *
     * urbi_strand_destroy is the canonical full teardown — unlinks from
     * realm->strands_head, unbinds queues, frees cleanup stack + register stack
     * + the strand struct itself.  After this call s is freed; callers MUST NOT
     * dereference s. */
    if (s->state == USTRAND_STATE_DEAD && !s->is_transient_strand) {
        urbi_strand_destroy(vm, s);
        /* s is freed; do not dereference past this point. */
        /* Steps 3 and 4 below only use vm, so they are safe to run after free. */
    }

    /* Step 3: Sleep-queue wake.
     *
     * Walk vm->sleep_q_head and wake every strand whose wake_us <= now.
     * CHSTR-025: every node on sleep_q is REASON_SLEEP, so wake_us is the
     * active union arm; assert at the loop head to surface a queue-invariant
     * break in -DURBI_DEBUG builds. */
    if (vm->sleep_q_head != NULL && vm->host_time_us != NULL) {
        uint64_t now = vm->host_time_us(vm->host_time_ud);
        while (vm->sleep_q_head != NULL) {
            URBI_INTERNAL_ASSERT(
                USTRAND_GET_REASON(vm->sleep_q_head) == USTRAND_REASON_SLEEP);
            if (vm->sleep_q_head->wait_payload.wake_us > now) break;
            UStrand *waker = vm->sleep_q_head;
            /* sched_strand_unblock removes from sleep_q (decrementing
             * wakeup_pending_count) and calls sched_strand_make_runnable. */
            sched_strand_unblock(waker);
        }
    }

    /* Step 4: Periodic pump.
     *
     * Fire every()-body re-spawn for any periodic whose next_fire_us has elapsed.
     * Running this inside the dispatch loop (rather than only pre-loop) allows a
     * body strand that just completed to re-arm and become a READY strand within
     * the same urbi_step call without waiting for the next host-level step. */
    (void)urbi_periodic_pump(vm);
}
