/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: urbi_unwind() walker (T9).
 *
 * Tests cover:
 *  1. UEXEC_RETURN absorbed at first CALL_FRAME cleanup entry.
 *  2. UEXEC_THROW caught at TRY_FRAME with HAS_CATCH.
 *  3. UEXEC_THROW uncaught (empty cleanup stack) marks strand DEAD.
 *  4. Frame teardown zeros the register range (Inv-5).
 *  5. urbi_unwind is a no-op on UEXEC_OK (double-call safety).
 *  6. UEXEC_RETURN without cleanup entries (backward-compat path).
 *  7. Innermost-first ordering: inner CALL_FRAME absorbs before outer.
 *
 * T13 adds the full .chk fixture suite including yield-inside-finally,
 * nested try/catch, and integration tests. */

#include "utest.h"
#include "uunwind.h"
#include "ustrand.h"
#include "ucleanup.h"
#include "uvm.h"
#include "umodule.h"

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* --- Helpers --- */

/* Minimal instruction array: OP_RET R[0] used as a placeholder when
   run_cleanup_with_replace is not exercised in a test. */
static uint32_t s_dummy_instr[4];

/* A real UModule pointing at dummy_instr so s->module->instructions is valid.
   Shared across tests; filled in by setup_module(). */
static UModule s_dummy_module;

static void
setup_module(void)
{
    /* OP_RET R[0] at index 0. */
    s_dummy_instr[0] = uinstr_enc_abc(OP_RET, 0, 0, 0);
    s_dummy_instr[1] = uinstr_enc_abc(OP_RET, 0, 0, 0);
    s_dummy_instr[2] = uinstr_enc_abc(OP_RET, 0, 0, 0);
    s_dummy_instr[3] = uinstr_enc_abc(OP_RET, 0, 0, 0);

    memset(&s_dummy_module, 0, sizeof(s_dummy_module));
    s_dummy_module.instructions = s_dummy_instr;
}

/* Zero-init a UStrand for testing.  Wires vm, module, stack and cleanup-stack.
   Returns a heap-allocated register stack that the caller must free. */
static UValue *
strand_setup_minimal(UStrand *s, UVM *vm)
{
    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    if (!reg_stack) return NULL;

    memset(s, 0, sizeof(*s));

    s->vm         = vm;
    s->state      = USTRAND_STATE_RUNNING;
    s->stack      = reg_stack;
    s->R          = reg_stack;
    s->pc         = s_dummy_module.instructions;
    s->pc_base    = s_dummy_module.instructions;
    s->cur_consts = NULL;
    s->module     = &s_dummy_module;
    s->frame_count   = 0;
    s->open_upvals   = NULL;
    s->closure_list  = NULL;
    s->closed_cells  = NULL;
    s->out_slot      = NULL;
    s->pending_unwind = UEXEC_OK;

    /* Initialise cleanup stack via the strand allocator. */
    strand_cleanup_stack_init(s, vm, (uint16_t)URBI_CLEANUP_MAX);

    return reg_stack;
}

/* ===== Test cases ===== */

/* Case 1: UEXEC_RETURN absorbed at first CALL_FRAME cleanup entry.
   Push a CALL_FRAME entry.  Set RETURN+value.  Walk.
   Verify: pending_unwind=OK, frame popped, value delivered. */
UTEST(unwind_return_at_call_frame_absorbs)
{
    UVM vm;
    UStrand s;
    UCallFrame *cf;

    uvm_init(&vm, NULL, NULL);
    UValue *reg_stack = strand_setup_minimal(&s, &vm);
    UASSERT(reg_stack != NULL);

    /* Set up a minimal call frame in s->frames[].
     * Simulates: caller called callee at R[2]; callee is about to return.
     * Caller's register base = s->stack (R[0..]).
     * result_dest_reg = 2 (result goes to caller's R[2]).
     * Saved pc = s_dummy_instr + 0 (the OP_CALL instruction in the caller). */
    s.frame_count = 1;
    cf = &s.frames[0];
    cf->closure         = NULL;
    cf->proto           = NULL;
    cf->pc              = s_dummy_instr + 0;   /* AT OP_CALL */
    cf->base            = s.stack;              /* caller's register base */
    cf->result_dest_reg = 2;

    /* Move callee's R to simulate being inside the callee. */
    s.R = s.stack + 3;   /* callee's register window starts at stack[3] */

    /* Push a CALL_FRAME cleanup entry. */
    UCleanupEntry *e = strand_cleanup_push(&s);
    UASSERT(e != NULL);
    e->kind           = (uint8_t)UCLEANUP_CALL_FRAME;
    e->flags          = 0;
    e->register_base  = 3;    /* callee's registers start at stack[3] */
    e->register_count = 2;    /* 2 callee registers to zero on teardown */
    e->handler_pc     = 0;
    e->owning_tag     = NULL;
    e->catch_pattern  = NULL;

    /* Set return value. */
    UValue retval;
    retval.kind = (uint8_t)UVAL_INT;
    retval.v.i  = 42;
    s.unwind_value   = retval;
    s.pending_unwind = UEXEC_RETURN;

    /* Walk. */
    urbi_unwind(&s);

    /* Verify: absorbed. */
    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_OK);
    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_RUNNING);
    /* Return value delivered to caller's R[2]. */
    UASSERT_EQ((int)s.stack[2].v.i, 42);
    UASSERT_EQ((int)s.stack[2].kind, (int)UVAL_INT);
    /* Cleanup stack should be empty after absorption. */
    UASSERT_EQ((unsigned)s.cleanup_depth, 0u);
    /* frame_count decremented. */
    UASSERT_EQ((int)s.frame_count, 0);

    free(reg_stack);
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 2: UEXEC_THROW caught at TRY_FRAME with HAS_CATCH.
   Push TRY_FRAME with HAS_CATCH and handler_pc=1.  Set THROW+value.  Walk.
   Verify: pending_unwind=OK, s->pc advanced to handler. */
