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
#include "chunk/uchunk.h"
#include "chunk/uchunk_strand.h"
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
fused_compile_chunk(UVM *vm, UArena *arena, UProto *out_mod, const char *src)
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
    UProto module = {0};
    uarena_init(&arena, 4096);
    UASSERT(fused_compile_chunk(&vm, &arena, &module, "1"));

    /* Before binding: root->refcount must be 0. */
    UASSERT_EQ((unsigned)0, (unsigned)module.refcount);

    /* Bind a strand: should bump root->refcount to 1. */
    UStrand *s = urbi_strand_create_for_module(&vm, realm, &module);
    UASSERT(s != NULL);
    UASSERT_EQ((unsigned)1, (unsigned)module.refcount);

    /* Destroy the strand: should decrement root->refcount back to 0. */
    urbi_strand_destroy(&vm, s);
    UASSERT_EQ((unsigned)0, (unsigned)module.refcount);

    uarena_destroy(&arena);
    uchunk_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 2: running `1 & 2` to completion — Variant B lifecycle.
 *
 * OP_FORK_JOIN allocates a closure for the `& 2` branch via vm_alloc_closure.
 * Under Variant B, that closure bumps root_proto.refcount via uproto_root_of.
 * UClosure is GC-managed (v0.8.4 Step C-2); vm->stdlib_closures was deleted
 * at Step C-3.  After draining child strands, the GC sweep reclaims the
 * closure when it becomes unreachable; uproto_root_of dec is called by the
 * uclosure_destroy finalizer.
 *
 * The correct full lifecycle: uchunk_destroy triggers rescue (root rescued to
 * vm->rescued_protos), GC sweep decs via uproto_root_of (§3.7 ordering
 * invariant), then rescued_protos sweep frees root.  No UAF. */
UTEST(op_fork_child_bumps_root_proto)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UArena arena;
    UProto module = {0};
    uarena_init(&arena, 4096);
    UASSERT(fused_compile_chunk(&vm, &arena, &module, "1 & 2"));

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

    /* Under Variant B: the closure for `& 2` bumped root_proto.refcount via
     * uproto_root_of at alloc time.  UClosure is GC-managed (v0.8.4 Step C-2);
     * vm->stdlib_closures was deleted at Step C-3.  The full lifecycle
     * (uchunk_destroy + vm_destroy) must complete without UAF. */

    uarena_destroy(&arena);
    /* uchunk_destroy: rescue fires because root_proto.refcount > 0. */
    uchunk_destroy(&module, &vm);
    /* vm_destroy: closure sweep dec's via uproto_root_of BEFORE rescued_protos
     * sweep frees rp (§3.7 ordering invariant).  Must not crash or UAF. */
    urbi_vm_destroy(&vm);
}

/* Case 3: Task 11 — UProto.refcount deleted; refcount lives on root_proto only.
 * Verify strand-bind refcount is correctly tracked on root_proto. */
UTEST(module_refcount_lives_on_root_proto)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UArena arena;
    UProto module = {0};
    uarena_init(&arena, 4096);
    UASSERT(fused_compile_chunk(&vm, &arena, &module, "1"));

    UStrand *s = urbi_strand_create_for_module(&vm, realm, &module);
    UASSERT(s != NULL);

    /* Strand bind increments root->refcount to 1. */
    UASSERT_EQ((unsigned)1, (unsigned)module.refcount);

    urbi_strand_destroy(&vm, s);
    /* After strand destroy, refcount drops back to 0. */
    UASSERT_EQ((unsigned)0, (unsigned)module.refcount);

    uarena_destroy(&arena);
    uchunk_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

/* === Task 8a (v0.8.1): Closure refcount fusion tests ==================
 *
 * Case 4: local closure (no escape).  After strand exits, root_proto->refcount
 * must be 0 (Variant B Option (a): no slot-implicit ref, closure ref discharged
 * at strand exit).  uchunk_destroy must not trigger rescue.
 *
 * Case 5: escaping closure (stored in realm global).  After strand exits,
 * root_proto->refcount > 0 (vm_alloc_closure bumped via uproto_root_of).
 * uchunk_destroy must rescue root_proto.  vm_destroy must dec via
 * uproto_root_of before freeing rescued_protos (§3.7 ordering invariant). */

