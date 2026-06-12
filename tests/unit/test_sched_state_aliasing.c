/* SPDX-License-Identifier: BSD-3-Clause */
/* Phase 8 (T38-T41): scheduler state-byte aliasing regressions.
 *
 * Audit IDs closed:
 *   SCHED-001 — JOIN/WAIT_EVENT state-byte alias (uunwind.c discriminator)
 *   SCHED-002 — sched_strand_block re-block already-WAITING (entry-state assert)
 *   SCHED-003 — sched_strand_yield re-yield already-READY (entry-state assert)
 *   SCHED-004 — c_event_waituntil re-stamp leaves stale sleep-queue links
 *   SCHED-005 — sched_strand_make_runnable idempotence assertion
 */

#include "utest.h"
#include "sched/usched_cooperative.h"
#include "sched/ustrand.h"
#include "vm/uvm.h"
#include <stddef.h>
#include <stdint.h>

#ifdef URBI_DEBUG
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* EXPECT_ABORT: assert that expr causes abort (via assert() failure).
 * Uses fork+waitpid: child executes expr; parent verifies abnormal exit.
 * Only meaningful in URBI_DEBUG builds where URBI_INTERNAL_ASSERT is assert(). */
#define EXPECT_ABORT(expr)                                                   \
    do {                                                                     \
        utest_checks++;                                                      \
        pid_t _pid = fork();                                                 \
        if (_pid == 0) {                                                     \
            (expr);                                                          \
            _exit(0); /* should not reach — abort expected */                \
        }                                                                    \
        int _st = 0;                                                         \
        waitpid(_pid, &_st, 0);                                              \
        int _aborted = WIFSIGNALED(_st) ||                                   \
                       (WIFEXITED(_st) && WEXITSTATUS(_st) != 0);            \
        if (!_aborted) {                                                     \
            utest_failures++;                                                \
            printf("  FAIL: %s:%d: " #expr " did not abort\n",               \
                   __FILE__, __LINE__);                                      \
            fflush(stdout);                                                  \
        }                                                                    \
    } while (0)
#endif /* URBI_DEBUG */

#define UTEST(name) static void name(void)

/* ===================================================================
 * T38 — SCHED-001: JOIN-blocked strand state distinct from WAIT_EVENT
 * ===================================================================
 *
 * After the v0.5.5 CHSTR-016 renumbering, REASON_JOIN = 0x04 produces
 * composite 0x34, distinct from WAIT_EVENT's 0x33.  But the architectural
 * fix is to discriminate by class+reason, not by full-state equality.
 * This test pins both invariants:
 *   (a) the literal byte is distinct (ensures the renumbering holds);
 *   (b) the class+reason discriminator agrees with the production
 *       counterpart: strand_unlink_park (src/sched/ustrand.c) dispatches
 *       on USTRAND_GET_REASON, never on full-state equality (v0.13.3:
 *       it replaced uunwind.c's is_event_parked_strand helper). */
UTEST(join_blocked_strand_state_distinct_from_wait_event)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand parent, child;
    ustrand_init(&parent, &vm);
    ustrand_init(&child,  &vm);

    /* Simulate child currently RUNNING about to block on parent. */
    child.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;

    sched_strand_block(&child, USTRAND_REASON_JOIN, (uint64_t)(uintptr_t)&parent);

    /* (a) literal byte distinct from WAIT_EVENT. */
    UASSERT(child.state != USTRAND_WAIT_EVENT);
    UASSERT_EQ((int)child.state, (int)USTRAND_STATE_WAITING_JOIN);
    /* class = WAITING; reason = JOIN, not EVENT. */
    UASSERT_EQ((int)(child.state & USTRAND_STATE_MASK), (int)USTRAND_WAITING);
    UASSERT_EQ((int)USTRAND_GET_REASON(&child), (int)USTRAND_REASON_JOIN);
    UASSERT(USTRAND_GET_REASON(&child) != USTRAND_REASON_EVENT);

    ustrand_destroy(&parent, &vm);
    ustrand_destroy(&child,  &vm);
    urbi_vm_destroy(&vm);
}