UTEST(unwind_throw_caught_at_try_frame)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    UValue *reg_stack = strand_setup_minimal(&s, &vm);
    UASSERT(reg_stack != NULL);

    /* Push TRY_FRAME with HAS_CATCH, no FINALLY. */
    UCleanupEntry *e = strand_cleanup_push(&s);
    UASSERT(e != NULL);
    e->kind           = (uint8_t)UCLEANUP_TRY_FRAME;
    e->flags          = FLAG_HAS_CATCH;       /* catch present, no finally */
    e->register_base  = 0;
    e->register_count = 0;
    e->handler_pc     = 1;                    /* catch handler at instr[1] */
    e->owning_tag     = NULL;
    e->catch_pattern  = NULL;                 /* M3 stub: pattern_matches returns true */

    /* Set throw value. */
    UValue throwval;
    throwval.kind = (uint8_t)UVAL_INT;
    throwval.v.i  = 99;
    s.unwind_value   = throwval;
    s.pending_unwind = UEXEC_THROW;

    /* Walk. */
    urbi_unwind(&s);

    /* Verify: absorbed. */
    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_OK);
    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_RUNNING);
    /* pc should point at handler_pc (instr[1]). */
    UASSERT(s.pc == s_dummy_module.instructions + 1);
    /* Cleanup stack empty. */
    UASSERT_EQ((unsigned)s.cleanup_depth, 0u);

    free(reg_stack);
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 3: UEXEC_THROW uncaught (empty cleanup stack) marks strand DEAD.
   Set THROW with no cleanup entries.  Walk.
   Verify: fatal_status=THROW, state=DEAD. */
UTEST(unwind_throw_uncaught_marks_fatal)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    UValue *reg_stack = strand_setup_minimal(&s, &vm);
    UASSERT(reg_stack != NULL);

    /* cleanup_depth = 0: no entries. */

    UValue throwval;
    throwval.kind = (uint8_t)UVAL_INT;
    throwval.v.i  = 7;
    s.unwind_value   = throwval;
    s.pending_unwind = UEXEC_THROW;

    urbi_unwind(&s);

    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_DEAD);
    UASSERT_EQ((int)s.fatal_status, (int)UEXEC_THROW);
    UASSERT_EQ((int)s.fatal_value.v.i, 7);

    free(reg_stack);
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 4: Frame teardown zeros registers (Inv-5).
   Push CALL_FRAME with register_base=0, register_count=4.
   Put sentinel values in stack[0..3].  Walk with RETURN.
   Verify: stack[0..3] are all zero (UVAL_NIL) after absorption. */
