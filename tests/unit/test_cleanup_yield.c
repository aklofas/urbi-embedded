/* SPDX-License-Identifier: BSD-3-Clause */
/* test_cleanup_yield — refactor-3 VM-02 + SCHED-10 cleanup-executor hardening.
 *
 * Cleanup bodies (finally / onleave) execute atomically: a body that blocks
 * (sleep), yields, or exhausts its budget mid-run must produce a LOUD fatal
 * (URBI_STEP_FATAL with the strand DEAD and off every scheduler queue), not
 * a silent truncation that leaves the strand parked on the sleep/ready queue
 * while the unwind walker keeps tearing it down (debug-assert abort /
 * queue corruption pre-fix).
 *
 * Cases:
 *   1. finally_that_blocks_is_fatal: sleep(1s) in a finally reached via the
 *      unwind path (tag.stop).  Pre-fix: sched_wake_due_sleepers /
 *      sched_strand_yield assert-abort (debug) or queue corruption (release).
 *      Post-fix: bounded urbi_step driving returns URBI_STEP_FATAL; the
 *      strand is DEAD and absent from both the sleep and ready queues.
 *   2. finally_budget_exhaustion_is_fatal: a budget-exhausting while loop
 *      (200k iterations) in a finally on the unwind path.  Same contract.
 *   3. eval_after_c1_replace_still_works: C-1 replace-on-raise where the
 *      cleanup body's new throw is absorbed by an outer catch INSIDE the
 *      nested dispatch.  Pre-fix run_cleanup_with_replace resurrected the
 *      suppressed original unwind after the script had already handled
 *      control flow, marking the strand fatal and poisoning the session
 *      (vm->fatal_strand latched; every subsequent urbi_step returns
 *      URBI_STEP_FATAL).  Post-fix: the run completes without fatal and a
 *      subsequent `1 + 1` chunk on the SAME vm evaluates to 2. */

#include "utest.h"

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"
#include "chunk/uchunk.h"
#include "realm/urealm.h"
#include "value/uarena.h"
#include "runtime/umacros.h"
#include "lex/ulex.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "emit/uemit.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers
 * =================================================================== */

/* Compile `src` into a heap-allocated module (heap so a fatal loader
 * strand's undischarged root_proto ref routes through rescued_protos
 * instead of dangling on a stack address — mirrors utest_e2e_helpers). */
static UProto *
compile_heap_chunk(UVM *vm, const char *src)
{
    UProto *module = (UProto *)vm->alloc_fn(NULL, sizeof(UProto), vm->alloc_ud);
    if (module == NULL) return NULL;
    urbi_zero(module, sizeof(*module));
    module->heap_allocated = true;
    module->alloc_fn       = vm->alloc_fn;
    module->alloc_ud       = vm->alloc_ud;

    UArena arena;
    uarena_init(&arena, 4096);

    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UEmitter e;
    uemit_init(&e, module, &arena, vm, NULL);
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

    if (!ok) {
        uchunk_destroy(module, vm);
        return NULL;
    }
    return module;
}

static int
on_sleep_queue(const UVM *vm, const UStrand *s)
{
    const UStrand *p;
    for (p = vm->sleep_q_head; p != NULL; p = p->wait_next)
        if (p == s) return 1;
    return 0;
}

static int
on_ready_queue(const UVM *vm, const UStrand *s)
{
    const UStrand *p;
    for (p = vm->ready_head; p != NULL; p = p->ready_next)
        if (p == s) return 1;
    return 0;
}

/* Arm a loader strand for `module` and drive urbi_step bounded.
 * Returns the last UStepResult; *out_loader receives the strand pointer
 * (valid for address comparison; deref only safe on the FATAL path where
 * the scheduler does not eager-reap). */
static UStepResult
drive_chunk(UVM *vm, UProto *module, UValue *out_result,
            UStrand **out_loader, int max_steps)
{
    URealm *realm = urbi_realm_global(vm);
    UStepResult rc = URBI_STEP_RUNNING;

    UStrand *loader = urbi_strand_create_for_module(vm, realm, module);
    if (out_loader != NULL) *out_loader = loader;
    if (loader == NULL) return URBI_STEP_FATAL;
    loader->out_slot = out_result;

    for (int i = 0; i < max_steps; i++) {
        rc = urbi_step(vm, 1000, NULL);
        if (rc == URBI_STEP_FATAL)     break;
        if (rc == URBI_STEP_QUIESCENT) break;
        if (rc == URBI_STEP_WAKE_AT)   break;
    }
    return rc;
}

