/* SPDX-License-Identifier: BSD-3-Clause */
/* test_scratch_strand_safety — refactor-3 B11/SCHED-03 + v0.13.1-F re-verify.
 *
 * Bug B11/SCHED-03: run_on_scratch_core never seeded the per-strand safepoint
 * budget (safepoint_budget_remaining stayed 0 from urbi_zero).  The first
 * backward branch (loop) or function call inside the scratch body hit the
 * safepoint budget-check at 0 and called sched_strand_yield, which ENQUEUED
 * the stack-allocated UStrand onto vm->ready_head — a dead-stack UAF on the
 * next urbi_step call.
 *
 * Fix (T7): seed safepoint_budget_remaining = URBI_SCRATCH_BUDGET_OPS after
 * urbi_strand_arm_from_closure.  Also: transient guards in uvm.c prevent
 * budget-exhaust arms from calling sched_strand_yield for any transient
 * strand; explicit unpark/unbind in the fail-soft arm ensures the stack
 * strand is removed from scheduler queues before the frame dies; teardown
 * asserts pin the invariant.
 *
 * v0.13.1-F: urbi_vm_run-driven VM with an at-watcher installed and a dirty
 * slot write pending at urbi_vm_destroy.  The v0.13.1 T13-T15 changes
 * (strand_cleanup_observers in ustrand_destroy) handle the parked-loader
 * shape; this test pins that the scenario stays clean under ASan. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include <stddef.h>
#include <string.h>

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "chunk/uchunk.h"
#include "runtime/uclosure.h"
#include "watcher/uwatcher.h"
#include "value/uarena.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"

#define UTEST(name) static void name(void)

/* ---- compile helper (compile src into caller-supplied module) ---------- */

static int
scratch_compile(UVM *vm, const char *src, UProto *mod_out)
{
    ULexer   lex;
    UEmitter e;
    UParser  p;
    UAstNode *node;
    UArena   arena;

    uarena_init(&arena, 4096);
    ulex_init(&lex, src, strlen(src));
    *mod_out = (UProto){0};
    uemit_init(&e, mod_out, &arena, vm, NULL);
    uparse_init(&p, &lex, &arena);

    int ok = 1;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) { ok = 0; break; }
        if (uemit_statement(&e, node) != EMIT_OK) { ok = 0; break; }
        uarena_reset(&arena);
    }
    if (ok && uemit_finish(&e) != EMIT_OK) ok = 0;
    uarena_destroy(&arena);
    return ok;
}

/* ===================================================================
 * Test 1: scratch_budget_seeded_loop_completes
 *
 * A scratch body with a while loop triggers a backward branch at the
 * `safepoint:` label in dispatch_loop_until_yield.  Pre-fix (budget == 0
 * + no transient guard), the first backward branch immediately called
 * sched_strand_yield, enqueuing the stack-allocated UStrand onto
 * vm->ready_head — a dead-stack UAF on the next urbi_step call.
 *
 * Post-fix (T7): budget seeded to URBI_SCRATCH_BUDGET_OPS after arm +
 * transient guard in uvm.c prevents sched_strand_yield for transient
 * strands.  The 5-iteration loop runs to completion (DEAD);
 * vm->ready_head stays NULL and *out_threw == 0.
 *
 * Red assertion (pre-fix): vm.ready_head != NULL.
 * =================================================================== */
UTEST(scratch_budget_seeded_loop_completes)
{
    UVM    vm;
    UArena arena;
    UProto module;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    uarena_init(&arena, 4096);
    memset(&module, 0, sizeof(module));

    /* Use a function literal so "var i" is a local register (no OP_SETSLOT
     * on the frozen realm global_object) and the UClosure is returned with
     * proto_inst set — required for ic_resolve_pi to find the IC table
     * during the scratch dispatch.
     *
     * Use `|` (atomic sequential) instead of `;` between statements: `;`
     * generates OP_YIELD which is a cooperative yield point; `|` does not.
     * The while loop's backward OP_JMP fires the safepoint on every iteration
     * (5 iterations = 5 safepoint triggers), pinning the B11/SCHED-03 budget
     * seed fix.  With the budget seeded and the transient guard in place the
     * body runs to DEAD without touching the scheduler queues. */
    UValue fn = {0};
    int rc = utest_e2e_compile_and_run_with_module(
        &vm, &arena, &module,
        "function() { var i = 0 | while (i < 5) { i = i + 1 } }",
        &fn);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_CLOSURE, (int)fn.kind);
    if (fn.kind != (uint8_t)UVAL_CLOSURE) {
        uchunk_destroy(&module, &vm);
        uarena_destroy(&arena);
        urbi_vm_destroy(&vm);
        return;
    }

    UValue result = {0};
    int    threw  = 0;
    rc = urbi_run_closure_on_scratch(&vm, (UClosure *)fn.v.p, &result, &threw);
    UASSERT_EQ(0, rc);

    /* Post-fix: body runs to completion; vm.ready_head must be NULL.
     * Pre-fix: vm.ready_head != NULL (dead-stack pointer from
     * sched_strand_yield enqueueing the stack UStrand). */
    UASSERT(vm.ready_head == NULL);

    /* Post-fix: no throw; pre-fix: threw == 1 from fail-soft else arm. */
    UASSERT_EQ(0, threw);

    uchunk_destroy(&module, &vm);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* ---- mock clock for sleep test ---------------------------------------- */

