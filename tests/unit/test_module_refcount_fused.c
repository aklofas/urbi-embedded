/* SPDX-License-Identifier: BSD-3-Clause */
/* test_module_refcount_fused — Phase 2 of v0.8.1-uproto-root.
 *
 * Verifies that strand binds bump root_proto->refcount (NOT module->refcount).
 * Variant B fusion: one canonical counter on root_proto accumulates both
 * strand-bind refs (this task, T7) and closure-alloc refs (Task 8).
 *
 * These tests FAIL before the T7 redirect lands; they pass after. */

#include "utest.h"

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

/* Case 2: after running `1 & 2` to completion, root_proto->refcount is 0.
 * OP_FORK spawns a child strand that also bumps root_proto->refcount;
 * both parent and child must discharge on exit. */
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

    /* After execution: all strand binds have been released.
     * root_proto->refcount must be back to 0. */
    UASSERT_EQ((unsigned)0, (unsigned)rp->refcount);

    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);
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
}
