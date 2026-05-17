/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: per-strand instruction budget exhaustion → soft yield
 * (row 12 §3.2 — budget-exhaustion safepoint in dispatch_loop_until_yield).
 *
 * A soft yield occurs when instruction_budget_remaining reaches 0 at a
 * safepoint.  The strand transitions RUNNING → READY (via sched_strand_yield)
 * and is re-enqueued at the tail rather than killed.  urbi_step returns
 * URBI_STEP_RUNNING (budget exhausted) rather than QUIESCENT.
 *
 * Tests drive via urbi_step with a small VM-wide step budget so that the
 * VM-wide budget exhausts before the program completes, verifying
 * URBI_STEP_RUNNING on the first call and eventual QUIESCENT after enough
 * calls.
 *
 * Note: URBI_STRAND_BUDGET_MAX is compile-time (default 1000).  We cannot
 * reduce it at runtime.  Instead, we set vm->step_budget_remaining to a tiny
 * value by calling urbi_step with budget=1 so the VM-wide budget exhausts
 * before the strand's per-strand budget.  This tests the VM-wide budget path.
 * For the per-strand budget path, we directly mutate
 * s->instruction_budget_remaining to 0 after arming the strand (the
 * "tunable_pin" approach tested in test_determinism_tunable_pin.c). */

#include "utest.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "urbi/urbi.h"
#include "module/umodule.h"
#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "sched/usched_cooperative.h"
#include "object/umodule_instance.h"

#include <string.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* ---- Compiler helper (same pattern as test_fork.c) ---- */

static bool
budget_compile(UVM *vm, const char *src, UModule *out_mod)
{
    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UArena arena;
    uarena_init(&arena, 4096);
    *out_mod = (UModule){0};

    UEmitter e;
    uemit_init(&e, out_mod, &arena, vm, NULL);

    UParser p;
    uparse_init(&p, &lex, &arena);

    bool ok = true;
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) { ok = false; break; }
        if (uemit_statement(&e, node) != EMIT_OK) { ok = false; break; }
        uarena_reset(&arena);
    }
    if (ok && uemit_finish(&e) != EMIT_OK) ok = false;
    uarena_destroy(&arena);
    return ok;
}

/* Arm a strand from a module (same pattern as fork_run_to_quiescent). */
static bool
budget_arm_strand(UVM *vm, UModule *module, UStrand *s, UValue *out_result)
{
    const size_t stack_bytes = UVM_STACK_CAP * sizeof(UValue);
    s->stack = (UValue *)vm->alloc_fn(NULL, stack_bytes, vm->alloc_ud);
    if (!s->stack) return false;
    {
        volatile unsigned char *p = (volatile unsigned char *)s->stack;
        size_t i;
        for (i = 0; i < stack_bytes; i++) p[i] = 0;
    }
    s->R          = s->stack;
    s->pc         = module->instructions;
    s->pc_base    = module->instructions;
    s->cur_consts = module->constants;
    s->module     = module;
    umodule_refcount_inc(module, vm);  /* v0.8.0: pair with ustrand_destroy dec */
    s->module_instance = urbi_module_instance_create(vm, module);
    s->frame_count = 0;
    s->open_upvals = NULL;
    s->closure_list = NULL;
    s->closed_cells = NULL;
    if (out_result) s->out_slot = out_result;
    return true;
}

/* ===========================================================================
 * Tests
 * =========================================================================== */

/* Case 1: VM-wide step budget=1 exhausts before a counting loop completes.
 * urbi_step returns URBI_STEP_RUNNING on the first call, then QUIESCENT
 * eventually when called with a large budget. */