UTEST(unwind_frame_teardown_zeros_registers)
{
    UVM vm;
    UStrand s;
    int i;

    uvm_init(&vm, NULL, NULL);
    UValue *reg_stack = strand_setup_minimal(&s, &vm);
    UASSERT(reg_stack != NULL);

    /* Set up call frame: caller at stack[0], callee at stack[0] (same base
     * for simplicity — we just want to test zeroing). */
    s.frame_count = 1;
    UCallFrame *cf = &s.frames[0];
    cf->closure         = NULL;
    cf->proto           = NULL;
    cf->pc              = s_dummy_instr + 0;
    cf->base            = s.stack;
    cf->result_dest_reg = 5;   /* result goes to caller's R[5] */
    s.R = s.stack;             /* callee base same as caller base for this test */

    /* Push CALL_FRAME entry for registers 0..3 (4 slots). */
    UCleanupEntry *e = strand_cleanup_push(&s);
    UASSERT(e != NULL);
    e->kind           = (uint8_t)UCLEANUP_CALL_FRAME;
    e->flags          = 0;
    e->register_base  = 0;
    e->register_count = 4;
    e->handler_pc     = 0;
    e->owning_tag     = NULL;
    e->catch_pattern  = NULL;

    /* Place sentinel values in stack[0..3]. */
    for (i = 0; i < 4; i++) {
        s.stack[i].kind = (uint8_t)UVAL_INT;
        s.stack[i].v.i  = 0xDEAD + i;
    }

    /* RETURN with a value. */
    UValue retval;
    retval.kind = (uint8_t)UVAL_INT;
    retval.v.i  = 123;
    s.unwind_value   = retval;
    s.pending_unwind = UEXEC_RETURN;

    urbi_unwind(&s);

    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_OK);

    /* stack[0..3] must be zeroed by zero_registers() before the frame was
     * processed.  Return value goes to caller's R[5] = stack[5]. */
    for (i = 0; i < 4; i++) {
        UASSERT_EQ((int)s.stack[i].kind, 0);
        UASSERT_EQ((int)s.stack[i].v.i,  0);
    }

    free(reg_stack);
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 5: urbi_unwind is a no-op when pending_unwind == UEXEC_OK.
   Call urbi_unwind twice; second call must be a no-op. */
UTEST(unwind_noop_on_ok_state)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    UValue *reg_stack = strand_setup_minimal(&s, &vm);
    UASSERT(reg_stack != NULL);

    /* First call: RETURN with a CALL_FRAME. */
    s.frame_count = 1;
    UCallFrame *cf = &s.frames[0];
    cf->closure         = NULL;
    cf->proto           = NULL;
    cf->pc              = s_dummy_instr + 0;
    cf->base            = s.stack;
    cf->result_dest_reg = 0;
    s.R = s.stack;

    UCleanupEntry *e = strand_cleanup_push(&s);
    UASSERT(e != NULL);
    e->kind           = (uint8_t)UCLEANUP_CALL_FRAME;
    e->flags          = 0;
    e->register_base  = 0;
    e->register_count = 0;
    e->handler_pc     = 0;
    e->owning_tag     = NULL;
    e->catch_pattern  = NULL;

    UValue retval;
    retval.kind = (uint8_t)UVAL_INT;
    retval.v.i  = 1;
    s.unwind_value   = retval;
    s.pending_unwind = UEXEC_RETURN;
    urbi_unwind(&s);

    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_OK);

    /* Second call: state is UEXEC_OK; must be a no-op. */
    uint8_t old_state = s.state;
    int old_depth = (int)s.cleanup_depth;
    urbi_unwind(&s);

    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_OK);
    UASSERT_EQ((int)s.state, (int)old_state);
    UASSERT_EQ((int)s.cleanup_depth, old_depth);

    free(reg_stack);
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 6: UEXEC_RETURN without cleanup entries (backward-compat direct-pop path).
   This mirrors T8's bridging stub: cleanup_depth=0, frame_count=1.
   Verifies all existing M2/M3 function-return tests still pass under T9. */
UTEST(unwind_return_direct_pop_no_cleanup_entry)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    UValue *reg_stack = strand_setup_minimal(&s, &vm);
    UASSERT(reg_stack != NULL);

    /* Set up call frame: callee returns to caller's R[3]. */
    s.frame_count = 1;
    UCallFrame *cf = &s.frames[0];
    cf->closure         = NULL;
    cf->proto           = NULL;
    cf->pc              = s_dummy_instr + 0;
    cf->base            = s.stack;
    cf->result_dest_reg = 3;
    s.R = s.stack + 4;   /* callee's register window */

    /* No cleanup entries pushed (cleanup_depth remains 0). */
    UASSERT_EQ((unsigned)s.cleanup_depth, 0u);

    UValue retval;
    retval.kind = (uint8_t)UVAL_INT;
    retval.v.i  = 77;
    s.unwind_value   = retval;
    s.pending_unwind = UEXEC_RETURN;

    urbi_unwind(&s);

    /* Absorbed via direct-pop path. */
    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_OK);
    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_RUNNING);
    UASSERT_EQ((int)s.frame_count, 0);
    /* Return value in caller's R[3]. */
    UASSERT_EQ((int)s.stack[3].v.i, 77);
    UASSERT_EQ((int)s.stack[3].kind, (int)UVAL_INT);

    free(reg_stack);
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 7: Innermost-first ordering.
   Push: CALL_FRAME (outer) + TRY_FRAME (middle) + CALL_FRAME (inner).
   Set RETURN.  Walk.  Inner CALL_FRAME absorbs.
   TRY_FRAME and outer CALL_FRAME are NOT reached. */
