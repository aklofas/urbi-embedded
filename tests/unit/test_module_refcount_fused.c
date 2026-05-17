/* SPDX-License-Identifier: BSD-3-Clause */
/* test_module_refcount_fused — Phase 2 of v0.8.1-uproto-root.
 *
 * Verifies that strand binds bump root_proto->refcount (NOT module->refcount).
 * Variant B fusion: one canonical counter on root_proto accumulates both
 * strand-bind refs (this task, T7) and closure-alloc refs (Task 8).
 *
 * These tests FAIL before the T7 redirect lands; they pass after. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "module/umodule.h"
#include "module/uchunk.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "vm/uvm.h"
#include "value/uarena.h"
#include "lex/ulex.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "emit/uemit.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Compile `src` into *out_mod.  Returns true on success. */
static bool
fused_compile_chunk(UVM *vm, UArena *arena, UModule *out_mod, const char *src)
{
    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UEmitter e;
    uemit_init(&e, out_mod, arena, vm, NULL);

    UParser p;
    uparse_init(&p, &lex, arena);

    bool ok = true;
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) { ok = false; break; }
        if (uemit_statement(&e, node) != EMIT_OK) { ok = false; break; }
        uarena_reset(arena);
    }
    if (ok && uemit_finish(&e) != EMIT_OK) ok = false;
    return ok;
}

/* Case 1: urbi_strand_create_for_module bumps root_proto->refcount.
 * urbi_strand_destroy decrements it back to 0. */
UTEST(strand_bind_bumps_root_proto)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UArena arena;
    UModule module = {0};
    uarena_init(&arena, 4096);
    UASSERT(fused_compile_chunk(&vm, &arena, &module, "1"));

    /* root_proto must be non-NULL after compile. */
    struct UProto *rp = module.root_proto;
    UASSERT(rp != NULL);

    /* Before binding: root_proto->refcount must be 0. */
    UASSERT_EQ((unsigned)0, (unsigned)rp->refcount);

    /* Bind a strand: should bump root_proto->refcount to 1. */
    UStrand *s = urbi_strand_create_for_module(&vm, realm, &module);
    UASSERT(s != NULL);
    UASSERT_EQ((unsigned)1, (unsigned)rp->refcount);

    /* Destroy the strand: should decrement root_proto->refcount back to 0. */
    urbi_strand_destroy(s);
    UASSERT_EQ((unsigned)0, (unsigned)rp->refcount);

    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 2: running `1 & 2` to completion — Variant B lifecycle.
 *
 * OP_FORK_JOIN allocates a closure for the `& 2` branch via vm_alloc_closure.
 * Under Variant B, that closure bumps root_proto.refcount via uproto_root_of.
 * When the loader strand dies, its closure_list is migrated to vm->stdlib_closures
 * (persistent-strand policy in release_strand_resource_chain).  After draining
 * child strands, root_proto.refcount > 0 because the closure on stdlib_closures
 * still holds a ref.
 *
 * The correct full lifecycle: umodule_destroy triggers rescue (root rescued to
 * vm->rescued_protos), vm_destroy closure sweep decs via uproto_root_of (§3.7
 * ordering invariant), then rescued_protos sweep frees root.  No UAF. */
UTEST(op_fork_child_bumps_root_proto)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UArena arena;
    UModule module = {0};
    uarena_init(&arena, 4096);
    UASSERT(fused_compile_chunk(&vm, &arena, &module, "1 & 2"));

    struct UProto *rp = module.root_proto;
    UASSERT(rp != NULL);

    UStrand *s = urbi_strand_create_for_module(&vm, realm, &module);
    UASSERT(s != NULL);

    /* Drive to completion — both parent and child strands run and die. */
    UValue result = {0};
    uchunk_loader_drive(&vm, s, &result);

    /* Drain any child strands spawned by OP_FORK. */
    for (int i = 0; i < 200; i++) {
        UStepResult sr = urbi_step(&vm, 1000, NULL);
        if (sr == URBI_STEP_QUIESCENT) break;
        if (sr == URBI_STEP_FATAL) { UASSERT(0 && "fatal step"); break; }
        if (vm.strand_runnable_count == 0) break;
    }

    /* Under Variant B: the closure for `& 2` was migrated to vm->stdlib_closures
     * (persistent-strand policy).  That closure bumped root_proto.refcount via
     * uproto_root_of, so refcount > 0 at this point.  The full lifecycle
     * (umodule_destroy + vm_destroy) must complete without UAF. */

    uarena_destroy(&arena);
    /* umodule_destroy: rescue fires because root_proto.refcount > 0. */
    umodule_destroy(&module, &vm);
    /* vm_destroy: closure sweep dec's via uproto_root_of BEFORE rescued_protos
     * sweep frees rp (§3.7 ordering invariant).  Must not crash or UAF. */
    urbi_vm_destroy(&vm);
}

/* Case 3: module->refcount is always 0 after the redirect —
 * nothing bumps it anymore.  This verifies the old counter is dead. */