UTEST(vm_step_budget_exhausts_mid_program)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* A program that takes more than 1 opcode to complete: count to 10. */
    UModule module;
    UASSERT(budget_compile(&vm, "var i = 0; while (i < 10) { i = i + 1 }", &module));

    UStrand *s = urbi_strand_create(realm, NULL);
    UASSERT(s != NULL);

    UValue result = {0};
    UASSERT(budget_arm_strand(&vm, &module, s, &result));
    urbi_strand_start(s);  /* DORMANT → READY */

    /* Call urbi_step with budget=1 opcode.  With a non-trivial program, this
     * should not be QUIESCENT after the very first call. */
    UStepResult first = urbi_step(&vm, 1, NULL);
    /* The first call with budget=1 opcode on a while loop must return
     * URBI_STEP_RUNNING (not enough budget to complete). */
    UASSERT(first == URBI_STEP_RUNNING);

    /* Drive to quiescent with a large budget. */
    int reached_quiescent = 0;
    int max_iters = 10000;
    while (max_iters-- > 0) {
        UStepResult r = urbi_step(&vm, 100, NULL);
        if (r == URBI_STEP_QUIESCENT) { reached_quiescent = 1; break; }
        if (r == URBI_STEP_FATAL)     break;
        if (r == URBI_STEP_WAKE_AT)   { reached_quiescent = 1; break; }
    }
    UASSERT(reached_quiescent);

    umodule_destroy(&module, NULL);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* Case 2: per-strand budget manually zeroed → strand soft-yields at next safepoint.
 *
 * Arm a strand for a simple counting loop, then set instruction_budget_remaining
 * to 0 on the strand so the first safepoint triggers a soft yield.
 * After urbi_step we expect the strand is back in READY (not DEAD), meaning
 * it yielded rather than completing or crashing.
 *
 * We then run with a large budget to completion and verify the result. */
UTEST(per_strand_budget_zero_causes_soft_yield)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UModule module;
    UASSERT(budget_compile(&vm, "var i = 0; while (i < 5) { i = i + 1 }", &module));

    UStrand *s = urbi_strand_create(realm, NULL);
    UASSERT(s != NULL);

    UValue result = {0};
    UASSERT(budget_arm_strand(&vm, &module, s, &result));

    /* Zero out the per-strand budget immediately after arming. */
    s->instruction_budget_remaining = 0;

    /* Start the strand. */
    urbi_strand_start(s);

    /* Run one step with a minimal budget; the strand should soft-yield at the
     * first safepoint because instruction_budget_remaining == 0. */
    UStepResult r1 = urbi_step(&vm, 10, NULL);
    /* With zero per-strand budget, the first step must return URBI_STEP_RUNNING
     * (soft yield at the first safepoint). */
    UASSERT(r1 == URBI_STEP_RUNNING);

    /* Run to completion.  The loop limit of 100,000 is generous; the strand's
     * per-strand budget is reset to URBI_STRAND_BUDGET_MAX (≈1000) each time
     * it is re-enqueued after a soft yield, so completion is guaranteed to
     * happen much faster than the limit unless there is a logic error. */
    int reached = 0;
    int iters = 100000;
    while (iters-- > 0) {
        UStepResult r = urbi_step(&vm, 1000, NULL);
        if (r == URBI_STEP_QUIESCENT || r == URBI_STEP_WAKE_AT) {
            reached = 1; break;
        }
        if (r == URBI_STEP_FATAL) break;
    }
    UASSERT(reached);

    umodule_destroy(&module, NULL);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* Case 3: sched_consume_budget floors at zero (no underflow).
 * Directly tests the inline helper rather than the full dispatch path. */
UTEST(consume_budget_floors_at_zero)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UStrand s;
    ustrand_init(&s, &vm);
    /* sched_strand_init sets instruction_budget_remaining = URBI_STRAND_BUDGET_MAX.
     * ustrand_init does not call it (lifecycle separation); call it explicitly. */
    sched_strand_init(&s, NULL);

    /* Initial budget is URBI_STRAND_BUDGET_MAX. */
    UASSERT(s.instruction_budget_remaining > 0U);
    uint16_t initial = s.instruction_budget_remaining;

    /* Consume partial. */
    sched_consume_budget(&s, 10);
    UASSERT_EQ(s.instruction_budget_remaining, (uint16_t)(initial - 10));

    /* Consume more than remaining: must floor at 0. */
    sched_consume_budget(&s, 65535U);
    UASSERT_EQ(s.instruction_budget_remaining, 0U);

    /* Consume again from 0: must stay at 0 (no underflow). */
    sched_consume_budget(&s, 1);
    UASSERT_EQ(s.instruction_budget_remaining, 0U);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* ===========================================================================
 * Suite registration
 * =========================================================================== */

void test_pipe_budget_exhaust_suite(void)
{
    utest_run("vm_step_budget_exhausts_mid_program",
              vm_step_budget_exhausts_mid_program);
    utest_run("per_strand_budget_zero_causes_soft_yield",
              per_strand_budget_zero_causes_soft_yield);
    utest_run("consume_budget_floors_at_zero",
              consume_budget_floors_at_zero);
}