UTEST(unwind_innermost_first_ordering)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    UValue *reg_stack = strand_setup_minimal(&s, &vm);
    UASSERT(reg_stack != NULL);

    /* Two call frames: outer (frame 0) and inner (frame 1). */
    s.frame_count = 2;

    /* Outer call frame: caller at stack[0]; result goes to R[1]. */
    s.frames[0].closure         = NULL;
    s.frames[0].proto           = NULL;
    s.frames[0].pc              = s_dummy_instr + 0;
    s.frames[0].base            = s.stack;
    s.frames[0].result_dest_reg = 1;

    /* Inner call frame: caller at stack[2]; result goes to R[0] (of outer). */
    s.frames[1].closure         = NULL;
    s.frames[1].proto           = NULL;
    s.frames[1].pc              = s_dummy_instr + 0;
    s.frames[1].base            = s.stack + 2;
    s.frames[1].result_dest_reg = 0;

    s.R = s.stack + 4;   /* innermost callee's register window */

    /* Push entries: outer CALL_FRAME, TRY_FRAME (no catch/finally), inner CALL_FRAME. */
    UCleanupEntry *e1 = strand_cleanup_push(&s);  /* outer CALL_FRAME */
    UASSERT(e1 != NULL);
    e1->kind           = (uint8_t)UCLEANUP_CALL_FRAME;
    e1->flags          = 0;
    e1->register_base  = 0;
    e1->register_count = 0;
    e1->handler_pc     = 0;
    e1->owning_tag     = NULL;
    e1->catch_pattern  = NULL;

    UCleanupEntry *e2 = strand_cleanup_push(&s);  /* TRY_FRAME, no catch/finally */
    UASSERT(e2 != NULL);
    e2->kind           = (uint8_t)UCLEANUP_TRY_FRAME;
    e2->flags          = 0;   /* neither catch nor finally — just pop on RETURN */
    e2->register_base  = 0;
    e2->register_count = 0;
    e2->handler_pc     = 0;
    e2->owning_tag     = NULL;
    e2->catch_pattern  = NULL;

    UCleanupEntry *e3 = strand_cleanup_push(&s);  /* inner CALL_FRAME */
    UASSERT(e3 != NULL);
    e3->kind           = (uint8_t)UCLEANUP_CALL_FRAME;
    e3->flags          = 0;
    e3->register_base  = 0;
    e3->register_count = 0;
    e3->handler_pc     = 0;
    e3->owning_tag     = NULL;
    e3->catch_pattern  = NULL;

    /* RETURN from innermost callee. */
    UValue retval;
    retval.kind = (uint8_t)UVAL_INT;
    retval.v.i  = 55;
    s.unwind_value   = retval;
    s.pending_unwind = UEXEC_RETURN;

    urbi_unwind(&s);

    /* Inner CALL_FRAME absorbs.  Result goes into inner caller's R[0] = stack[2]. */
    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_OK);
    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_RUNNING);
    /* Inner call frame popped: frame_count becomes 1. */
    UASSERT_EQ((int)s.frame_count, 1);
    /* Return value at inner caller's result register. */
    UASSERT_EQ((int)s.stack[2].v.i, 55);
    /* TRY_FRAME and outer CALL_FRAME are still on the cleanup stack (depth=2). */
    UASSERT_EQ((unsigned)s.cleanup_depth, 2u);

    free(reg_stack);
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ===== Suite registration ===== */

void test_unwind_suite(void) {
    setup_module();
    utest_run("unwind: RETURN absorbed at CALL_FRAME",
              unwind_return_at_call_frame_absorbs);
    utest_run("unwind: THROW caught at TRY_FRAME with HAS_CATCH",
              unwind_throw_caught_at_try_frame);
    utest_run("unwind: THROW uncaught marks strand DEAD",
              unwind_throw_uncaught_marks_fatal);
    utest_run("unwind: frame teardown zeros registers (Inv-5)",
              unwind_frame_teardown_zeros_registers);
    utest_run("unwind: no-op on UEXEC_OK (double-call safety)",
              unwind_noop_on_ok_state);
    utest_run("unwind: RETURN backward-compat direct-pop (no cleanup entry)",
              unwind_return_direct_pop_no_cleanup_entry);
    utest_run("unwind: innermost-first ordering (inner absorbs, outer untouched)",
              unwind_innermost_first_ordering);
}
