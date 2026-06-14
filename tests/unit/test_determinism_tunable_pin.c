/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: per-strand safepoint_budget_remaining mutation changes
 * per-step instruction consumption (row 12 §3.2 "tunable pin" determinism
 * test — different budget produces different execution interleaving).
 *
 * URBI_STRAND_BUDGET_MAX is a compile-time constant.  The single-binary
 * approach mutates s->safepoint_budget_remaining at runtime to observe
 * that different budget settings produce different per-step pc progression.
 *
 * Observable: with a tight budget (1), each urbi_step call runs fewer
 * instructions and the program requires more iterations to complete.
 * With a loose budget (URBI_STRAND_BUDGET_MAX), fewer urbi_step calls
 * suffice for the same program.
 *
 * These tests do NOT require URBI_DEBUG: the safepoint_budget_remaining
 * field is part of UStrand (defined in ustrand.h) and is writable. */

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

/* ---- Compile helper ---- */

static bool
tunable_compile(UVM *vm, const char *src, UProto *out_mod)
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

/* Arm a strand from a module. */
static bool
tunable_arm_strand(UVM *vm, UProto *module, UStrand *s, UValue *out_result)
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
    /* v0.10.1 W4: use typed-handle acquire so g_strand_ref_total stays balanced. */
    urbi_proto_strand_ref_acquire(module, URBI_PROTO_REF_OWNER_STRAND);
    s->module_instance = urbi_chunk_instance_create(vm, module);
    s->frame_count = 0;
    s->open_upvals = NULL;
    if (out_result) s->out_slot = out_result;
    return true;
}

/* Run the VM with a fixed step budget until quiescent; return step-call count. */
static int
run_to_quiescent(UVM *vm, uint64_t step_budget)
{
    int count = 0;
    int limit = 1000000;
    while (limit-- > 0) {
        UStepResult r = urbi_step(vm, step_budget, NULL);
        count++;
        if (r == URBI_STEP_QUIESCENT || r == URBI_STEP_WAKE_AT) break;
        if (r == URBI_STEP_FATAL) { count = -1; break; }
    }
    return count;
}

/* ===========================================================================
 * Tests
 * =========================================================================== */

/* Case 1: tight VM-wide step budget (1 opcode) requires more iterations than
 * a loose budget (10000 opcodes) for the same program.
 *
 * This validates that the budget-tuning knob changes execution scheduling.
 * It also guards the scheduler-liveness wedge (refactor-3 VM-04/SCHED-11): a
 * tight budget far below the per-strand budget must still COMPLETE in a bounded
 * number of urbi_step calls.  Pre-fix the budget-exhausted strand exited
 * RUNNING-off-queue and run_to_quiescent capped out at its limit; the
 * `tight_count < limit` assertion below turns that wedge into a failure rather
 * than an accidental pass. */
UTEST(tight_budget_requires_more_step_calls_than_loose)
{
    /* Run with tight budget. */
    int tight_count;
    {
        UVM vm;
        urbi_vm_init(&vm, NULL, NULL);

        URealm *realm = urbi_realm_create(&vm);
        UASSERT(realm != NULL);

        UProto module;
        UASSERT(tunable_compile(&vm, "var i = 0; while (i < 20) { i = i + 1 }", &module));

        UStrand *s = urbi_strand_create(&vm, realm, NULL);
        UASSERT(s != NULL);

        UValue result = {0};
        UASSERT(tunable_arm_strand(&vm, &module, s, &result));
        urbi_strand_start(&vm, s);

        tight_count = run_to_quiescent(&vm, 1);
        UASSERT(tight_count > 0);
        /* Wedge guard: must actually reach QUIESCENT, not cap out at
         * run_to_quiescent's 1000000 iteration limit (refactor-3
         * VM-04/SCHED-11). */
        UASSERT(tight_count < 1000000);

        uchunk_destroy(&module, NULL);
        urbi_realm_destroy(&vm, realm);
        urbi_vm_destroy(&vm);
    }

    /* Run with loose budget. */
    int loose_count;
    {
        UVM vm;
        urbi_vm_init(&vm, NULL, NULL);

        URealm *realm = urbi_realm_create(&vm);
        UASSERT(realm != NULL);

        UProto module;
        UASSERT(tunable_compile(&vm, "var i = 0; while (i < 20) { i = i + 1 }", &module));

        UStrand *s = urbi_strand_create(&vm, realm, NULL);
        UASSERT(s != NULL);

        UValue result = {0};
        UASSERT(tunable_arm_strand(&vm, &module, s, &result));
        urbi_strand_start(&vm, s);

        loose_count = run_to_quiescent(&vm, 10000);
        UASSERT(loose_count > 0);

        uchunk_destroy(&module, NULL);
        urbi_realm_destroy(&vm, realm);
        urbi_vm_destroy(&vm);
    }

    /* Tight budget must require strictly more step calls. */
    UASSERT(tight_count > loose_count);
}

/* Case 2: directly mutating safepoint_budget_remaining to 0 at runtime does
 * NOT block completion — the per-slice re-arm (refactor-3 VM-04/SCHED-11)
 * overwrites the zero at each dispatch start, so the program still reaches
 * QUIESCENT.  (Pre-re-arm this zero soft-yielded at the first safepoint; the
 * test now pins that the zeroing is harmless rather than that it stalls.)
 *
 * We use a trivially short program (1 + 1) and zero the budget immediately
 * after arming the strand, then drive to completion. */
UTEST(zeroed_strand_budget_does_not_block_completion)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UProto module;
    /* Short program: just a single computation. */
    UASSERT(tunable_compile(&vm, "1 + 1", &module));

    UStrand *s = urbi_strand_create(&vm, realm, NULL);
    UASSERT(s != NULL);

    UValue result = {0};
    UASSERT(tunable_arm_strand(&vm, &module, s, &result));

    /* Zero the per-strand budget: forces soft yield at first safepoint. */
    s->safepoint_budget_remaining = 0;

    urbi_strand_start(&vm, s);

    /* With the per-slice re-arm in place, the zeroed budget is overwritten at
     * dispatch start, so the strand completes normally.  Drive to QUIESCENT. */
    int reached = 0;
    int iters = 10000;
    while (iters-- > 0) {
        UStepResult r = urbi_step(&vm, 100000, NULL);
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

void test_determinism_tunable_pin_suite(void)
{
    utest_run("tight_budget_requires_more_step_calls_than_loose",
              tight_budget_requires_more_step_calls_than_loose);
    utest_run("zeroed_strand_budget_does_not_block_completion",
              zeroed_strand_budget_does_not_block_completion);
}
