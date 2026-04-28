/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: URBI_SCHED_COOPERATIVE interface skeleton (4 basic ops).
   Full scheduler test plan (10-15 cases) lands at T21. */

#include "utest.h"
#include "usched_cooperative.h"
#include "uvm.h"
#include "ustrand.h"

#define UTEST(name) static void name(void)

/* Case 1: sched_init empties the ready queue and reports quiescent. */
UTEST(sched_init_empties_ready_queue)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);
    UASSERT(vm.ready_head == NULL);
    UASSERT(vm.ready_tail == NULL);
    UASSERT_EQ(vm.strand_runnable_count, 0u);
    UASSERT(sched_pick_next(&vm) == NULL);
    UASSERT(sched_quiescent(&vm));
    uvm_destroy(&vm);
}

/* Case 2: sched_strand_make_runnable appends in FIFO order; pick_next
   returns the head; runnable_count reflects all three strands. */
UTEST(sched_make_runnable_appends_tail)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UStrand a, b, c;
    ustrand_init(&a, &vm);
    ustrand_init(&b, &vm);
    ustrand_init(&c, &vm);

    sched_strand_make_runnable(&a);
    sched_strand_make_runnable(&b);
    sched_strand_make_runnable(&c);

    UASSERT(sched_pick_next(&vm) == &a);
    UASSERT_EQ(vm.strand_runnable_count, 3u);
    UASSERT(!sched_quiescent(&vm));

    ustrand_destroy(&a, &vm);
    ustrand_destroy(&b, &vm);
    ustrand_destroy(&c, &vm);
    uvm_destroy(&vm);
}

/* Case 3: sched_earliest_wake_us returns UINT64_MAX when sleep queue is empty. */
UTEST(sched_earliest_wake_uintmax_on_empty)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);
    UASSERT_EQ(sched_earliest_wake_us(&vm), UINT64_MAX);
    uvm_destroy(&vm);
}

/* Case 4: sched_consume_budget decrements correctly and floors at zero. */
UTEST(sched_consume_budget_floors_at_zero)
{
    UStrand s;
    s.instruction_budget_remaining = 5;
    sched_consume_budget(&s, 3);
    UASSERT_EQ(s.instruction_budget_remaining, 2u);
    sched_consume_budget(&s, 10);   /* would underflow without the floor */
    UASSERT_EQ(s.instruction_budget_remaining, 0u);
}

void test_scheduler_cooperative_suite(void) {
    utest_run("sched_init_empties_ready_queue",      sched_init_empties_ready_queue);
    utest_run("sched_make_runnable_appends_tail",    sched_make_runnable_appends_tail);
    utest_run("sched_earliest_wake_uintmax_on_empty", sched_earliest_wake_uintmax_on_empty);
    utest_run("sched_consume_budget_floors_at_zero", sched_consume_budget_floors_at_zero);
}