/* The audit's runtime concern: uunwind.c used `s->state == USTRAND_WAIT_EVENT`
 * to gate uevent_waiter_unregister.  The production counterpart is now
 * strand_unlink_park (src/sched/ustrand.c, v0.13.3 / SCHED-05), which
 * dispatches on USTRAND_GET_REASON — the shape (class == WAITING &&
 * reason == EVENT) for the event arm.  We replicate the predicate locally
 * and cross-check that it reports the right answer for every interesting
 * state.
 *
 * If the production dispatch regresses to full-state equality and a future
 * renumbering re-introduces an alias (e.g., reason 0x05 + WAITING = 0x35
 * colliding with some new state) the (a) check above would also break.
 * Together the two tests cover both the literal-byte and the structural
 * pattern. */
static inline int is_event_parked_local(const UStrand *s) {
    return ((s->state & USTRAND_STATE_MASK) == USTRAND_WAITING &&
            (s->state & USTRAND_REASON_MASK) == USTRAND_REASON_EVENT);
}

UTEST(is_event_parked_predicate_distinguishes_reason_byte)
{
    UStrand s;
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);

    /* RUNNING — class != WAITING. */
    s.state = USTRAND_STATE_RUNNING;
    UASSERT(!is_event_parked_local(&s));

    /* WAITING_SLEEP — class WAITING, reason SLEEP. */
    s.state = USTRAND_STATE_WAITING_SLEEP;
    UASSERT(!is_event_parked_local(&s));

    /* WAITING_JOIN — class WAITING, reason JOIN. */
    s.state = USTRAND_STATE_WAITING_JOIN;
    UASSERT(!is_event_parked_local(&s));

    /* WAITING_EVENT — class WAITING, reason EVENT. */
    s.state = USTRAND_STATE_WAITING_EVENT;
    UASSERT(is_event_parked_local(&s));

    /* USTRAND_WAIT_EVENT macro is the same composite. */
    s.state = USTRAND_WAIT_EVENT;
    UASSERT(is_event_parked_local(&s));

    /* v0.13.3 (SCHED-13): restore DORMANT before teardown — the raw stamps
     * above bypass sched_strand_block, so the strand was never counted in
     * strand_waiting_count; destroying it in a WAITING state would trip
     * the no-saturation decrement.  This test only exercises the
     * state-byte predicate. */
    s.state = USTRAND_STATE_DORMANT;

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T39 — SCHED-002: sched_strand_block requires entry state RUNNING/READY
 * ===================================================================
 *
 * Re-blocking a strand that's already WAITING re-inserts via sleep_q_insert,
 * causing duplicate counter increments and possible queue cycle.  Fix:
 * URBI_INTERNAL_ASSERT(s->state == RUNNING || s->state == READY) at entry.
 *
 * Positive-path test: block from RUNNING → WAITING_SLEEP — counters correct,
 * single sleep-queue node.  Aborts only fire under URBI_DEBUG; the EXPECT_ABORT
 * branch verifies the entry-state assert is wired in debug builds. */
UTEST(block_from_running_state_correct)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    s.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;

    sched_strand_block(&s, USTRAND_REASON_SLEEP, 1000U);

    UASSERT_EQ((int)(s.state & USTRAND_STATE_MASK), (int)USTRAND_WAITING);
    UASSERT_EQ(vm.wakeup_pending_count, 1U);
    UASSERT(vm.sleep_q_head == &s);
    UASSERT(s.wait_next == NULL);  /* lone occupant */

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

#ifdef URBI_DEBUG
/* Helper invoked inside the forked child of EXPECT_ABORT below.  Sets up a
 * SLEEP-blocked strand then re-blocks it — the second sched_strand_block must
 * trip URBI_INTERNAL_ASSERT and abort the child. */
static void
reblock_waiting_strand(void)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    /* First block: legitimate RUNNING → WAITING_SLEEP. */
    s.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;
    sched_strand_block(&s, USTRAND_REASON_SLEEP, 1000U);

    /* Second block: WAITING → WAITING_SLEEP — must abort. */
    sched_strand_block(&s, USTRAND_REASON_SLEEP, 2000U);
}