static uint64_t g_mock_t_safety;
static uint64_t mock_clock_safety(void *ud) { (void)ud; return g_mock_t_safety; }

/* ===================================================================
 * Test 2: scratch_body_sleep_failsoft_unparks
 *
 * A scratch body that calls sleep() blocks the strand on the sleep queue
 * (USTRAND_REASON_SLEEP).  At teardown the strand must NOT be reachable
 * via vm->sleep_q_head — a dead-stack UAF would occur at the next
 * sched_wake_due_sleepers call.
 *
 * T3 (SCHED-05) shipped strand_cleanup_observers in ustrand_destroy which
 * calls strand_unlink_park for WAITING strands.  T7 adds an EXPLICIT unpark
 * before ustrand_destroy (belt-and-suspenders, mirrors cleanup executor at
 * uunwind.c:314).  This test pins both layers: vm->sleep_q_head == NULL
 * after run_on_scratch_core returns.
 * =================================================================== */
UTEST(scratch_body_sleep_failsoft_unparks)
{
    UVM    vm;
    UProto mod;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* sleep() requires a time source. */
    g_mock_t_safety = 1000000ULL;  /* 1 s past epoch */
    urbi_set_clock_fn(&vm, mock_clock_safety, NULL);

    /* sleep(1) — duration 1 microsecond (INT arg interpreted as µs). */
    int ok = scratch_compile(&vm, "sleep(1)", &mod);
    UASSERT(ok);
    if (!ok) { urbi_vm_destroy(&vm); return; }

    UClosure cl;
    memset(&cl, 0, sizeof(cl));
    cl.proto = &mod;
    cl.nupvals = 0;

    UValue result = {0};
    int    threw  = 0;
    (void)urbi_run_closure_on_scratch(&vm, &cl, &result, &threw);

    /* threw == 1: body blocked (WAITING violates the no-yield contract). */
    UASSERT_EQ(1, threw);

    /* Sleep queue must be empty after run_on_scratch_core returns.
     * Pre-T3: sleep queue retained a dead-stack pointer (UAF).
     * Post-T3/T7: either the explicit unpark or ustrand_destroy's
     * strand_cleanup_observers removes the strand from the sleep queue. */
    UASSERT(vm.sleep_q_head == NULL);

    uchunk_destroy(&mod, NULL);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 3: v0.13.1-F re-verify — urbi_vm_run transient teardown
 *
 * Install an at-watcher via urbi_vm_run, then write the watched slot
 * (making the watcher condition dirty) WITHOUT running any urbi_step steps.
 * Call urbi_vm_destroy directly.
 *
 * Pre-v0.13.1 shape: the urbi_vm_run transient strand is stack-local and
 * linked into realm->strands_head during dispatch.  A watcher remaining
 * dirty at VM destroy could UAF if the VM's realm teardown walked a
 * stale watcher back-pointer into an already-dead transient's frame.
 *
 * Post-v0.13.1: urbi_vm_destroy → urbi_realm_destroy → watcher unregister
 * runs the proper teardown; the transient is already unlinked from
 * strands_head before ustrand_destroy (see uvm_run.c teardown block).
 * Under ASan, any stale pointer would be detected here.
 *
 * This test is GREEN at v0.13.1+ and closes design-risk v0.13.1-F as
 * "already handled; pinned by this test".
 * =================================================================== */
UTEST(vm_run_transient_teardown_with_dirty_watcher)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *gr = urbi_realm_global(&vm);
    UASSERT(gr != NULL);
    if (gr == NULL) { urbi_vm_destroy(&vm); return; }

    /* Seed the watched slot. */
    int rc = urbi_realm_set_global(&vm, gr, "x", 1, utest_e2e_make_int(0));
    UASSERT_EQ(URBI_OK, rc);

    /* Install an at sync watcher; the transient strand is alive during
     * the compile_and_run call and unlinked on return. */
    rc = utest_e2e_compile_and_run(&vm,
        "at sync (Realm.x > 10) Realm.x = Realm.x + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);
    if (rc != URBI_OK) { urbi_vm_destroy(&vm); return; }

    /* Dirty the watcher via the public API (no urbi_step follows). */
    (void)urbi_realm_set_global(&vm, gr, "x", 1, utest_e2e_make_int(20));

    /* Destroy without draining: the parked-loader shape.
     * ASan would detect any stale pointer into the transient's stack frame. */
    urbi_vm_destroy(&vm);
    /* Reaching here confirms v0.13.1-F is closed (no UAF under ASan). */
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_scratch_strand_safety_suite(void)
{
    printf("test_scratch_strand_safety\n");
    utest_run("scratch_budget_seeded_loop_completes",
              scratch_budget_seeded_loop_completes);
    utest_run("scratch_body_sleep_failsoft_unparks",
              scratch_body_sleep_failsoft_unparks);
    utest_run("vm_run_transient_teardown_with_dirty_watcher",
              vm_run_transient_teardown_with_dirty_watcher);
}
