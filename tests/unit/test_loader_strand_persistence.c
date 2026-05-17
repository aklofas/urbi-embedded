/* SPDX-License-Identifier: BSD-3-Clause */
/* test_loader_strand_persistence — v0.8.0 persistent loader strand tests.
 *
 * Replaces the v0.7.x transient-strand model: urbi_run_chunk allocates
 * a real scheduler-managed strand, drives via internal urbi_step iterations
 * until park-or-die, then returns.  Strand persists in realm->strands_head;
 * subsequent host urbi_step advances it normally.
 *
 * Phase 1 (this file): the urbi_strand_create_for_module helper directly.
 * Phase 2 (Task 8): full end-to-end via urbi_run_chunk after the restructure. */

#include "utest.h"

#include "urbi/urbi.h"
#include "module/umodule.h"
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

/* Compile `src` into *out_mod.  Returns true on success.
 * Mirrors the fork_compile pattern from test_fork.c (file-static helper). */
static bool
compile_chunk(UVM *vm, UArena *arena, UModule *out_mod, const char *src)
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

/* Task 6: the helper creates a non-transient strand bound to the module,
 * bumps refcount, returns DORMANT→READY (via urbi_strand_start). */
UTEST(strand_create_for_module_returns_non_transient)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UArena arena;
    UModule module = {0};
    uarena_init(&arena, 4096);
    UASSERT(compile_chunk(&vm, &arena, &module, "42"));

    UStrand *s = urbi_strand_create_for_module(&vm, realm, &module);
    UASSERT(s != NULL);
    UASSERT_EQ((unsigned)USTRAND_STATE_READY, (unsigned)USTRAND_GET_STATE(s));
    UASSERT_EQ(0U, (unsigned)s->is_transient_strand);  /* NOT transient */
    UASSERT(s->module == &module);
    UASSERT_EQ((unsigned)1, (unsigned)module.refcount);

    urbi_strand_destroy(s);  /* tears down + drops refcount */
    UASSERT_EQ((unsigned)0, (unsigned)module.refcount);

    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

void test_loader_strand_persistence_suite(void)
{
    utest_run("loader_strand: create_for_module returns non-transient",
              strand_create_for_module_returns_non_transient);
}
