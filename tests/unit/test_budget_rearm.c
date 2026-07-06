/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit test: per-slice safepoint budget re-arm (refactor-3 VM-04/SCHED-11).
 *
 * Bug: the per-strand safepoint budget (safepoint_budget_remaining) was seeded
 * once per strand LIFETIME via urbi_sched_strand_init.  After URBI_STRAND_BUDGET_MAX
 * safepoints every subsequent safepoint yields BEFORE the GC-slice/drain
 * section, so a lone long-lived loop stops collecting garbage entirely.
 *
 * Fix: re-arm the budget per dispatch SLICE (in ustep.c and the urbi_vm_run
 * adapter loop) so every fresh dispatch window gets a full budget window.
 *
 * Oracle strategy: directly zero safepoint_budget_remaining to simulate an
 * exhausted strand, set gc_pending = 1 so GC wants to run at the next
 * safepoint, then dispatch one slice.
 *   Pre-fix: budget stays 0 → safepoint yields before the GC check →
 *            gc_slices stays 0.
 *   Post-fix: dispatch re-arms budget to URBI_STRAND_BUDGET_MAX → safepoint
 *             reaches the GC check → gc_slices advances. */

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

/* ---- Local compile helper (matches test_determinism_tunable_pin.c) ---- */

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

/* ---- Local strand-arm helper (matches test_determinism_tunable_pin.c) ---- */

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
    s->R           = s->stack;
    s->root_proto  = module;
    s->pc          = module->instructions;
    s->pc_base     = module->instructions;
    s->cur_consts  = module->constants;
    /* v0.10.1 W4: typed-handle acquire so g_strand_ref_total stays balanced. */
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

/* Per-slice safepoint budget re-arm (refactor-3 VM-04/SCHED-11; v0.13.1-E).
 *
 * A strand whose safepoint budget is exhausted (safepoint_budget_remaining==0)
 * must still reach the GC check at the first safepoint of the NEXT dispatch
 * slice — because ustep.c re-arms the budget to URBI_STRAND_BUDGET_MAX at
 * each slice start.
 *
 * Oracle: directly zero safepoint_budget_remaining to simulate a mature strand,
 * arm gc_pending = 1, dispatch one slice, and check gc_slices > 0.
 *   Pre-fix: budget stays 0 at dispatch entry → safepoint yields before GC →
 *            gc_slices stays 0.
 *   Post-fix: dispatch re-arms budget → safepoint reaches the GC check →
 *             gc_slices > 0. */
UTEST(mature_strand_still_runs_gc_slices)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* Non-allocating loop — only backward-branch safepoints.  GC can only
     * run if gc_pending is set, which we control explicitly below. */
    UProto module;
    UASSERT(budget_compile(&vm,
        "var i = 0; while (i < 5000) { i = i + 1 }",
        &module));

    UStrand *s = urbi_strand_create(&vm, realm, NULL);
    UASSERT(s != NULL);

    UValue result = {0};
    UASSERT(budget_arm_strand(&vm, &module, s, &result));

    /* Simulate a strand that has exhausted its per-lifetime budget.
     * Pre-fix: ustep.c does not re-arm on dispatch → budget stays 0 →
     *          first backward-branch safepoint yields before the GC check.
     * Post-fix: ustep.c re-arms to URBI_STRAND_BUDGET_MAX on dispatch →
     *           first safepoint reaches the GC check. */
    s->safepoint_budget_remaining = 0U;

    urbi_strand_start(&vm, s);

    /* Reset counters AFTER setup so only the dispatch slice below
     * contributes to the oracle. */
    vm.gc_slices  = 0U;
    vm.gc_pending = 1U;   /* request a GC slice at the next safepoint */

    /* One step: the strand dispatches and hits the backward-branch safepoint
     * in the while-loop header.
     *   Pre-fix : budget=0 → yield before GC check → gc_slices stays 0.
     *   Post-fix: budget re-armed → GC runs → gc_slices > 0. */
    (void)urbi_step(&vm, 1000U, NULL);

    UASSERT(vm.gc_slices > 0);

    uchunk_destroy(&module, NULL);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* Scheduler-liveness wedge (refactor-3 VM-04/SCHED-11).
 *
 * An embedder that drives the VM with a step budget BELOW the per-strand
 * safepoint budget — `while (urbi_step(&vm, SMALL) == URBI_STEP_RUNNING) {}` —
 * over a long tight loop must REACH completion in a bounded number of calls.
 *
 * Pre-fix: when the VM-wide step budget is exhausted mid-loop, the strand
 *          exits dispatch as RUNNING but OFF the ready queue.  sched_pick_next
 *          only returns vm->ready_head, so no later urbi_step re-dispatches it;
 *          urbi_step returns URBI_STEP_RUNNING forever and this loop hangs.
 *          The cap below turns the hang into a clean assertion failure.
 * Post-fix: the budget-exhausted strand is re-enqueued READY, so the next
 *           urbi_step picks it up and the loop completes well within the cap. */
UTEST(embedder_small_budget_loop_completes)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* Long tight loop: 500 backward-branch safepoints, far more than the
     * SMALL per-call budget below, so the VM-wide step budget is exhausted
     * mid-loop on many calls. */
    UProto module;
    UASSERT(budget_compile(&vm,
        "var i = 0; while (i < 500) { i = i + 1 }",
        &module));

    UStrand *s = urbi_strand_create(&vm, realm, NULL);
    UASSERT(s != NULL);

    UValue result = {0};
    UASSERT(budget_arm_strand(&vm, &module, s, &result));

    urbi_strand_start(&vm, s);

    /* SMALL is well below URBI_STRAND_BUDGET_MAX (the per-strand safepoint
     * budget), guaranteeing the VM-wide budget exhausts before the per-strand
     * one — the exact arm the wedge lived on. */
    const uint64_t SMALL = 4U;
    int completed = 0;
    int steps = 0;               /* RUNNING calls before completion */
    int cap = 100000;            /* generous; a correct run needs << this */
    while (cap-- > 0) {
        UStepResult r = urbi_step(&vm, SMALL, NULL);
        if (r == URBI_STEP_QUIESCENT || r == URBI_STEP_WAKE_AT) {
            completed = 1; break;
        }
        if (r == URBI_STEP_FATAL) break;
        steps++;                 /* URBI_STEP_RUNNING: keep driving */
    }
    UASSERT(completed);          /* pre-fix: never set → fails (no hang) */

    /* A 500-iteration loop driven 4 safepoints at a time needs >100 RUNNING
     * steps to finish.  Asserting a floor guards the other direction: a
     * spurious QUIESCENT from the verdict ladder (a READY budget-exhausted
     * strand miscounted as idle) would set `completed` after only a handful
     * of steps — caught here. */
    UASSERT(steps > 50);

    uchunk_destroy(&module, NULL);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* ===========================================================================
 * Suite registration
 * =========================================================================== */

void test_budget_rearm_suite(void)
{
    utest_run("mature_strand_still_runs_gc_slices",
              mature_strand_still_runs_gc_slices);
    utest_run("embedder_small_budget_loop_completes",
              embedder_small_budget_loop_completes);
}
