/* SPDX-License-Identifier: BSD-3-Clause */
/* test_scratch_cur_strand — refactor-3 VM-10 + SCHED-10 (scratch half).
 *
 * run_on_scratch_core (src/watcher/uwatcher_scratch.c) drives a nested
 * dispatch_loop_until_yield for watcher conds, at-sync bodies, onleave
 * handlers, getter/setter bodies, and event sync-emit bodies.  Pre-fix it
 * never set vm->cur_strand to the scratch strand and never preserved the
 * embedder's urbi_step budget:
 *
 *   VM-10: slot faults (uvm_slot.c slot_throw_or_fatal) throw on
 *   vm->cur_strand — the OUTER strand (e.g. the loader strand executing
 *   OP_AT_INSTALL) or NULL.  The scratch run sails past the faulting op
 *   with a stale register, "completes" cleanly, and the outer strand later
 *   unwinds an uncaught TypeError it never raised (or the throw is lost
 *   if no safepoint intervenes before the outer strand dies).
 *
 *   SCHED-10: dispatch_loop_until_yield overwrites
 *   vm->step_budget_remaining at entry, so a scratch eval mid-urbi_step
 *   resets the embedder's budget to URBI_SCRATCH_BUDGET_OPS leftovers.
 *
 * Pre-fix behavior recorded empirically at HEAD (7d72cd12):
 *   - case 1: outer.pending_unwind == UEXEC_THROW — the cross-strand
 *     deposit, confirmed.  *out_threw was already 1, but only by ACCIDENT:
 *     the zeroed scratch strand's instruction_budget_remaining == 0 made
 *     the first safepoint after the fault yield the strand
 *     (sched_strand_yield even enqueued the stack-local transient on the
 *     VM ready queue), and the "yielded/blocked" fail-soft arm fired.
 *   - case 2: vm->step_budget_remaining clobbered to
 *     URBI_SCRATCH_BUDGET_OPS (sentinel lost).
 *   - case 3: passed at HEAD via the same accidental yield (TRACE_FAULT
 *     reached by luck), but the loader strand still received the spurious
 *     pending_unwind deposit — unobserved only because the loader halts at
 *     OP_AT_INSTALL before its next safepoint.  The case is kept because
 *     it pins the fail-soft half of the fix: with cur_strand routed to the
 *     scratch strand, an unhandled typed throw unwinds the scratch strand
 *     to DEAD with fatal_status latched while vm->last_error stays UVM_OK;
 *     without a fatal_status detection arm in run_on_scratch_core the
 *     death reads as a clean OP_RET returning nil — cond_threw would be 0
 *     and the broken watcher would be silently installed.
 *
 * Post-fix contract:
 *   - The fault lands on the scratch strand; the scratch run reports
 *     *out_threw = 1 (fail-soft, via the fatal_status arm) and the outer
 *     strand keeps a clean pending_unwind.
 *   - vm->cur_strand and vm->step_budget_remaining are restored around the
 *     nested dispatch.
 *   - The at-install fault routes through the deliberate VM-002 install
 *     fault (URBI_INSTALL_TRACE_FAULT → "condition threw during trace"
 *     halt at the OP_AT_INSTALL site) instead of a spurious cross-strand
 *     throw, and the watcher is NOT left installed. */

#include "utest.h"

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "chunk/uchunk.h"
#include "realm/urealm.h"
#include "value/uarena.h"
#include "runtime/uclosure.h"
#include "runtime/umacros.h"
#include "watcher/uwatcher.h"
#include "lex/ulex.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "emit/uemit.h"
#include "utest_e2e_helpers.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers
 * =================================================================== */

/* Compile `src` into a heap-allocated module (mirrors test_cleanup_yield.c:
 * heap so a fatal loader strand's undischarged root_proto ref routes through
 * rescued_protos instead of dangling on a stack address). */
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

/* Arm a loader strand for `module` and drive urbi_step bounded.
 * Mirrors test_cleanup_yield.c's drive_chunk. */
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
 * Case 1 (VM-10): a slot fault inside a scratch run must land on the
 * SCRATCH strand (fail-soft via *out_threw), not on whatever strand
 * vm->cur_strand happened to point at.
 * =================================================================== */
UTEST(slot_fault_lands_on_scratch_strand_not_outer)
{
    UVM    vm;
    UArena arena;
    UProto module;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    uarena_init(&arena, 4096);
    memset(&module, 0, sizeof(module));

    /* Capture a function-literal closure whose body faults on an
     * unresolved slot (vm_getslot_slow → slot_throw_or_fatal). */
    UValue fn = {0};
    int rc = utest_e2e_compile_and_run_with_module(
        &vm, &arena, &module, "function() { Realm.missing_obj }", &fn);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_CLOSURE, (int)fn.kind);

    /* Simulate an outer strand mid-dispatch (e.g. the loader strand
     * executing OP_AT_INSTALL): vm->cur_strand points at it while the
     * scratch helper runs. */
    UStrand outer;
    memset(&outer, 0, sizeof(outer));
    outer.vm      = &vm;
    vm.cur_strand = &outer;

    UValue out   = {0};
    int    threw = 0;
    rc = urbi_run_closure_on_scratch(&vm, (UClosure *)fn.v.p, &out, &threw);
    UASSERT_EQ(0, rc);

    /* Post-fix: the fault is detected on the scratch strand (fail-soft). */
    UASSERT_EQ(1, threw);

    /* Post-fix: NO cross-strand deposit on the outer strand. */
    UASSERT_EQ((int)UEXEC_OK, (int)outer.pending_unwind);

    /* Post-fix: vm->cur_strand restored to the caller's value. */
    UASSERT(vm.cur_strand == &outer);

    vm.cur_strand = NULL;
    uchunk_destroy(&module, &vm);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 2 (SCHED-10): the scratch dispatch must not clobber the
 * embedder's urbi_step budget.  Preset vm->step_budget_remaining to a
 * sentinel, run a trivial closure on scratch, assert it is unchanged.
 * =================================================================== */