UTEST(reblock_already_waiting_strand_aborts_in_debug)
{
    EXPECT_ABORT(reblock_waiting_strand());
}
#endif

/* ===================================================================
 * T40 — SCHED-003: sched_strand_yield requires entry state RUNNING
 * ===================================================================
 *
 * Re-yielding a READY strand silently re-enqueues, double-counting
 * runnable_count and producing a circular ready_next/prev chain.
 * Fix: URBI_INTERNAL_ASSERT(s->state == USTRAND_RUNNING) at entry.
 * The two live call sites in uvm.c (OP_YIELD and the safepoint
 * budget-exhaust path) had each pre-set state to READY before the
 * yield call; that pre-set was redundant (sched_strand_make_runnable
 * sets state unconditionally) and is removed in the same commit. */
UTEST(yield_from_running_makes_ready)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    /* SCHED-01: a RUNNING strand is counted; yield is count-neutral. */
    s.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;
    sched_strand_yield(&s);

    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_READY);
    UASSERT_EQ(vm.strand_runnable_count, 1U);
    UASSERT(vm.ready_head == &s);
    UASSERT(vm.ready_tail == &s);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

#ifdef URBI_DEBUG
static void
yield_already_ready(void)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    /* First yield: legitimate RUNNING → READY (counted; SCHED-01). */
    s.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;
    sched_strand_yield(&s);
    /* state now READY, on ready queue. */

    /* Second yield from READY — must abort. */
    sched_strand_yield(&s);
}

UTEST(yield_already_ready_strand_aborts_in_debug)
{
    EXPECT_ABORT(yield_already_ready());
}
#endif

/* ===================================================================
 * T41 — SCHED-004: re-stamp through sched_strand_unbind_from_sleep_queue
 * ===================================================================
 *
 * Direct exercise of the helper.  c_event_waituntil's full path requires
 * a constructed UEvent + cur_strand wiring (covered by existing
 * test_event_waituntil suite); here we focus on the load-bearing helper
 * contract: idempotence + correct counter decrement. */
UTEST(unbind_from_sleep_queue_clears_links_when_present)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    /* Setup: place strand on sleep queue (sole occupant). */
    s.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;
    sched_strand_block(&s, USTRAND_REASON_SLEEP, 5000U);
    UASSERT(vm.sleep_q_head == &s);
    UASSERT_EQ(vm.wakeup_pending_count, 1U);

    /* Helper unbinds the strand: queue empty, counter zero, wait_next NULL. */
    sched_strand_unbind_from_sleep_queue(&s);
    UASSERT(vm.sleep_q_head == NULL);
    UASSERT_EQ(vm.wakeup_pending_count, 0U);
    UASSERT(s.wait_next == NULL);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

UTEST(unbind_from_sleep_queue_idempotent_when_not_on_queue)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);
    /* Strand never put on sleep queue — wait_next is NULL by ustrand_init. */

    /* No-op: counter must not underflow, wait_next stays NULL. */
    sched_strand_unbind_from_sleep_queue(&s);
    UASSERT(vm.sleep_q_head == NULL);
    UASSERT_EQ(vm.wakeup_pending_count, 0U);
    UASSERT(s.wait_next == NULL);

    /* Repeated call still no-op (counter stays at 0, no underflow). */
    sched_strand_unbind_from_sleep_queue(&s);
    UASSERT_EQ(vm.wakeup_pending_count, 0U);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* When a sleep-blocked strand is "manually" re-stamped to WAIT_EVENT (the
 * pathological pattern SCHED-004 guards against), the unbind helper must
 * splice it out cleanly so the sleep queue's invariants hold afterwards.
 * This simulates the worst case the audit identified: a stale wait_next
 * pointer that would otherwise survive into the WAIT_EVENT state. */