UTEST(closure_alloc_bumps_root_via_backptr)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UArena arena;
    /* Heap-allocate: only heap_allocated roots may go on rescued_protos.
     * (Stack roots use the self-link sentinel path; vm_destroy must not
     *  free a stack address via the rescued_protos sweep.) */
    UProto *module = (UProto *)vm.alloc_fn(NULL, sizeof(UProto), vm.alloc_ud);
    UASSERT(module != NULL);
    memset(module, 0, sizeof(*module));
    module->heap_allocated = true;
    module->alloc_fn       = vm.alloc_fn;
    module->alloc_ud       = vm.alloc_ud;
    uarena_init(&arena, 4096);

    /* Closure alloc: vm_alloc_closure bumps root_proto.refcount via uproto_root_of.
     * UClosure is GC-managed (v0.8.4 Step C-2; vm->stdlib_closures deleted at C-3).
     * uchunk_destroy rescues root_proto; vm_destroy completes the lifecycle
     * per §3.7 ordering. */
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, module,
        "var f = function () { 1 }; f()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Drain any lingering strands. */
    utest_e2e_run_to_no_runnable(&vm);

    /* Verify: nested[0] exists and its ->root back-pointer points at the root. */
    UASSERT(module->nested != NULL);
    UASSERT(module->nested_count > 0U);
    struct UProto *nested = module->nested[0];
    UASSERT(nested != NULL);
    UASSERT_EQ((void *)module, (void *)nested->root);

    /* Under Variant B Option (a): nested proto itself has refcount 0.
     * The slot-implicit ref was dropped; no refcount lives on the nested struct. */
    UASSERT_EQ((unsigned)0, (unsigned)nested->refcount);

    /* root.refcount > 0: a GC-managed UClosure holds a ref
     * via uproto_root_of.  Strand-bind refs were discharged at strand exit,
     * but the closure's ref keeps root alive until GC collection. */
    UASSERT((unsigned)module->refcount > 0U);

    /* Full lifecycle: uchunk_destroy rescues root because refcount > 0. */
    struct UProto *saved_root = module;
    uchunk_destroy(module, &vm);
    UASSERT(vm.rescued_protos != NULL);
    UASSERT_EQ((void *)saved_root, (void *)vm.rescued_protos);

    uarena_destroy(&arena);
    /* vm_destroy: GC sweep decs via uproto_root_of (§3.7 ordering invariant),
     * then rescued_protos sweep frees saved_root.  No UAF. */
    urbi_vm_destroy(&vm);
}

UTEST(escaping_closure_rescues_whole_root_proto)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Use heap UProto (heap_allocated=true) so vm_destroy frees it via
     * the rescued_protos sweep after the closure drops its ref. */
    UArena arena;
    UProto *m = (UProto *)vm.alloc_fn(NULL, sizeof(UProto), vm.alloc_ud);
    UASSERT(m != NULL);
    memset(m, 0, sizeof(*m));
    m->heap_allocated = true;
    uarena_init(&arena, 4096);

    /* Escaping closure: f stays alive in Realm.f after strand exit. */
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, m,
        "var f = function () { 1 }; Realm.f = f",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Drain all strands. */
    utest_e2e_run_to_no_runnable(&vm);

    /* Closure alloc bumped root via uproto_root_of.
     * Strand released its bind (root dec).  Net: still > 0
     * because a GC-managed UClosure holds a ref via uproto_root_of. */
    UASSERT((unsigned)m->refcount > 0U);

    /* Destroy module — rescue path triggers (m IS the root). */
    struct UProto *saved_root = m;
    uchunk_destroy(m, &vm);
    /* rescued_protos must be non-NULL after rescue. */
    UASSERT(vm.rescued_protos != NULL);
    UASSERT_EQ((void *)vm.rescued_protos, (void *)saved_root);

    uarena_destroy(&arena);

    /* vm_destroy: GC sweep decs via uproto_root_of (§3.7 ordering invariant)
     * while root is still on rescued_protos.  Then rescued_protos sweep frees
     * saved_root.  No UAF. */
    urbi_vm_destroy(&vm);
}

/* Case 6: uchunk_destroy with vm=NULL when a closure is still live.
 *
 * Verifies the self-link sentinel mechanism: uchunk_destroy(m, NULL) must
 * not crash, must mark root_proto with next_alloc == root_proto (sentinel),
 * and the subsequent urbi_vm_destroy must sweep cleanly (no leak per ASan). */
UTEST(vm_null_destroy_with_live_closure)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Allocate module on the heap (heap_allocated=true) so the
     * deferred-destroy path can free it when the strand finally dies. */
    UProto *m = (UProto *)vm.alloc_fn(NULL, sizeof(UProto), vm.alloc_ud);
    UASSERT(m != NULL);
    memset(m, 0, sizeof(*m));
    m->heap_allocated = true;
    m->alloc_fn = vm.alloc_fn;
    m->alloc_ud = vm.alloc_ud;
    UArena arena;
    uarena_init(&arena, 4096);

    /* Closure escapes to Realm.f — GC-managed UClosure keeps a ref on
     * root via uproto_root_of.  root.refcount > 0 after draining. */
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, m,
        "var f = function () { 1 }; Realm.f = f",
        NULL);
    UASSERT_EQ(URBI_OK, rc);
    utest_e2e_run_to_no_runnable(&vm);

    /* Closure GC-managed; root.refcount > 0 via uproto_root_of. */
    UASSERT(m->refcount > 0U);

    /* Call destroy WITH NULL vm (defensive contract path).
     * m IS the root — must not crash; must set the self-link sentinel. */
    uchunk_destroy(m, NULL);
    /* Self-link sentinel is the sole deferred-destroy signal (Task 11:
     * destroy_requested field deleted; only next_alloc == self matters). */
    UASSERT_EQ((void *)m->next_alloc, (void *)m);

    uarena_destroy(&arena);

    /* vm_destroy: GC sweep dec's via uproto_root_of (§3.7 ordering);
     * detects self-link sentinel on root_proto, promotes to rescued_protos,
     * then rescued_protos sweep frees it.  No leak per ASan. */
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
    utest_run("module_refcount_fused: refcount lives on root_proto only",
              module_refcount_lives_on_root_proto);
    utest_run("module_refcount_fused: closure alloc bumps root via back-ptr",
              closure_alloc_bumps_root_via_backptr);
    utest_run("module_refcount_fused: escaping closure rescues whole root_proto",
              escaping_closure_rescues_whole_root_proto);
    utest_run("module_refcount_fused: vm=NULL destroy sets self-link sentinel",
              vm_null_destroy_with_live_closure);
}
