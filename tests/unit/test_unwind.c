/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: urbi_unwind() walker (T9 + T13).
 *
 * Tests cover:
 *  1. UEXEC_RETURN absorbed at first CALL_FRAME cleanup entry.
 *  2. UEXEC_THROW caught at TRY_FRAME with HAS_CATCH.
 *  3. UEXEC_THROW uncaught (empty cleanup stack) marks strand DEAD.
 *  4. Frame teardown zeros the register range (Inv-5).
 *  5. urbi_unwind is a no-op on UEXEC_OK (double-call safety).
 *  6. UEXEC_RETURN without cleanup entries (backward-compat path).
 *  7. Innermost-first ordering: inner CALL_FRAME absorbs before outer.
 *  8. URBI_CLEANUP_MAX overflow marks strand fatal (T13).
 *  9. suppressed_head stays NULL throughout walk (T13 — Inv pre-v1.x).
 * 10. UEXEC_CANCEL propagates through CALL_FRAME without absorption (T13).
 * 11. UEXEC_THROW propagates past TRY_FRAME with only FINALLY (T13).
 * 12. Nested TRY_FRAMEs: innermost catch absorbs (T13).
 * 13. THROW propagates through TAG_SCOPE (M3 stub passthrough) (T13). */

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

/* Case 8: URBI_CLEANUP_MAX overflow: push a full cleanup stack then attempt
   one more push; strand_cleanup_push returns NULL (stack full).
   Simulate what the VM should do on NULL: set THROW + fatal escalation.
   Verify: fatal_status=THROW, state=DEAD. */
UTEST(unwind_cleanup_max_overflow_marks_fatal)
{
    UVM vm;
    UStrand s;
    uint16_t i;

    uvm_init(&vm, NULL, NULL);
    UValue *reg_stack = strand_setup_minimal(&s, &vm);
    UASSERT(reg_stack != NULL);

    /* Fill the cleanup stack to capacity. */
    for (i = 0; i < s.cleanup_cap; i++) {
        UCleanupEntry *e = strand_cleanup_push(&s);
        UASSERT(e != NULL);
        e->kind           = (uint8_t)UCLEANUP_TRY_FRAME;
        e->flags          = 0;
        e->register_base  = 0;
        e->register_count = 0;
        e->handler_pc     = 0;
        e->owning_tag     = NULL;
        e->catch_pattern  = NULL;
    }

    /* One more push must return NULL — stack is full. */
    UCleanupEntry *overflow = strand_cleanup_push(&s);
    UASSERT(overflow == NULL);

    /* Simulate the VM escalating to fatal on overflow (as the spec requires):
     * set THROW and call urbi_unwind with an exhausted cleanup stack that
     * has no matching catch — all TRY_FRAMEs lack HAS_CATCH, so the walk
     * pops all of them and hits the fatal escalation path. */
    UValue throwval;
    throwval.kind = (uint8_t)UVAL_INT;
    throwval.v.i  = 1;
    s.unwind_value   = throwval;
    s.pending_unwind = UEXEC_THROW;

    urbi_unwind(&s);

    /* All TRY_FRAMEs (no catch, no finally) are popped, throw is unhandled. */
    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_DEAD);
    UASSERT_EQ((int)s.fatal_status, (int)UEXEC_THROW);

    free(reg_stack);
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 9: suppressed_head invariant.
   s->suppressed_head must remain NULL throughout a normal unwind walk
   (reserved for v1.x — no suppressed-unwind chain is built at M3).
   Walk a THROW through a TRY_FRAME (no catch/finally) to fatal; verify
   suppressed_head is still NULL afterward. */
UTEST(unwind_suppressed_head_invariant)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    UValue *reg_stack = strand_setup_minimal(&s, &vm);
    UASSERT(reg_stack != NULL);

    /* Precondition: suppressed_head is NULL (zero-init via calloc). */
    UASSERT(s.suppressed_head == NULL);

    /* Push a TRY_FRAME with neither catch nor finally — pure passthrough. */
    UCleanupEntry *e = strand_cleanup_push(&s);
    UASSERT(e != NULL);
    e->kind           = (uint8_t)UCLEANUP_TRY_FRAME;
    e->flags          = 0;   /* no catch, no finally */
    e->register_base  = 0;
    e->register_count = 0;
    e->handler_pc     = 0;
    e->owning_tag     = NULL;
    e->catch_pattern  = NULL;

    UValue throwval;
    throwval.kind = (uint8_t)UVAL_INT;
    throwval.v.i  = 2;
    s.unwind_value   = throwval;
    s.pending_unwind = UEXEC_THROW;

    urbi_unwind(&s);

    /* Fatal escalation expected (no handler). */
    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_DEAD);
    /* suppressed_head must remain NULL throughout — no chain built at M3. */
    UASSERT(s.suppressed_head == NULL);

    free(reg_stack);
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 10: UEXEC_CANCEL propagates through CALL_FRAME (not absorbed).
   RETURN is absorbed by CALL_FRAME; CANCEL (and THROW, TAG_STOP) must
   propagate past it.  Push a CALL_FRAME, set CANCEL, walk.
   Verify: fatal escalation (no outer handler) — CANCEL not absorbed. */
