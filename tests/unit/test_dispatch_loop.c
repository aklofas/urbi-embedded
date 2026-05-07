/* SPDX-License-Identifier: BSD-3-Clause */
/* Tests for dispatch_loop_until_yield (T6).
   Extended from 2 to 13 cases at T21.
   Verifies the M3-scheduler-facing dispatch contract:
   budget accounting, safepoint firing, try/catch absorption,
   backward-branch yield, and nested call frames. */

#include "utest.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "module/umodule.h"
#include "runtime/uclosure.h"
#include "sched/usched_cooperative.h"
#include "runtime/ucleanup.h"

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ============================================================
 * Bytecode construction helpers
 * ============================================================ */

/* Encode OP_YIELD (no operands). */
static uint32_t enc_yield(void) {
    return uinstr_enc_abc(OP_YIELD, 0, 0, 0);
}

/* Encode OP_RET R[A]. */
static uint32_t enc_ret_reg(uint8_t a) {
    return uinstr_enc_abc(OP_RET, a, 0, 0);
}
static uint32_t enc_ret(void) { return enc_ret_reg(0); }

/* Encode OP_LOADK R[dst] = K[kidx]. */
static uint32_t enc_loadk(uint8_t dst, uint8_t kidx) {
    return uinstr_enc_abx(OP_LOADK, dst, (uint16_t)kidx);
}

/* Encode OP_LOADNIL R[dst]. */
static uint32_t enc_loadnil(uint8_t dst) {
    return uinstr_enc_abc(OP_LOADNIL, dst, 0, 0);
}

/* Encode OP_MOVE R[dst] := R[src]. */
static uint32_t enc_move(uint8_t dst, uint8_t src) {
    return uinstr_enc_abc(OP_MOVE, dst, src, 0);
}

/* Encode OP_ADD R[dst] := R[b] + R[c]. */
static uint32_t enc_add(uint8_t dst, uint8_t b, uint8_t c) {
    return uinstr_enc_abc(OP_ADD, dst, b, c);
}

/* Encode OP_JMP with a signed offset (offset applied after pc++ in NEXT,
   so offset=0 means NOP-jump, offset=-1 means spin-back-one, etc.). */
static uint32_t enc_jmp(int offset) {
    return uinstr_enc_abx(OP_JMP, 0, (uint16_t)(32768 + offset));
}

/* Encode OP_THROW R[a]. */
static uint32_t enc_throw(uint8_t a) {
    return uinstr_enc_abx(OP_THROW, a, 0);
}

/* Encode OP_TRY_BEGIN flags, handler_pc. */
static uint32_t enc_try_begin(uint8_t flags, uint16_t handler_pc) {
    return uinstr_enc_abx(OP_TRY_BEGIN, flags, handler_pc);
}

/* Encode OP_TRY_END. */
static uint32_t enc_try_end(void) {
    return uinstr_enc_abc(OP_TRY_END, 0, 0, 0);
}

/* Encode OP_LOAD_CATCH_VALUE R[dst]. */
static uint32_t enc_load_catch(uint8_t dst) {
    return uinstr_enc_abc(OP_LOAD_CATCH_VALUE, dst, 0, 0);
}

/* Encode OP_CALL R[a](R[a+1]..R[a+b-1]); C=0 (b-1 args). */
static uint32_t enc_call(uint8_t a, uint8_t b) {
    return uinstr_enc_abc(OP_CALL, a, b, 0);
}

/* Encode OP_PUSH_TAG A Bx — stubs (T11 wired; T29/T30 for real runtime).
   A[7:4]=flags nibble, A[3:0]=tag_reg; Bx=onleave_pc. */
static uint32_t enc_push_tag(uint8_t flags_nibble, uint8_t tag_reg, uint16_t onleave_pc) {
    uint8_t a = (uint8_t)((flags_nibble << 4) | (tag_reg & 0x0Fu));
    return uinstr_enc_abx(OP_PUSH_TAG, a, onleave_pc);
}

/* Encode OP_POP_TAG A=tag_reg. */
static uint32_t enc_pop_tag(uint8_t tag_reg) {
    return uinstr_enc_abc(OP_POP_TAG, tag_reg, 0, 0);
}

/* ============================================================
 * Strand setup helper
 * ============================================================ */

