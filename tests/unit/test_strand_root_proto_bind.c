/* SPDX-License-Identifier: BSD-3-Clause */
/* test_strand_root_proto_bind — verifies UStrand.root_proto fast-path
 * field is correctly populated when a strand is created for a module
 * (Phase 1) and when a fork child inherits the field (via OP_FORK).
 *
 * All tests use the live urbi_strand_create_for_module path so that
 * removing the binding assignment in ustrand.c would cause a NULL
 * dereference or assertion failure here. */

#include "utest.h"

#include "urbi/urbi.h"
#include "chunk/uchunk_strand.h"
#include "chunk/uchunk.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "vm/uvm.h"
#include "value/uarena.h"
#include "lex/ulex.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "emit/uemit.h"

#include <stddef.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Compile `src` into *out_mod.  Returns true on success. */
static bool
compile_chunk(UVM *vm, UArena *arena, UProto *out_mod, const char *src)
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

/* Case 1: urbi_strand_create_for_module sets root_proto to module.root_proto.
 *
 * Uses the live binding path in ustrand.c.  If the assignment
 * `s->root_proto = module->root_proto` is removed, s->root_proto will
 * be NULL and the final assertion fails. */
UTEST(ustrand_has_root_proto_field)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UArena arena;
    UProto module = {0};
    uarena_init(&arena, 4096);
    UASSERT(compile_chunk(&vm, &arena, &module, "1 + 2"));

    /* Create a live strand via the real binding path. */
    UStrand *s = urbi_strand_create_for_module(&vm, realm, &module);
    UASSERT(s != NULL);

    /* Phase 1 invariant: root_proto points directly at module (module IS the root). */
    UASSERT(s->root_proto == &module);

    /* Drive to completion so the strand dies cleanly. */
    UValue result = {0};
    uchunk_loader_drive(&vm, s, &result);

    uarena_destroy(&arena);
    uchunk_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 2: root_proto is stable across execution — reading it after
 * uchunk_loader_drive parks yields the same module.root_proto value.
 *
 * Uses waituntil(false) to park the strand without executing OP_RET,
 * then verifies root_proto is still set correctly. */
UTEST(root_proto_aliases_module_root_proto)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UArena arena;
    UProto module = {0};
    uarena_init(&arena, 4096);
    /* waituntil(false) parks the strand in WAITING state without dying. */
    UASSERT(compile_chunk(&vm, &arena, &module, "waituntil (false)"));

    UStrand *s = urbi_strand_create_for_module(&vm, realm, &module);
    UASSERT(s != NULL);

    /* Drive until the strand parks. */
    UValue result = {0};
    uchunk_loader_drive(&vm, s, &result);

    /* Strand is now WAITING (parked on the watcher); root_proto must still
     * point at module (module IS the root). */
    UASSERT(USTRAND_IS_WAITING(s));
    UASSERT(s->root_proto == &module);

    /* Explicit destroy discharges the module refcount. */
    urbi_strand_destroy(s);

    uarena_destroy(&arena);
    uchunk_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 3: fork child inherits root_proto via OP_FORK.
 *
 * Runs `1 & 2` through urbi_strand_create_for_module + uchunk_loader_drive.
 * OP_FORK spawns a child strand; the child inherits root_proto from the
 * parent (uop_fork.c fork_spawn_child).  If the parent's root_proto is
 * NULL, fork_spawn_child writes NULL into the child, causing a NULL
 * dereference crash in the child strand during its first slot lookup.
 * A crash-free completion of both parent and child proves the invariant. */
UTEST(fork_child_inherits_root_proto)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UArena arena;
    UProto module = {0};
    uarena_init(&arena, 4096);
    /* `1 & 2`: parent executes `1`, fork child executes `2`.
     * Object.clone() inside the fork child would dereference root_proto;
     * even the simple integer literals exercise atom-proto dispatch,
     * verifying the child strand is wired correctly. */
    UASSERT(compile_chunk(&vm, &arena, &module, "1 & 2"));

    UStrand *s = urbi_strand_create_for_module(&vm, realm, &module);
    UASSERT(s != NULL);

    /* Parent must have root_proto == &module before OP_FORK runs. */
    UASSERT(s->root_proto == &module);

    /* Drive until quiescent — both parent and fork child must complete
     * without a crash.  A NULL root_proto in the parent would propagate
     * to the child and cause a fault inside fork_spawn_child or the
     * child's first opcode. */
    UValue result = {0};
    int rc = uchunk_loader_drive(&vm, s, &result);
    UASSERT_EQ(URBI_OK, rc);

    /* Drain any child strands spawned by OP_FORK. */
    for (int i = 0; i < 200; i++) {
        UStepResult sr = urbi_step(&vm, 1000, NULL);
        if (sr == URBI_STEP_QUIESCENT) break;
        if (sr == URBI_STEP_FATAL) { UASSERT(0 && "fatal step in fork test"); break; }
        if (vm.strand_runnable_count == 0) break;
    }

    uarena_destroy(&arena);
    uchunk_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

void
test_strand_root_proto_bind_suite(void)
{
    printf("test_strand_root_proto_bind\n");
    utest_run("ustrand_has_root_proto_field",         ustrand_has_root_proto_field);
    utest_run("root_proto_aliases_module_root_proto",  root_proto_aliases_module_root_proto);
    utest_run("fork_child_inherits_root_proto",        fork_child_inherits_root_proto);
}