UTEST(unwind_cancel_propagates_through_call_frame)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    UValue *reg_stack = strand_setup_minimal(&s, &vm);
    UASSERT(reg_stack != NULL);

    /* Minimal call frame. */
    s.frame_count = 1;
    UCallFrame *cf = &s.frames[0];
    cf->closure         = NULL;
    cf->proto           = NULL;
    cf->pc              = s_dummy_instr + 0;
    cf->base            = s.stack;
    cf->result_dest_reg = 0;
    s.R = s.stack + 2;

    /* Push a CALL_FRAME cleanup entry. */
    UCleanupEntry *e = strand_cleanup_push(&s);
    UASSERT(e != NULL);
    e->kind           = (uint8_t)UCLEANUP_CALL_FRAME;
    e->flags          = 0;
    e->register_base  = 2;
    e->register_count = 1;
    e->handler_pc     = 0;
    e->owning_tag     = NULL;
    e->catch_pattern  = NULL;

    /* CANCEL (not RETURN) — must not be absorbed by CALL_FRAME. */
    UValue reason;
    reason.kind = (uint8_t)UVAL_NIL;
    reason.v.i  = 0;
    s.unwind_value   = reason;
    s.pending_unwind = UEXEC_CANCEL;

    urbi_unwind(&s);

    /* CALL_FRAME popped (caller context restored) but CANCEL propagates onward.
     * With no further handler, the strand hits fatal escalation. */
    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_DEAD);
    UASSERT_EQ((int)s.fatal_status, (int)UEXEC_CANCEL);

    free(reg_stack);
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 11: THROW propagates past TRY_FRAME with only FINALLY (no catch).
   A TRY_FRAME with FLAG_HAS_FINALLY but not FLAG_HAS_CATCH should run the
   finally body and then continue propagating the THROW.
   We exercise this via a pure-finally TRY_FRAME with handler_pc=0 (dummy
   instr is OP_RET — dispatch_loop_until_yield sees no THROW from the body
   and returns; run_cleanup_with_replace restores the original THROW).
   After the finally frame: no further handler → fatal escalation. */
UTEST(unwind_throw_propagates_past_try_with_only_finally)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    UValue *reg_stack = strand_setup_minimal(&s, &vm);
    UASSERT(reg_stack != NULL);

    /* Push TRY_FRAME with FINALLY only (no CATCH). */
    UCleanupEntry *e = strand_cleanup_push(&s);
    UASSERT(e != NULL);
    e->kind           = (uint8_t)UCLEANUP_TRY_FRAME;
    e->flags          = FLAG_HAS_FINALLY;  /* only finally, no catch */
    e->register_base  = 0;
    e->register_count = 0;
    e->handler_pc     = 0;    /* dummy OP_RET at instr[0]: finally body is trivial */
    e->owning_tag     = NULL;
    e->catch_pattern  = NULL;

    /* Running state required so dispatch_loop_until_yield can execute. */
    s.state = USTRAND_STATE_RUNNING;

    UValue throwval;
    throwval.kind = (uint8_t)UVAL_INT;
    throwval.v.i  = 3;
    s.unwind_value   = throwval;
    s.pending_unwind = UEXEC_THROW;

    urbi_unwind(&s);

    /* Finally ran (trivially, via OP_RET at instr[0]); THROW resumed.
     * No outer handler → strand DEAD with THROW status. */
    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_DEAD);
    UASSERT_EQ((int)s.fatal_status, (int)UEXEC_THROW);

    free(reg_stack);
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 12: Nested TRY_FRAMEs — innermost catch absorbs.
   Push: outer TRY_FRAME (HAS_CATCH, handler_pc=2) + inner TRY_FRAME
   (HAS_CATCH, handler_pc=1).  Set THROW.  Walk.
   Inner TRY_FRAME absorbs first; outer is never reached. */