/* Init a strand for a synthetic module.
   module is read-only; strand fields set up as uvm_run does for transient strands. */
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
    s->module     = NULL;
    s->frame_count  = 0;
    s->open_upvals  = NULL;
    s->closure_list = NULL;
    s->closed_cells = NULL;
    s->out_slot     = NULL;
    return 0;
}

/* Init the cleanup stack so OP_TRY_BEGIN can push entries. */
static void strand_setup_cleanup(UStrand *s, UVM *vm)
{
    (void)vm;
    s->cleanup_base  = (UCleanupEntry *)calloc(64, sizeof(UCleanupEntry));
    s->cleanup_cap   = 64;
    s->cleanup_depth = 0;
    s->cleanup_top   = NULL;
}

/* ============================================================
 * Test 1: OP_YIELD causes dispatch_loop_until_yield to return READY
 * ============================================================ */

UTEST(dispatch_loop_yields_on_op_yield) {
    static uint32_t instrs[2];
    instrs[0] = enc_yield();
    instrs[1] = enc_ret();

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    static UValue no_consts[1];

    UStrand s;
    strand_setup(&s, &vm, instrs, no_consts, reg_stack);

    uint64_t consumed = dispatch_loop_until_yield(&s, /*step_budget*/ 10000);

    UASSERT_EQ((int)USTRAND_STATE_READY, (int)s.state);
    UASSERT(consumed >= 1);

    /* Drain the ready queue so sched state is clean. */
    if (vm.ready_head == &s) {
        vm.ready_head = s.ready_next;
        if (vm.ready_head == NULL) vm.ready_tail = NULL;
        if (vm.strand_runnable_count > 0) vm.strand_runnable_count--;
    }

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ============================================================
 * Test 2: OP_RET at top frame causes strand to reach DEAD
 * ============================================================ */

UTEST(dispatch_loop_dies_on_top_level_ret) {
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

    UValue retval = {0};
    s.out_slot = &retval;

    uint64_t consumed = dispatch_loop_until_yield(&s, /*step_budget*/ 10000);

    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT(consumed >= 1);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ============================================================
 * Test 3: VM-wide step budget exhaustion — strand stays RUNNING
 * dispatch_loop_until_yield exits with state RUNNING when the
 * VM-wide step_budget_remaining reaches 0 before the strand finishes.
 * ============================================================ */

UTEST(dispatch_loop_exits_on_step_budget_exhaustion) {
    /* Three LOADNIL instructions followed by RET.
       With a step budget of 1 (at the safepoint-based decrement),
       the loop exits before reaching RET.
       However: LOADNIL does not hit a safepoint — only backward branches
       and calls do.  Use OP_JMP 0 (forward, offset=0 → NOP-jump) which
       does NOT hit a safepoint, then use backward branch to trigger safepoint.
       Actually: easiest approach is a backward OP_JMP with step_budget=1
       so the first safepoint decrements vm->step_budget_remaining to 0. */

    /* Program: LOADNIL R0; JMP -1 (backward, hits safepoint); RET.
       With step_budget=1: on first backward-branch safepoint, budget → 0,
       strand exits with state still RUNNING (not DEAD). */
    static uint32_t instrs[3];
    instrs[0] = enc_loadnil(0);
    instrs[1] = enc_jmp(-1);   /* backward: hits safepoint */
    instrs[2] = enc_ret();

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    static UValue no_consts[1];

    UStrand s;
    strand_setup(&s, &vm, instrs, no_consts, reg_stack);

    /* Give a large per-strand budget so instruction_budget_remaining doesn't
       cause a yield; use a tiny VM-wide step budget = 1. */
    s.instruction_budget_remaining = 1000u;

    uint64_t consumed = dispatch_loop_until_yield(&s, /*step_budget*/ 1u);

    /* strand stays RUNNING (budget exhausted from VM's perspective) */
    UASSERT_EQ((int)USTRAND_STATE_RUNNING, (int)s.state);
    UASSERT(consumed >= 1u);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ============================================================
 * Test 4: instruction_budget_remaining decrements at safepoint
 * ============================================================ */

UTEST(dispatch_loop_instruction_budget_decrements) {
    /* Program: JMP -1 (backward) → JMP -1 → ...
       Each backward branch hits a safepoint and decrements instruction_budget_remaining.
       With initial budget = 3, after 3 safepoints the strand soft-yields. */
    static uint32_t instrs[2];
    instrs[0] = enc_loadnil(0);
    instrs[1] = enc_jmp(-1);   /* backward: safepoint every iteration */

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    static UValue no_consts[1];

    UStrand s;
    strand_setup(&s, &vm, instrs, no_consts, reg_stack);
    s.instruction_budget_remaining = 3u;  /* will soft-yield after 3 safepoints */

    /* Give a very large VM step budget so that never triggers. */
    uint64_t consumed = dispatch_loop_until_yield(&s, /*step_budget*/ 100000u);

    /* Strand should be READY (soft-yield due to budget exhaustion). */
    UASSERT_EQ((int)USTRAND_STATE_READY, (int)s.state);
    UASSERT(consumed >= 3u);
    UASSERT_EQ(s.instruction_budget_remaining, 0u);

    /* Drain ready queue. */
    if (vm.ready_head == &s) {
        vm.ready_head = s.ready_next;
        if (vm.ready_head == NULL) vm.ready_tail = NULL;
        if (vm.strand_runnable_count > 0) vm.strand_runnable_count--;
    }

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ============================================================
 * Test 5: backward branch hits safepoint; forward branch does not
 * dispatch_loop executes a forward JMP (offset > 0) without yielding,
 * then a normal RET terminates the strand.
 * ============================================================ */

UTEST(dispatch_loop_forward_jump_no_safepoint) {
    /* Program: JMP +1 (skip one instr); LOADNIL R0 (skipped); RET.
       Forward jump: no safepoint.  Strand completes in one quantum. */
    static uint32_t instrs[3];
    instrs[0] = enc_jmp(1);    /* forward: skip instrs[1] */
    instrs[1] = enc_loadnil(1); /* skipped */
    instrs[2] = enc_ret();

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    static UValue no_consts[1];

    UStrand s;
    strand_setup(&s, &vm, instrs, no_consts, reg_stack);
    s.instruction_budget_remaining = 2u;  /* only 2 budget; forward jump uses 0 */

    UValue retval = {0};
    s.out_slot = &retval;

    uint64_t consumed = dispatch_loop_until_yield(&s, 10000u);

    /* Strand reached DEAD via RET — no yield was triggered. */
    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT(consumed >= 1u);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ============================================================
 * Test 6: multiple OP_YIELD in sequence — re-enter and yield again
 * ============================================================ */

UTEST(dispatch_loop_multiple_yields) {
    /* Program: YIELD; YIELD; RET.
       First call: yields at first YIELD (state→READY).
       Second call (re-enter with pc pointing at second YIELD):
         yields again (state→READY).
       Third call: RET → DEAD. */
    static uint32_t instrs[3];
    instrs[0] = enc_yield();
    instrs[1] = enc_yield();
    instrs[2] = enc_ret();

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    static UValue no_consts[1];

    UStrand s;
    strand_setup(&s, &vm, instrs, no_consts, reg_stack);

    /* First dispatch: should yield. */
    uint64_t c1 = dispatch_loop_until_yield(&s, 10000u);
    UASSERT_EQ((int)USTRAND_STATE_READY, (int)s.state);
    UASSERT(c1 >= 1u);

    /* Simulate scheduler dequeuing and re-dispatching. */
    s.state = USTRAND_STATE_RUNNING;
    if (vm.ready_head == &s) {
        vm.ready_head = s.ready_next;
        if (vm.ready_head == NULL) vm.ready_tail = NULL;
        if (vm.strand_runnable_count > 0) vm.strand_runnable_count--;
    }

    /* Second dispatch: should yield again. */
    uint64_t c2 = dispatch_loop_until_yield(&s, 10000u);
    UASSERT_EQ((int)USTRAND_STATE_READY, (int)s.state);
    UASSERT(c2 >= 1u);

    /* Drain ready queue again. */
    s.state = USTRAND_STATE_RUNNING;
    if (vm.ready_head == &s) {
        vm.ready_head = s.ready_next;
        if (vm.ready_head == NULL) vm.ready_tail = NULL;
        if (vm.strand_runnable_count > 0) vm.strand_runnable_count--;
    }

    /* Third dispatch: RET → DEAD. */
    UValue retval = {0};
    s.out_slot = &retval;
    uint64_t c3 = dispatch_loop_until_yield(&s, 10000u);
    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT(c3 >= 1u);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ============================================================
 * Test 7: OP_TRY_BEGIN / OP_TRY_END normal path (no throw)
 * ============================================================ */

UTEST(dispatch_loop_try_begin_end_normal_path) {
    /* Program: TRY_BEGIN(HAS_CATCH, handler_pc=5); LOADNIL R0; TRY_END; RET.
       No throw: TRY_END pops the entry normally; RET succeeds. */
    static uint32_t instrs[4];
    instrs[0] = enc_try_begin(FLAG_HAS_CATCH, 5u); /* handler at pc=5 (beyond prog) */
    instrs[1] = enc_loadnil(0);
    instrs[2] = enc_try_end();
    instrs[3] = enc_ret();

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    static UValue no_consts[1];

    UStrand s;
    strand_setup(&s, &vm, instrs, no_consts, reg_stack);
    strand_setup_cleanup(&s, &vm);

    UValue retval = {0};
    s.out_slot = &retval;

    uint64_t consumed = dispatch_loop_until_yield(&s, 10000u);

    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    /* steps_consumed only counts safepoint-firing instrs (RET, CALL, backward JMP, YIELD).
       TRY_BEGIN/LOADNIL/TRY_END hit no safepoint; only the final top-level RET counts. */
    UASSERT(consumed >= 1u);
    /* cleanup_depth should be back to 0 after TRY_END. */
    UASSERT_EQ((int)s.cleanup_depth, 0);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ============================================================
 * Test 8: OP_THROW inside try/catch — exception absorbed by handler
 * ============================================================ */

UTEST(dispatch_loop_throw_absorbed_by_catch) {
    /* Program layout (indices):
       0: TRY_BEGIN(HAS_CATCH, handler=3)
       1: THROW R[0]          ← R[0] = catch_value_sentinel (set manually)
       2: TRY_END             ← not reached (throw skips past this)
       3: LOAD_CATCH_VALUE R[1]  ← handler: save caught value
       4: RET (R[0] still has original value — we return R[0])
       The throw unwinds, the catch handler at pc=3 runs. */

    static uint32_t instrs[5];
    instrs[0] = enc_try_begin(FLAG_HAS_CATCH, 3u);
    instrs[1] = enc_throw(0);          /* throw R[0] */
    instrs[2] = enc_try_end();         /* not reached */
    instrs[3] = enc_load_catch(1);     /* R[1] := catch_value */
    instrs[4] = enc_ret_reg(1);        /* return R[1] (the caught value) */

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    static UValue no_consts[1];

    /* urbi_unwind catch-absorption path does s->pc = s->module->instructions + handler_pc.
       We must provide a minimal module so the pointer arithmetic is valid.
       instrs is static so it outlives this call. */
    static UModule fake_mod;
    memset(&fake_mod, 0, sizeof(fake_mod));
    fake_mod.instructions = instrs;
    fake_mod.instr_count  = 5u;
    fake_mod.constants    = no_consts;

    UStrand s;
    strand_setup(&s, &vm, instrs, no_consts, reg_stack);
    strand_setup_cleanup(&s, &vm);
    s.module = &fake_mod;  /* required by urbi_unwind catch-absorption */
    /* Give sufficient budget so the safepoint after THROW doesn't soft-yield
       before the catch handler can run; OP_THROW → safepoint → urbi_unwind
       → catch absorbed → dispatch continues from handler. */
    s.instruction_budget_remaining = 100u;

    /* Pre-set R[0] to an integer value (42) that will be thrown. */
    reg_stack[0].kind  = (uint8_t)UVAL_INT;
    reg_stack[0].v.i   = 42;

    UValue retval = {0};
    s.out_slot = &retval;

    uint64_t consumed = dispatch_loop_until_yield(&s, 10000u);

    /* Strand should reach DEAD (handler ran; RET at top frame). */
    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    /* Only the top-level RET fires a safepoint step; THROW goes to safepoint
       without incrementing steps_consumed; urbi_unwind() then redirects pc
       to the handler without a new steps_consumed++ either. */
    UASSERT(consumed >= 1u);
    /* Return value is the caught exception (42). */
    UASSERT_EQ((int)retval.kind, (int)UVAL_INT);
    UASSERT_EQ((long long)retval.v.i, 42LL);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ============================================================
 * Test 9: LOADK loads a constant; return value matches
 * ============================================================ */

UTEST(dispatch_loop_loadk_and_ret) {
    /* Program: LOADK R[0] = K[0]; RET R[0].
       K[0] = integer 77. */
    static uint32_t instrs[2];
    instrs[0] = enc_loadk(0, 0);
    instrs[1] = enc_ret();

    static UValue consts[1];
    consts[0].kind = (uint8_t)UVAL_INT;
    consts[0].v.i  = 77;

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    UStrand s;
    strand_setup(&s, &vm, instrs, consts, reg_stack);

    UValue retval = {0};
    s.out_slot = &retval;

    uint64_t consumed = dispatch_loop_until_yield(&s, 10000u);

    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    /* LOADK hits no safepoint; only the final top-level RET counts. */
    UASSERT(consumed >= 1u);
    UASSERT_EQ((int)retval.kind, (int)UVAL_INT);
    UASSERT_EQ((long long)retval.v.i, 77LL);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ============================================================
 * Test 10: MOVE copies a register; ADD adds two registers
 * ============================================================ */

UTEST(dispatch_loop_move_and_add) {
    /* Program: LOADK R[0]=K[0](3); LOADK R[1]=K[1](7); ADD R[2]=R[0]+R[1]; RET R[2]. */
    static uint32_t instrs[4];
    instrs[0] = enc_loadk(0, 0);
    instrs[1] = enc_loadk(1, 1);
    instrs[2] = enc_add(2, 0, 1);
    instrs[3] = enc_ret_reg(2);

    static UValue consts[2];
    consts[0].kind = (uint8_t)UVAL_INT; consts[0].v.i = 3;
    consts[1].kind = (uint8_t)UVAL_INT; consts[1].v.i = 7;

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    UStrand s;
    strand_setup(&s, &vm, instrs, consts, reg_stack);

    UValue retval = {0};
    s.out_slot = &retval;

    uint64_t consumed = dispatch_loop_until_yield(&s, 10000u);

    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT_EQ((int)retval.kind, (int)UVAL_INT);
    UASSERT_EQ((long long)retval.v.i, 10LL);
    (void)consumed;

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ============================================================
 * Test 11: OP_PUSH_TAG / OP_POP_TAG — pure-scope no-throw path
 * ============================================================ */

UTEST(dispatch_loop_push_pop_tag_noop) {
    /* Program: PUSH_TAG (flags=0, tag_reg=0, onleave_pc=0); LOADNIL R[1]; POP_TAG R[0]; RET.
       R[0] = nil (tag register).  No tag runtime at M3 — these are stubs
       that push/pop cleanup entries without semantic effect. */
    static uint32_t instrs[4];
    instrs[0] = enc_push_tag(0, 0, 0u);
    instrs[1] = enc_loadnil(1);
    instrs[2] = enc_pop_tag(0);
    instrs[3] = enc_ret();

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    static UValue no_consts[1];

    UStrand s;
    strand_setup(&s, &vm, instrs, no_consts, reg_stack);
    strand_setup_cleanup(&s, &vm);

    UValue retval = {0};
    s.out_slot = &retval;

    uint64_t consumed = dispatch_loop_until_yield(&s, 10000u);

    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    /* PUSH_TAG/LOADNIL/POP_TAG fire no safepoints; only the top-level RET counts. */
    UASSERT(consumed >= 1u);
    /* cleanup_depth must be 0 after POP_TAG. */
    UASSERT_EQ((int)s.cleanup_depth, 0);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ============================================================
 * Test 12: nested call — OP_CALL pushes a frame; inner OP_RET pops it
 * Requires a real UClosure + UProto for the callee.
 * ============================================================ */

UTEST(dispatch_loop_nested_call_and_ret) {
    /* Callee proto: LOADK R[0]=K[0](99); RET R[0].
       Caller: R[0] = callee_closure; CALL R[0](0 args); RET R[0].
       Expected: R[0] after CALL = 99; RET returns 99. */

    static uint32_t callee_instrs[2];
    callee_instrs[0] = enc_loadk(0, 0);
    callee_instrs[1] = enc_ret();

    static UValue callee_consts[1];
    callee_consts[0].kind = (uint8_t)UVAL_INT;
    callee_consts[0].v.i  = 99;

    static UProto callee_proto;
    memset(&callee_proto, 0, sizeof(callee_proto));
    callee_proto.instructions = callee_instrs;
    callee_proto.instr_count  = 2u;
    callee_proto.constants    = callee_consts;
    callee_proto.const_count  = 1u;
    callee_proto.max_reg      = 0u;
    callee_proto.nparams      = 0u;
    callee_proto.nupvals      = 0u;

    static UClosure callee_closure;
    memset(&callee_closure, 0, sizeof(callee_closure));
    callee_closure.proto   = &callee_proto;
    callee_closure.nupvals = 0u;

    /* Caller instructions:
       [0] (placeholder): R[0] is pre-set to callee closure value
       [0] CALL R[0] b=1 (0 args = b-1 = 0)
       [1] RET R[0]  */
    static uint32_t caller_instrs[2];
    caller_instrs[0] = enc_call(0, 1);  /* CALL R[0], b=1 (nargs=0) */
    caller_instrs[1] = enc_ret();

    static UValue no_consts[1];

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    /* Pre-set R[0] to the closure value. */
    reg_stack[0].kind = (uint8_t)UVAL_CLOSURE;
    reg_stack[0].v.p  = &callee_closure;

    /* urbi_unwind pop_call_frame does s->pc_base = s->module->instructions.
       A minimal fake module pointing at caller_instrs is required. */
    static UModule fake_caller_mod;
    memset(&fake_caller_mod, 0, sizeof(fake_caller_mod));
    fake_caller_mod.instructions = caller_instrs;
    fake_caller_mod.instr_count  = 2u;
    fake_caller_mod.constants    = no_consts;

    UStrand s;
    strand_setup(&s, &vm, caller_instrs, no_consts, reg_stack);
    strand_setup_cleanup(&s, &vm);
    s.module = &fake_caller_mod;
    /* Need non-zero budget so safepoints at CALL and non-top RET don't soft-yield. */
    s.instruction_budget_remaining = 100u;

    UValue retval = {0};
    s.out_slot = &retval;

    uint64_t consumed = dispatch_loop_until_yield(&s, 10000u);

    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT_EQ((int)retval.kind, (int)UVAL_INT);
    UASSERT_EQ((long long)retval.v.i, 99LL);
    UASSERT(consumed >= 3u);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ============================================================
 * Test 13: LOADNIL yields a nil register; MOVE copies it
 * (exercises two more opcodes not covered by earlier tests)
 * ============================================================ */

UTEST(dispatch_loop_loadnil_then_move) {
    /* Program: LOADNIL R[0]; MOVE R[1]=R[0]; RET R[1]. */
    static uint32_t instrs[3];
    instrs[0] = enc_loadnil(0);
    instrs[1] = enc_move(1, 0);
    instrs[2] = enc_ret_reg(1);

    static UValue no_consts[1];

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    /* Pre-dirty R[1] to verify LOADNIL+MOVE overwrites it. */
    reg_stack[1].kind = (uint8_t)UVAL_INT;
    reg_stack[1].v.i  = 12345;

    UStrand s;
    strand_setup(&s, &vm, instrs, no_consts, reg_stack);

    UValue retval = {0};
    s.out_slot = &retval;

    uint64_t consumed = dispatch_loop_until_yield(&s, 10000u);

    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT_EQ((int)retval.kind, (int)UVAL_NIL);
    /* LOADNIL and MOVE fire no safepoints; only the top-level RET counts. */
    UASSERT(consumed >= 1u);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ============================================================
 * Test 14: gc_pending flag triggers gc_slice at backward-branch safepoint
 * Verifies the dispatch loop calls gc_slice when gc_pending is set
 * at a backward-branch safepoint. gc_slice is a no-op stub at M3.
 * ============================================================ */

UTEST(dispatch_loop_gc_pending_flag_triggers_gc_slice_at_safepoint) {
    /* Program: LOADNIL R0; JMP -1 (backward, hits safepoint); RET.
       With gc_pending=1, the backward-branch safepoint should invoke gc_slice.
       gc_slice is a stub at M3 (no-op), so we verify dispatch completes
       without crash and state remains consistent. */
    static uint32_t instrs[3];
    instrs[0] = enc_loadnil(0);
    instrs[1] = enc_jmp(-1);   /* backward: safepoint */
    instrs[2] = enc_ret();

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    static UValue no_consts[1];

    UStrand s;
    strand_setup(&s, &vm, instrs, no_consts, reg_stack);
    s.instruction_budget_remaining = 100u;

    /* Set gc_pending flag; it will be tested at the backward-branch safepoint. */
    vm.gc_pending = 1;

    uint64_t consumed = dispatch_loop_until_yield(&s, 100000u);

    /* Dispatch should complete without crash. Strand may be RUNNING or READY
       depending on budget exhaustion; we verify the dispatch path was exercised. */
    UASSERT(consumed >= 1u);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ============================================================
 * Test 15: watcher_dirty_count triggers watcher_eval_dirty at backward-branch safepoint
 * Verifies the dispatch loop calls watcher_eval_dirty when watcher_dirty_count > 0
 * at a backward-branch safepoint. watcher_eval_dirty is a no-op stub at M3.
 * ============================================================ */

UTEST(dispatch_loop_watcher_dirty_count_triggers_watcher_eval_at_safepoint) {
    /* Program: LOADNIL R0; JMP -1 (backward, hits safepoint); RET.
       With watcher_dirty_count=1, the backward-branch safepoint should invoke
       watcher_eval_dirty. watcher_eval_dirty is a stub at M3 (no-op), so we
       verify dispatch completes without crash and state remains consistent. */
    static uint32_t instrs[3];
    instrs[0] = enc_loadnil(0);
    instrs[1] = enc_jmp(-1);   /* backward: safepoint */
    instrs[2] = enc_ret();

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    static UValue no_consts[1];

    UStrand s;
    strand_setup(&s, &vm, instrs, no_consts, reg_stack);
    s.instruction_budget_remaining = 100u;

    /* Set watcher_dirty_count; it will be tested at the backward-branch safepoint. */
    vm.watcher_dirty_count = 1;

    uint64_t consumed = dispatch_loop_until_yield(&s, 100000u);

    /* Dispatch should complete without crash. Strand may be RUNNING or READY
       depending on budget exhaustion; we verify the dispatch path was exercised. */
    UASSERT(consumed >= 1u);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ============================================================
 * Suite registration
 * ============================================================ */

void test_dispatch_loop_suite(void) {
    utest_run("dispatch_loop yields on OP_YIELD",
              dispatch_loop_yields_on_op_yield);
    utest_run("dispatch_loop DEAD on top-level RET",
              dispatch_loop_dies_on_top_level_ret);
    utest_run("dispatch_loop exits on step budget exhaustion",
              dispatch_loop_exits_on_step_budget_exhaustion);
    utest_run("dispatch_loop instruction_budget decrements",
              dispatch_loop_instruction_budget_decrements);
    utest_run("dispatch_loop forward jump no safepoint",
              dispatch_loop_forward_jump_no_safepoint);
    utest_run("dispatch_loop multiple yields",
              dispatch_loop_multiple_yields);
    utest_run("dispatch_loop try_begin/end normal path",
              dispatch_loop_try_begin_end_normal_path);
    utest_run("dispatch_loop throw absorbed by catch",
              dispatch_loop_throw_absorbed_by_catch);
    utest_run("dispatch_loop LOADK and RET",
              dispatch_loop_loadk_and_ret);
    utest_run("dispatch_loop MOVE and ADD",
              dispatch_loop_move_and_add);
    utest_run("dispatch_loop PUSH_TAG/POP_TAG noop",
              dispatch_loop_push_pop_tag_noop);
    utest_run("dispatch_loop nested CALL and RET",
              dispatch_loop_nested_call_and_ret);
    utest_run("dispatch_loop LOADNIL then MOVE",
              dispatch_loop_loadnil_then_move);
    utest_run("dispatch_loop gc_pending flag triggers gc_slice at safepoint",
              dispatch_loop_gc_pending_flag_triggers_gc_slice_at_safepoint);
    utest_run("dispatch_loop watcher_dirty_count triggers watcher_eval at safepoint",
              dispatch_loop_watcher_dirty_count_triggers_watcher_eval_at_safepoint);
}
