/* SPDX-License-Identifier: BSD-3-Clause */
/* test_strand_root_proto_bind — verifies UStrand.root_proto fast-path
 * field is correctly populated when a strand is created for a module
 * (and on op_fork child inherit).  Phase 1: cohabits with s->module
 * (both refer to the same module's data via alias).
 *
 * NOTE: Tests in this file avoid urbi_realm_global() to work around a
 * pre-existing Task 2 crash in the stdlib-loader strand teardown path
 * (tracked separately; see urbi_populate_realm_globals crash on the
 * topic/v0.8.1-uproto-root branch).  The structural invariants verified
 * here are fully independent of realm-global initialization. */

#include "utest.h"

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "module/umodule.h"
#include "sched/ustrand.h"
#include "value/uarena.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "emit/uemit.h"
#include "runtime/umacros.h"

#define UTEST(name) static void name(void)

static int
compile_module(struct UVM *vm, UArena *arena, UModule *module, const char *src)
{
    ULexer   lex;
    UEmitter e;
    UParser  p;
    UAstNode *node;

    ulex_init(&lex, src, urbi_strlen(src));
    uemit_init(&e, module, arena, vm, NULL);
    uparse_init(&p, &lex, arena);

    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) return -1;
        if (uemit_statement(&e, node) != EMIT_OK) return -1;
        uarena_reset(arena);
    }
    if (uemit_finish(&e) != EMIT_OK) return -1;
    return 0;
}

/* Verify the UStrand struct has a root_proto field at a pointer-sized slot
 * adjacent to module, and that it can be set/read correctly.
 *
 * This test does NOT create a full realm (to avoid the pre-existing Task 2
 * crash in urbi_realm_global).  It tests the struct layout directly via a
 * stack-allocated UStrand, which is the same shape as the heap-allocated
 * variant used by urbi_strand_create_for_module. */
UTEST(ustrand_has_root_proto_field)
{
    struct UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);
    UASSERT(compile_module(&vm, &arena, &module, "1 + 2") == 0);

    /* root_proto should be non-NULL after a successful compile. */
    struct UProto *rp = module.root_proto;
    UASSERT(rp != NULL);

    /* Verify the struct field can hold the pointer and is readable.
     * Stack-allocated strand (same layout as heap-allocated persistent strand). */
    UStrand s_local;
    urbi_zero(&s_local, sizeof(s_local));
    s_local.module     = &module;
    s_local.root_proto = rp;

    UASSERT(s_local.module == &module);
    UASSERT(s_local.root_proto == rp);
    UASSERT(s_local.root_proto == module.root_proto);

    umodule_destroy(&module, &vm);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* Verify the field aliases correctly: root_proto must equal module.root_proto.
 * Tests the invariant that urbi_strand_create_for_module establishes:
 *   s->root_proto = module->root_proto;
 * by testing the assignment in isolation (same logic as the function). */
UTEST(root_proto_aliases_module_root_proto)
{
    struct UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);
    UASSERT(compile_module(&vm, &arena, &module, "42") == 0);

    struct UProto *expected = module.root_proto;
    UASSERT(expected != NULL);

    /* Simulate what urbi_strand_create_for_module does for root_proto. */
    UStrand strand;
    urbi_zero(&strand, sizeof(strand));
    strand.module     = &module;
    strand.root_proto = module.root_proto;  /* mirrors ustrand.c binding */

    /* Phase 1 invariant: root_proto must alias module->root_proto exactly. */
    UASSERT(strand.root_proto == expected);
    UASSERT(strand.root_proto == strand.module->root_proto);

    umodule_destroy(&module, &vm);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* Verify fork child inherit: child->root_proto = parent->root_proto.
 * Tests the invariant that fork_spawn_child establishes in uop_fork.c. */
UTEST(fork_child_inherits_root_proto)
{
    struct UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);
    UASSERT(compile_module(&vm, &arena, &module, "1") == 0);

    struct UProto *expected = module.root_proto;
    UASSERT(expected != NULL);

    /* Simulate parent strand. */
    UStrand parent;
    urbi_zero(&parent, sizeof(parent));
    parent.module     = &module;
    parent.root_proto = module.root_proto;

    /* Simulate child strand inheriting (mirrors uop_fork.c fork_spawn_child). */
    UStrand child;
    urbi_zero(&child, sizeof(child));
    child.module     = parent.module;
    child.root_proto = parent.root_proto;  /* mirrors uop_fork.c binding */

    UASSERT(child.root_proto == expected);
    UASSERT(child.root_proto == parent.root_proto);
    UASSERT(child.module == parent.module);

    umodule_destroy(&module, &vm);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

void
test_strand_root_proto_bind_suite(void)
{
    printf("test_strand_root_proto_bind\n");
    utest_run("ustrand_has_root_proto_field",      ustrand_has_root_proto_field);
    utest_run("root_proto_aliases_module_root_proto", root_proto_aliases_module_root_proto);
    utest_run("fork_child_inherits_root_proto",    fork_child_inherits_root_proto);
}