UTEST(unwind_nested_try_frames_innermost_catches)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    UValue *reg_stack = strand_setup_minimal(&s, &vm);
    UASSERT(reg_stack != NULL);

    /* Push outer TRY_FRAME first (it will be at depth 0, i.e. outermost). */
    UCleanupEntry *outer = strand_cleanup_push(&s);
    UASSERT(outer != NULL);
    outer->kind           = (uint8_t)UCLEANUP_TRY_FRAME;
    outer->flags          = FLAG_HAS_CATCH;
    outer->register_base  = 0;
    outer->register_count = 0;
    outer->handler_pc     = 2;   /* outer catch at instr[2] */
    outer->owning_tag     = NULL;
    outer->catch_pattern  = NULL;

    /* Push inner TRY_FRAME on top (it's at depth 1, i.e. innermost). */
    UCleanupEntry *inner = strand_cleanup_push(&s);
    UASSERT(inner != NULL);
    inner->kind           = (uint8_t)UCLEANUP_TRY_FRAME;
    inner->flags          = FLAG_HAS_CATCH;
    inner->register_base  = 0;
    inner->register_count = 0;
    inner->handler_pc     = 1;   /* inner catch at instr[1] */
    inner->owning_tag     = NULL;
    inner->catch_pattern  = NULL;

    /* Throw. */
    UValue throwval;
    throwval.kind = (uint8_t)UVAL_INT;
    throwval.v.i  = 5;
    s.unwind_value   = throwval;
    s.pending_unwind = UEXEC_THROW;

    urbi_unwind(&s);

    /* Inner TRY_FRAME absorbs: pc at instr[1], pending_unwind=OK. */
    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_OK);
    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_RUNNING);
    UASSERT(s.pc == s_dummy_module.instructions + 1);
    /* Inner frame popped; outer frame remains (depth=1). */
    UASSERT_EQ((unsigned)s.cleanup_depth, 1u);

    free(reg_stack);
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* Case 13: THROW propagates through TAG_SCOPE (M3 stub passthrough).
   At M3, UCLEANUP_TAG_SCOPE entries are popped without absorption (T29 owns
   absorption).  Push a TAG_SCOPE, set THROW, walk.  The TAG_SCOPE is popped
   and THROW continues to fatal escalation (no outer handler).
   Exercises the UCLEANUP_TAG_SCOPE branch of the walker (uunwind.c lines 270-286). */
UTEST(unwind_throw_propagates_through_tag_scope)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    UValue *reg_stack = strand_setup_minimal(&s, &vm);
    UASSERT(reg_stack != NULL);

    /* Push TAG_SCOPE entry with no onleave (plain passthrough at M3). */
    UCleanupEntry *e = strand_cleanup_push(&s);
    UASSERT(e != NULL);
    e->kind           = (uint8_t)UCLEANUP_TAG_SCOPE;
    e->flags          = 0;   /* no onleave */
    e->register_base  = 0;
    e->register_count = 0;
    e->handler_pc     = 0;
    e->owning_tag     = NULL;
    e->catch_pattern  = NULL;

    UValue throwval;
    throwval.kind = (uint8_t)UVAL_INT;
    throwval.v.i  = 8;
    s.unwind_value   = throwval;
    s.pending_unwind = UEXEC_THROW;

    urbi_unwind(&s);

    /* TAG_SCOPE popped (passthrough); THROW has no outer handler → fatal. */
    UASSERT_EQ((int)s.state, (int)USTRAND_STATE_DEAD);
    UASSERT_EQ((int)s.fatal_status, (int)UEXEC_THROW);
    UASSERT_EQ((unsigned)s.cleanup_depth, 0u);

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
    utest_run("unwind: URBI_CLEANUP_MAX overflow escalates to fatal",
              unwind_cleanup_max_overflow_marks_fatal);
    utest_run("unwind: suppressed_head stays NULL throughout walk (Inv pre-v1.x)",
              unwind_suppressed_head_invariant);
    utest_run("unwind: CANCEL propagates through CALL_FRAME (not absorbed)",
              unwind_cancel_propagates_through_call_frame);
    utest_run("unwind: THROW propagates past TRY_FRAME with only FINALLY",
              unwind_throw_propagates_past_try_with_only_finally);
    utest_run("unwind: nested TRY_FRAMEs — innermost catch absorbs",
              unwind_nested_try_frames_innermost_catches);
    utest_run("unwind: THROW propagates through TAG_SCOPE (M3 stub passthrough)",
              unwind_throw_propagates_through_tag_scope);
}