UTEST(step_budget_preserved_across_scratch_run)
{
    UVM    vm;
    UArena arena;
    UProto module;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    uarena_init(&arena, 4096);
    memset(&module, 0, sizeof(module));

    ULexer lex;
    ulex_init(&lex, "42", 2);
    UEmitter e;
    uemit_init(&e, &module, &arena, &vm, NULL);
    UParser p;
    uparse_init(&p, &lex, &arena);
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        UASSERT(node->kind != AST_ERROR);
        UASSERT_EQ((int)EMIT_OK, (int)uemit_statement(&e, node));
        uarena_reset(&arena);
    }
    UASSERT_EQ((int)EMIT_OK, (int)uemit_finish(&e));

    /* Stack-local proto/closure wrapper (mirrors test_uwatcher_scratch.c). */
    UProto proto;
    memset(&proto, 0, sizeof(proto));
    proto.instructions = module.instructions;
    proto.instr_count  = module.instr_count;
    proto.constants    = module.constants;
    proto.const_count  = module.const_count;
    proto.ic_count     = module.ic_count;
    proto.ic_names     = module.ic_names;

    UClosure cl;
    memset(&cl, 0, sizeof(cl));
    cl.proto   = &proto;
    cl.nupvals = 0;

    /* Pre-create the global realm: run_on_scratch_core's realm-link step
     * calls urbi_realm_global, which on FIRST use lazily creates the realm
     * and boots the stdlib (urbi_run_chunk → urbi_step inner loop) — a
     * once-per-VM side effect that also rewrites step_budget_remaining and
     * would mask the dispatch-clobber this case pins.  Real scratch runs
     * happen mid-urbi_step on a fully-booted VM, so boot first. */
    UASSERT(urbi_realm_global(&vm) != NULL);

    /* Sentinel: mid-urbi_step leftover budget the embedder still owns. */
    vm.step_budget_remaining = 7777U;

    UValue out   = {0};
    int    threw = 0;
    int    rc    = urbi_run_closure_on_scratch(&vm, &cl, &out, &threw);

    UASSERT_EQ(0, rc);
    UASSERT_EQ(0, threw);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(42, (int)out.v.i);

    /* Post-fix: budget untouched (pre-fix: overwritten with
     * URBI_SCRATCH_BUDGET_OPS leftovers by dispatch_loop_until_yield). */
    UASSERT_EQ(7777, (long long)vm.step_budget_remaining);

    /* Post-fix: cur_strand restored to the caller's value (NULL here). */
    UASSERT(vm.cur_strand == NULL);

    vm.step_budget_remaining = 0U;
    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 3 (VM-10 end-to-end): an at-cond that slot-faults during the
 * OP_AT_INSTALL scratch eval must NOT deposit the throw on the loader
 * strand.  Pre-fix the loader dies of an uncaught TypeError it never
 * raised (URBI_STEP_FATAL, fatal_strand == loader) at its next safepoint
 * (the while loop's backward JMP), with the broken watcher silently
 * installed.  Post-fix the install fail-faults via the deliberate VM-002
 * path: "watcher install: condition threw during trace" halt at the
 * OP_AT_INSTALL site, no fatal_strand, no watcher left installed.
 * =================================================================== */
UTEST(at_install_cond_fault_no_spurious_outer_throw)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UProto *module = compile_heap_chunk(&vm,
        "Realm.seen = 0; "
        "at (Realm.missing) { Realm.seen = 1 }; "
        "var i = 0; while (i < 3) { i = i + 1 }; "
        "Realm.done = 1");
    UASSERT(module != NULL);

    UValue result = urbi_make_nil();
    UStrand *loader = NULL;
    UStepResult rc = drive_chunk(&vm, module, &result, &loader, 100);

    /* Post-fix: no spurious cross-strand throw — the loader must not die
     * of an uncaught TypeError raised by the cond's scratch run. */
    UASSERT(rc != URBI_STEP_FATAL);
    UASSERT(vm.fatal_strand == NULL);

    /* Post-fix: the failure is the deliberate install fault, raised at
     * the OP_AT_INSTALL site (VM-002 promotes URBI_INSTALL_TRACE_FAULT
     * to a clean halt with a diagnosable message). */
    UASSERT_EQ((int)UVM_TYPE_ERROR, (int)vm.last_error);
    UASSERT(strstr(vm.last_errmsg, "condition threw during trace") != NULL);

    /* Post-fix: the broken watcher must NOT be left installed
     * (pre-fix it was: the cond "evaluated" to nil and install
     * proceeded). */
    UASSERT(vm.active_watchers_head == NULL);

    uchunk_destroy(module, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry point.
 * =================================================================== */

void
test_scratch_cur_strand_suite(void)
{
    utest_run("scratch_cur_strand: slot fault lands on scratch strand, not outer",
              slot_fault_lands_on_scratch_strand_not_outer);
    utest_run("scratch_cur_strand: urbi_step budget preserved across scratch run",
              step_budget_preserved_across_scratch_run);
    utest_run("scratch_cur_strand: at-install cond fault has no spurious outer throw",
              at_install_cond_fault_no_spurious_outer_throw);
}
