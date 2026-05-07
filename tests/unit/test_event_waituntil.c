/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: c_event_waituntil C primitive (spec #3 §7.1).
 *
 * Source-level tests require T53 opcode binding and globals, so we use the
 * direct C-API path:
 *   1. waituntil_from_scratch_warns_and_returns_nil:
 *      Call c_event_waituntil with in_watcher_scratch=1; expect NIL + warn.
 *   2. waituntil_appends_to_waiters_head:
 *      Set vm->cur_strand, call c_event_waituntil (which blocks the strand
 *      and returns); verify the strand is on e->waiters_head and in
 *      USTRAND_WAIT_EVENT state.
 *
 * Note on case 2: c_event_waituntil calls sched_strand_block which transitions
 * the strand to WAITING and decrements strand_runnable_count.  We pre-set
 * state to USTRAND_STATE_RUNNING (as if the strand were dispatching) so the
 * block path finds the correct initial state.  The function returns after
 * blocking (it does NOT coroutine-switch to another strand — that is the
 * dispatch loop's job via goto exit_strand in T53). */

#include "utest.h"

#include "uevent.h"
#include "uevent_emit.h"
#include "ustrand.h"
#include "vm/uvm.h"
#include "sched/usched_cooperative.h"
#include "urbi/urbi.h"   /* URBI_LOG_WARN */

#include <stddef.h>
#include <string.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers
 * =================================================================== */

static int g_warn_count;

static void
capture_log(struct UVM *vm, int level, const char *fmt, ...)
{
    (void)vm; (void)fmt;
    if (level == URBI_LOG_WARN) g_warn_count++;
}

/* ===================================================================
 * Case 1: waituntil_from_scratch_warns_and_returns_nil
 *
 * c_event_waituntil must short-circuit when in_watcher_scratch is set:
 *   - Return NIL without modifying waiters_head.
 *   - Emit exactly one URBI_LOG_WARN.
 * =================================================================== */

UTEST(waituntil_from_scratch_warns_and_returns_nil)
{
    UVM vm;

    uvm_init(&vm, NULL, NULL);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    g_warn_count = 0;
    vm.host_log_fn = capture_log;

    vm.in_watcher_scratch = 1;
    UValue r = c_event_waituntil(&vm, e);
    vm.in_watcher_scratch = 0;

    /* Must return NIL. */
    UASSERT_EQ((int)r.kind, (int)UVAL_NIL);

    /* Must have emitted exactly one URBI_LOG_WARN. */
    UASSERT_EQ(g_warn_count, 1);

    /* waiters_head must be untouched. */
    UASSERT(e->waiters_head == NULL);

    uvm_destroy(&vm);
}

/* ===================================================================
 * Case 2: waituntil_appends_to_waiters_head
 *
 * Call c_event_waituntil with a real cur_strand set.  The function
 * must:
 *   - Append the strand to e->waiters_head.
 *   - Set the strand state to USTRAND_WAIT_EVENT.
 *   - Set wait_event_target = e.
 *   - Return (sched_strand_block does NOT switch context).
 *
 * After the call, manually wake the strand (simulating c_event_emit_*)
 * and verify last_event_payload is returned correctly on a second
 * c_event_waituntil call — but for now we only test the park path, since
 * the resume path requires bytecode dispatch (T53).
 * =================================================================== */

UTEST(waituntil_appends_to_waiters_head)
{
    UVM vm;

    uvm_init(&vm, NULL, NULL);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    /* Set up a strand as the "currently running" strand. */
    UStrand s;
    ustrand_init(&s, &vm);
    /* Put strand in RUNNING state and increment the runnable count so
     * sched_strand_block's decrement does not underflow. */
    s.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;
    vm.cur_strand = &s;

    UValue ret = c_event_waituntil(&vm, e);

    /* The strand should be parked on the waiter list. */
    UASSERT(e->waiters_head == &s);
    UASSERT(s.wait_event_target == e);
    UASSERT(s.next_event_waiter == NULL);

    /* State must be USTRAND_WAIT_EVENT (0x33). */
    UASSERT_EQ((int)s.state, (int)USTRAND_WAIT_EVENT);

    /* Function returns last_event_payload which was set to NIL at park time. */
    UASSERT_EQ((int)ret.kind, (int)UVAL_NIL);

    vm.cur_strand = NULL;
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_event_waituntil_suite(void)
{
    printf("test_event_waituntil\n");
    utest_run("waituntil_from_scratch_warns_and_returns_nil",
              waituntil_from_scratch_warns_and_returns_nil);
    utest_run("waituntil_appends_to_waiters_head",
              waituntil_appends_to_waiters_head);
}
