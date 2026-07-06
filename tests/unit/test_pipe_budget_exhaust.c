/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: per-strand instruction budget exhaustion → soft yield
 * (row 12 §3.2 — budget-exhaustion safepoint in urbi_vm_dispatch_loop_until_yield).
 *
 * A soft yield occurs when safepoint_budget_remaining reaches 0 at a
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
 * s->safepoint_budget_remaining to 0 after arming the strand (the
 * "tunable_pin" approach tested in test_determinism_tunable_pin.c). */

#include "utest.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "urbi/urbi.h"
#include "chunk/uchunk.h"
#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "sched/usched_cooperative.h"
#include "object/uchunk_instance.h"

#include <string.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* ---- Compiler helper (same pattern as test_fork.c) ---- */

static bool
budget_compile(UVM *vm, const char *src, UProto *out_mod)
{
    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UArena arena;
    uarena_init(&arena, 4096);
    *out_mod = (UProto){0};

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
budget_arm_strand(UVM *vm, UProto *module, UStrand *s, UValue *out_result)
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
    s->root_proto = module;
    s->pc         = module->instructions;
    s->pc_base    = module->instructions;
    s->cur_consts = module->constants;
    /* v0.10.1 W4: use typed-handle acquire so g_strand_ref_total stays balanced. */
    urbi_proto_strand_ref_acquire(module, URBI_PROTO_REF_OWNER_STRAND);
    s->module_instance = urbi_chunk_instance_create(vm, module);
    s->frame_count = 0;
    s->open_upvals = NULL;
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
    UProto module;
    UASSERT(budget_compile(&vm, "var i = 0; while (i < 10) { i = i + 1 }", &module));

    UStrand *s = urbi_strand_create(&vm, realm, NULL);
    UASSERT(s != NULL);

    UValue result = {0};
    UASSERT(budget_arm_strand(&vm, &module, s, &result));
    urbi_strand_start(&vm, s);  /* DORMANT → READY */

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

    uchunk_destroy(&module, NULL);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* Case 2: manually zeroing the per-strand budget does NOT stall the strand —
 * the per-slice re-arm overrides it and the strand still completes.
 *
 * Arm a strand for a counting loop and zero safepoint_budget_remaining before
 * starting.  Pre-fix (per-lifetime seed) that zero would soft-yield at the
 * first safepoint; post-fix (refactor-3 VM-04/SCHED-11) ustep.c re-arms the
 * budget to URBI_STRAND_BUDGET_MAX at each dispatch slice, so the zero is
 * overwritten before the first safepoint and forward progress is unaffected.
 * We drive to completion with a normal budget and verify QUIESCENT. */
UTEST(zeroed_strand_budget_is_rearmed_and_completes)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* Loop bound 50 (refactor-3 FE-01): each backward branch costs exactly
     * ONE safepoint slice.  50 iterations >> the 1-step VM budget used for
     * the initial probe, so the first step returns URBI_STEP_RUNNING before
     * the program completes, which is what this case asserts. */
    UProto module;
    UASSERT(budget_compile(&vm, "var i = 0; while (i < 50) { i = i + 1 }", &module));

    UStrand *s = urbi_strand_create(&vm, realm, NULL);
    UASSERT(s != NULL);

    UValue result = {0};
    UASSERT(budget_arm_strand(&vm, &module, s, &result));

    /* Zero out the per-strand budget immediately after arming. */
    s->safepoint_budget_remaining = 0;

    /* Start the strand. */
    urbi_strand_start(&vm, s);

    /* Run one step with a tight VM budget (1 step-unit).  The `;` separator
     * between `var i = 0` and `while` emits OP_YIELD, which uses the single
     * step-unit and exits with the strand READY.  The per-strand budget is
     * re-armed to URBI_STRAND_BUDGET_MAX at each dispatch start (refactor-3
     * VM-04/SCHED-11 fix), so the manually-zeroed safepoint_budget_remaining
     * does not prevent forward progress; it is overridden before the first
     * safepoint is reached. */
    UStepResult r1 = urbi_step(&vm, 1, NULL);
    /* The strand yielded (READY) rather than completing; urbi_step must
     * report RUNNING because runnable_count > 0. */
    UASSERT(r1 == URBI_STEP_RUNNING);

    /* Run to completion.  The loop limit of 100,000 is generous; with the
     * re-arm in place the strand's 50-iteration loop completes in a single
     * dispatch call when given a step budget of 1000, so QUIESCENT is
     * reached well within the limit unless there is a logic error. */
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

    uchunk_destroy(&module, NULL);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* ===========================================================================
 * Suite registration
 * =========================================================================== */

void test_pipe_budget_exhaust_suite(void)
{
    utest_run("vm_step_budget_exhausts_mid_program",
              vm_step_budget_exhausts_mid_program);
    utest_run("zeroed_strand_budget_is_rearmed_and_completes",
              zeroed_strand_budget_is_rearmed_and_completes);
}