/* ===================================================================
 * Case 1: a finally body that BLOCKS (sleep) on the unwind path must
 * produce a loud fatal — not a silent truncation that parks the strand
 * on the sleep queue while the walker keeps unwinding it.
 * =================================================================== */
UTEST(finally_that_blocks_is_fatal)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UProto *module = compile_heap_chunk(&vm,
        "var t = Tag.new(); "
        "t: { try { t.stop() } finally { sleep(1s); Realm.b9 = 2 } }");
    UASSERT(module != NULL);

    UValue result = urbi_make_nil();
    UStrand *loader = NULL;
    UStepResult rc = drive_chunk(&vm, module, &result, &loader, 100);

    /* Loud fatal, not a parked/corrupted strand. */
    UASSERT_EQ((int)URBI_STEP_FATAL, (int)rc);
    UASSERT(vm.fatal_strand == loader);
    UASSERT_EQ((unsigned)USTRAND_DEAD, (unsigned)USTRAND_GET_STATE(loader));

    /* The strand must NOT be linked on the sleep queue or ready queue. */
    UASSERT_EQ(0, on_sleep_queue(&vm, loader));
    UASSERT_EQ(0, on_ready_queue(&vm, loader));

    uchunk_destroy(module, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 2: a finally body that exhausts the cleanup budget (200k-iter
 * while loop) on the unwind path must also produce a loud fatal.
 * Pre-fix: the backward-JMP safepoint yields the strand mid-unwind
 * (sched_strand_yield assert-abort in debug; ready-queue corruption
 * in release).
 * =================================================================== */
UTEST(finally_budget_exhaustion_is_fatal)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UProto *module = compile_heap_chunk(&vm,
        "var t = Tag.new(); "
        "t: { try { t.stop() } finally { "
        "var i = 0; while (i < 200000) { i = i + 1 }; Realm.b9 = 2 } }");
    UASSERT(module != NULL);

    UValue result = urbi_make_nil();
    UStrand *loader = NULL;
    UStepResult rc = drive_chunk(&vm, module, &result, &loader, 100);

    UASSERT_EQ((int)URBI_STEP_FATAL, (int)rc);
    UASSERT(vm.fatal_strand == loader);
    UASSERT_EQ((unsigned)USTRAND_DEAD, (unsigned)USTRAND_GET_STATE(loader));
    UASSERT_EQ(0, on_sleep_queue(&vm, loader));
    UASSERT_EQ(0, on_ready_queue(&vm, loader));

    uchunk_destroy(module, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 3: C-1 replace where the cleanup body's new throw is absorbed
 * by an outer catch inside the nested dispatch must NOT poison the
 * session — a subsequent eval on the same vm must still work.
 * =================================================================== */
UTEST(eval_after_c1_replace_still_works)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* Run the double-throw construct: inner finally raises throw(2) while
     * unwinding throw(1); the outer catch absorbs the replacement inside
     * the cleanup body's nested dispatch and the chunk runs to completion. */
    UProto *m1 = compile_heap_chunk(&vm,
        "try { try { throw(1) } finally { throw(2) } } catch (var e) { e }");
    UASSERT(m1 != NULL);

    UValue r1 = urbi_make_nil();
    UStrand *l1 = NULL;
    UStepResult rc1 = drive_chunk(&vm, m1, &r1, &l1, 100);

    /* No fatal: the script handled all control flow itself. */
    UASSERT(rc1 != URBI_STEP_FATAL);
    UASSERT(vm.fatal_strand == NULL);

    /* Subsequent eval on the SAME vm must produce 2, not an error. */
    UProto *m2 = compile_heap_chunk(&vm, "1 + 1");
    UASSERT(m2 != NULL);

    UValue r2 = urbi_make_nil();
    UStrand *l2 = NULL;
    UStepResult rc2 = drive_chunk(&vm, m2, &r2, &l2, 100);

    UASSERT(rc2 != URBI_STEP_FATAL);
    UASSERT_EQ((int)UVAL_INT, (int)r2.kind);
    UASSERT_EQ((int64_t)2, r2.v.i);

    uchunk_destroy(m1, &vm);
    uchunk_destroy(m2, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry point.
 * =================================================================== */

void
test_cleanup_yield_suite(void)
{
    utest_run("cleanup_yield: finally body that blocks (sleep) is a loud fatal",
              finally_that_blocks_is_fatal);
    utest_run("cleanup_yield: finally body that exhausts the budget is a loud fatal",
              finally_budget_exhaustion_is_fatal);
    utest_run("cleanup_yield: eval after C-1 replace-absorb still works (no session poison)",
              eval_after_c1_replace_still_works);
}