/* ===================================================================
 * SCHED-005: sched_strand_make_runnable idempotence assertion
 * ===================================================================
 *
 * Calling sched_strand_make_runnable on a strand already in READY state
 * tail-inserts it a second time, producing a circular ready_next/ready_prev
 * chain and double-counting strand_runnable_count.  Fix: assert
 * (s->state != USTRAND_STATE_READY) at entry.  Aborts only fire under
 * URBI_DEBUG; positive path (DORMANT → READY) verifies the legitimate
 * caller invariants. */
UTEST(make_runnable_from_dormant_correct)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);
    /* state at init is DORMANT — make_runnable is the legitimate transition. */

    sched_strand_make_runnable(&s);

    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_READY);
    UASSERT_EQ(vm.strand_runnable_count, 1U);
    UASSERT(vm.ready_head == &s);
    UASSERT(vm.ready_tail == &s);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

#ifdef URBI_DEBUG
static void
make_runnable_already_ready(void)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand s;
    ustrand_init(&s, &vm);

    /* First make_runnable: legitimate DORMANT → READY. */
    sched_strand_make_runnable(&s);
    /* state now READY, on ready queue. */

    /* Second make_runnable from READY — must abort. */
    sched_strand_make_runnable(&s);
}

UTEST(make_runnable_already_ready_aborts_in_debug)
{
    EXPECT_ABORT(make_runnable_already_ready());
}
#endif

UTEST(unbind_then_restamp_clears_stale_sleep_queue_link)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    /* Two sleep-blocked strands so unbind exercises the mid-list splice. */
    UStrand a, b;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);

    a.state = USTRAND_STATE_RUNNING;
    b.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 2;

    sched_strand_block(&a, USTRAND_REASON_SLEEP, 100U);
    sched_strand_block(&b, USTRAND_REASON_SLEEP, 200U);
    UASSERT(vm.sleep_q_head == &a);
    UASSERT(a.wait_next == &b);
    UASSERT_EQ(vm.wakeup_pending_count, 2U);

    /* Pathological: re-stamp a's state to WAIT_EVENT.  Without unbind a
     * would still link to b on the sleep queue. */
    sched_strand_unbind_from_sleep_queue(&a);
    a.state = USTRAND_WAIT_EVENT;

    UASSERT(a.wait_next == NULL);
    UASSERT_EQ(vm.wakeup_pending_count, 1U);
    UASSERT(vm.sleep_q_head == &b);
    UASSERT(b.wait_next == NULL);  /* still tail of queue */

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    urbi_vm_destroy(&vm);
}

void test_sched_state_aliasing_suite(void) {
    utest_run("join_blocked_strand_state_distinct_from_wait_event",
              join_blocked_strand_state_distinct_from_wait_event);
    utest_run("is_event_parked_predicate_distinguishes_reason_byte",
              is_event_parked_predicate_distinguishes_reason_byte);
    utest_run("block_from_running_state_correct",
              block_from_running_state_correct);
#ifdef URBI_DEBUG
    utest_run("reblock_already_waiting_strand_aborts_in_debug",
              reblock_already_waiting_strand_aborts_in_debug);
#endif
    utest_run("yield_from_running_makes_ready",
              yield_from_running_makes_ready);
#ifdef URBI_DEBUG
    utest_run("yield_already_ready_strand_aborts_in_debug",
              yield_already_ready_strand_aborts_in_debug);
#endif
    utest_run("unbind_from_sleep_queue_clears_links_when_present",
              unbind_from_sleep_queue_clears_links_when_present);
    utest_run("unbind_from_sleep_queue_idempotent_when_not_on_queue",
              unbind_from_sleep_queue_idempotent_when_not_on_queue);
    utest_run("unbind_then_restamp_clears_stale_sleep_queue_link",
              unbind_then_restamp_clears_stale_sleep_queue_link);
    utest_run("make_runnable_from_dormant_correct",
              make_runnable_from_dormant_correct);
#ifdef URBI_DEBUG
    utest_run("make_runnable_already_ready_aborts_in_debug",
              make_runnable_already_ready_aborts_in_debug);
#endif
}
