/* SPDX-License-Identifier: BSD-3-Clause */
/* Tests for dispatch_loop_until_yield (T6).
   Verifies that the dispatch loop:
   1. Yields and returns READY when OP_YIELD is dispatched.
   2. Exits and returns DEAD when OP_RET at top frame is dispatched.
   These tests exercise dispatch_loop_until_yield directly, bypassing
   the uvm_run adapter, to validate the M3-scheduler-facing contract. */

#include "utest.h"
#include "uvm.h"
#include "ustrand.h"
#include "umodule.h"
#include "usched_cooperative.h"

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* --- Bytecode construction helpers --- */

/* Encode OP_YIELD (no operands). */
static uint32_t enc_yield(void) {
    return uinstr_enc_abc(OP_YIELD, 0, 0, 0);
}

/* Encode OP_RET R[0]. */
static uint32_t enc_ret(void) {
    return uinstr_enc_abc(OP_RET, 0, 0, 0);
}

/* Encode OP_LOADK R[0] = K[0] (a filler instruction). */
static uint32_t enc_loadk(uint8_t dst, uint8_t kidx) {
    return uinstr_enc_abx(OP_LOADK, dst, (uint16_t)kidx);
}

/* --- Helper: init a strand for a synthetic module ---
   module is assumed to point at a non-empty instruction array.
   Strand fields are set up exactly as uvm_run does for a transient strand. */
static int strand_setup(UStrand *s, UVM *vm,
                        const uint32_t *instructions,
                        const UValue   *constants,
                        UValue         *reg_stack)
{
    volatile unsigned char *p = (volatile unsigned char *)s;
    size_t n = sizeof(*s);
    size_t i;
    for (i = 0; i < n; i++) p[i] = 0;

    s->vm         = vm;
    s->state      = USTRAND_STATE_RUNNING;
    s->stack      = reg_stack;
    s->R          = reg_stack;
    s->pc         = instructions;
    s->pc_base    = instructions;
    s->cur_consts = constants;
    /* module pointer: cast away const since the strand field is non-const
       but the module itself is read-only; safe because dispatch only reads it. */
    s->module     = NULL;  /* NULL ok for tests that never access s->module fields */
    s->frame_count  = 0;
    s->open_upvals  = NULL;
    s->closure_list = NULL;
    s->closed_cells = NULL;
    s->out_slot     = NULL;
    return 0;
}

/* --- Test 1: OP_YIELD causes dispatch_loop_until_yield to return READY --- */

UTEST(dispatch_loop_yields_on_op_yield) {
    /* Module: OP_YIELD then OP_RET.
       dispatch_loop should return after OP_YIELD with state=READY. */
    static uint32_t instrs[] = {
        /* [0] */ 0, /* OP_YIELD = 23 */ 0, 0, 0   /* placeholder — filled below */
    };
    instrs[0] = enc_yield();
    instrs[1] = enc_ret();

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    /* Allocate a minimal register stack. */
    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    static UValue no_consts[1];  /* empty constant pool */

    UStrand s;
    strand_setup(&s, &vm, instrs, no_consts, reg_stack);

    uint64_t consumed = dispatch_loop_until_yield(&s, /*step_budget*/ 10000);

    /* After OP_YIELD: strand should be READY (on the ready queue). */
    UASSERT_EQ((int)USTRAND_STATE_READY, (int)s.state);
    /* At least 1 opcode consumed. */
    UASSERT(consumed >= 1);

    /* Drain the ready queue so sched state is clean. */
    if (vm.ready_head == &s) {
        vm.ready_head = s.ready_next;
        if (vm.ready_head == NULL) vm.ready_tail = NULL;
        if (vm.strand_runnable_count > 0) vm.strand_runnable_count--;
    }

    free(reg_stack);
    uvm_destroy(&vm);
}

/* --- Test 2: OP_RET at top frame causes strand to reach DEAD --- */

UTEST(dispatch_loop_dies_on_top_level_ret) {
    /* Module: OP_RET R[0] (returns Nil).
       dispatch_loop should return with state=DEAD after one instruction. */
    static uint32_t instrs[1];
    instrs[0] = enc_ret();

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    static UValue no_consts[1];

    UStrand s;
    strand_setup(&s, &vm, instrs, no_consts, reg_stack);

    /* Set out_slot so OP_RET can write the return value. */
    UValue retval = {0};
    s.out_slot = &retval;

    uint64_t consumed = dispatch_loop_until_yield(&s, /*step_budget*/ 10000);

    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT(consumed >= 1);

    free(reg_stack);
    uvm_destroy(&vm);
}

/* --- Suite registration --- */

void test_dispatch_loop_suite(void) {
    utest_run("dispatch_loop yields on OP_YIELD",    dispatch_loop_yields_on_op_yield);
    utest_run("dispatch_loop DEAD on top-level RET", dispatch_loop_dies_on_top_level_ret);
}