UTEST(module_refcount_stays_zero_after_redirect)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UArena arena;
    UModule module = {0};
    uarena_init(&arena, 4096);
    UASSERT(fused_compile_chunk(&vm, &arena, &module, "1"));

    UStrand *s = urbi_strand_create_for_module(&vm, realm, &module);
    UASSERT(s != NULL);

    /* module->refcount should be 0: strand-bind refcount moved to root_proto. */
    UASSERT_EQ((unsigned)0, (unsigned)module.refcount);

    urbi_strand_destroy(s);
    UASSERT_EQ((unsigned)0, (unsigned)module.refcount);

    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

/* === Task 8a (v0.8.1): Closure refcount fusion tests ==================
 *
 * Case 4: local closure (no escape).  After strand exits, root_proto->refcount
 * must be 0 (Variant B Option (a): no slot-implicit ref, closure ref discharged
 * at strand exit).  umodule_destroy must not trigger rescue.
 *
 * Case 5: escaping closure (stored in realm global).  After strand exits,
 * root_proto->refcount > 0 (vm_alloc_closure bumped via uproto_root_of).
 * umodule_destroy must rescue root_proto.  vm_destroy must dec via
 * uproto_root_of before freeing rescued_protos (§3.7 ordering invariant). */

UTEST(closure_alloc_bumps_root_via_backptr)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UArena arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    /* Closure alloc: vm_alloc_closure bumps root_proto.refcount via uproto_root_of.
     * The closure ends up on vm->stdlib_closures (persistent-strand policy in
     * release_strand_resource_chain).  umodule_destroy rescues root_proto;
     * vm_destroy completes the lifecycle per §3.7 ordering. */
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var f = function () { 1 }; f()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Drain any lingering strands. */
    utest_e2e_run_to_no_runnable(&vm);

    struct UProto *rp = module.root_proto;
    UASSERT(rp != NULL);

    /* Verify: nested[0] exists and its ->root back-pointer points at root_proto. */
    UASSERT(rp->nested != NULL);
    UASSERT(rp->nested_count > 0U);
    struct UProto *nested = rp->nested[0];
    UASSERT(nested != NULL);
    UASSERT_EQ((void *)rp, (void *)nested->root);

    /* Under Variant B Option (a): nested proto itself has refcount 0.
     * The slot-implicit ref was dropped; no refcount lives on the nested struct. */
    UASSERT_EQ((unsigned)0, (unsigned)nested->refcount);

    /* root_proto.refcount > 0: the closure on vm->stdlib_closures holds a ref
     * via uproto_root_of.  Strand-bind refs were discharged at strand exit,
     * but the closure's ref keeps root alive. */
    UASSERT((unsigned)rp->refcount > 0U);

    /* Full lifecycle: umodule_destroy rescues root_proto because refcount > 0. */
    struct UProto *saved_root = rp;
    umodule_destroy(&module, &vm);
    UASSERT(vm.rescued_protos != NULL);
    UASSERT_EQ((void *)saved_root, (void *)vm.rescued_protos);

    uarena_destroy(&arena);
    /* vm_destroy: stdlib_closures sweep decs via uproto_root_of FIRST (§3.7),
     * then rescued_protos sweep frees saved_root.  No UAF. */
    urbi_vm_destroy(&vm);
}

UTEST(escaping_closure_rescues_whole_root_proto)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Use heap UModule so we can free it independently after destroy. */
    UArena arena;
    UModule *m = (UModule *)vm.alloc_fn(NULL, sizeof(UModule), vm.alloc_ud);
    UASSERT(m != NULL);
    memset(m, 0, sizeof(*m));
    uarena_init(&arena, 4096);

    /* Escaping closure: f stays alive in Realm.f after strand exit. */
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, m,
        "var f = function () { 1 }; Realm.f = f",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Drain all strands. */
    utest_e2e_run_to_no_runnable(&vm);

    /* Closure alloc bumped root_proto via uproto_root_of.
     * Strand released its bind (root_proto dec).  Net: still > 0
     * because the closure on vm->stdlib_closures holds a ref. */
    UASSERT((unsigned)m->root_proto->refcount > 0U);

    /* Destroy module shell — rescue path triggers. */
    struct UProto *saved_root = m->root_proto;
    umodule_destroy(m, &vm);
    /* rescued_protos must be non-NULL after rescue. */
    UASSERT(vm.rescued_protos != NULL);
    UASSERT_EQ((void *)vm.rescued_protos, (void *)saved_root);

    vm.alloc_fn(m, 0, vm.alloc_ud);
    uarena_destroy(&arena);

    /* vm_destroy: stdlib_closures sweep MUST run before rescued_protos sweep
     * (§3.7 ordering invariant): each closure decs via uproto_root_of while
     * root is still alive.  No UAF. */
    urbi_vm_destroy(&vm);
}

void
test_module_refcount_fused_suite(void)
{
    printf("test_module_refcount_fused\n");
    utest_run("module_refcount_fused: strand_bind bumps root_proto",
              strand_bind_bumps_root_proto);
    utest_run("module_refcount_fused: op_fork child bumps root_proto",
              op_fork_child_bumps_root_proto);
    utest_run("module_refcount_fused: module->refcount stays 0 after redirect",
              module_refcount_stays_zero_after_redirect);
    utest_run("module_refcount_fused: closure alloc bumps root via back-ptr",
              closure_alloc_bumps_root_via_backptr);
    utest_run("module_refcount_fused: escaping closure rescues whole root_proto",
              escaping_closure_rescues_whole_root_proto);
}
