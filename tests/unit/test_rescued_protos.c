/* SPDX-License-Identifier: BSD-3-Clause */
/* test_rescued_protos — Phase 2 Task 9 of v0.8.1-uproto-root (re-ordered
 * to land before Task 8 per controller plan revision).
 *
 * Verifies the whole-root_proto rescue path: when umodule_destroy fires
 * while root_proto->refcount > 0 (strand still bound), the root_proto is
 * moved to vm->rescued_protos and freed cleanly at vm_destroy (no ASan
 * leaks, no double-frees).
 *
 * Coexists with the per-nested rescue path (vm->stdlib_protos); both run
 * independently.  Task 10 removes the per-nested path once Task 8's
 * closure-refcount fusion lands and makes whole-root_proto rescue
 * self-sufficient.
 */

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
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* Compile `src` into *out_mod.  Returns true on success.
 * Does NOT run the chunk — caller inspects module fields directly. */
static bool
rp_compile_chunk(UVM *vm, UArena *arena, UModule *out_mod, const char *src)
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

/* ---- test cases ---- */

/* Case 1: when root_proto->refcount > 0 (strand still bound), umodule_destroy
 * must rescue root_proto to vm->rescued_protos rather than freeing it.
 * The strand is then destroyed (dec refcount), and vm_destroy frees
 * rescued_protos cleanly. */
UTEST(whole_root_proto_rescue_when_refcount_nonzero)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UArena arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    /* Compile a chunk with a nested proto so the rescue path exercises
     * nested[] ownership transfer. */
    UASSERT(rp_compile_chunk(&vm, &arena, &module, "var f = function () { 1 };"));
    UASSERT(module.root_proto != NULL);

    /* Bind a strand — this bumps root_proto->refcount to 1. */
    UStrand *s = urbi_strand_create_for_module(&vm, realm, &module);
    UASSERT(s != NULL);
    UASSERT((unsigned)module.root_proto->refcount > (unsigned)0);

    /* Save root_proto pointer so we can verify it lands on rescued_protos. */
    UProto *saved_rp = module.root_proto;

    /* Destroy the module while the strand is still alive.
     * Should rescue root_proto, NOT free it. */
    umodule_destroy(&module, &vm);

    /* rescued_protos must be non-NULL and point to the rescued root_proto. */
    UASSERT(vm.rescued_protos != NULL);
    UASSERT(vm.rescued_protos == saved_rp);

    /* Destroy the strand — decrements root_proto->refcount back toward 0. */
    urbi_strand_destroy(s);

    /* vm_destroy must free rescued_protos cleanly (no leaks, no double-free). */
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* Case 2: when root_proto->refcount == 0, umodule_destroy must NOT rescue
 * anything — the normal free path runs and vm->rescued_protos stays NULL. */
UTEST(no_rescue_when_refcount_zero)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    /* Compile but do NOT create a strand → refcount stays at 0. */
    UASSERT(rp_compile_chunk(&vm, &arena, &module, "1 + 2;"));
    UASSERT(module.root_proto != NULL);
    UASSERT_EQ((unsigned)0, (unsigned)module.root_proto->refcount);

    /* Destroy module — no strand bound, refcount is 0, normal path runs. */
    umodule_destroy(&module, &vm);

    /* Nothing should have been rescued. */
    UASSERT(vm.rescued_protos == NULL);

    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* Case 3: two modules — one rescues, one does not.  Verify rescued_protos
 * list contains exactly the rescued entry and vm_destroy cleans up both. */
UTEST(two_modules_one_rescued_one_normal)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UArena arena;
    uarena_init(&arena, 4096);

    /* Module A — will be rescued (strand alive at destroy time). */
    UModule ma = {0};
    UASSERT(rp_compile_chunk(&vm, &arena, &ma, "var g = function () { 2 };"));
    UASSERT(ma.root_proto != NULL);
    UStrand *sa = urbi_strand_create_for_module(&vm, realm, &ma);
    UASSERT(sa != NULL);
    UProto *saved_rp_a = ma.root_proto;

    /* Module B — will NOT be rescued (no strand). */
    UModule mb = {0};
    UASSERT(rp_compile_chunk(&vm, &arena, &mb, "3 + 4;"));
    UASSERT(mb.root_proto != NULL);
    UASSERT_EQ((unsigned)0, (unsigned)mb.root_proto->refcount);

    /* Destroy B first (normal path, no rescue). */
    umodule_destroy(&mb, &vm);
    UASSERT(vm.rescued_protos == NULL);

    /* Destroy A while strand alive (rescue path). */
    umodule_destroy(&ma, &vm);
    UASSERT(vm.rescued_protos != NULL);
    UASSERT(vm.rescued_protos == saved_rp_a);

    /* Clean up the strand. */
    urbi_strand_destroy(sa);

    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

void
test_rescued_protos_suite(void)
{
    printf("test_rescued_protos\n");
    utest_run("rescued_protos: whole-root_proto rescue when refcount nonzero",
              whole_root_proto_rescue_when_refcount_nonzero);
    utest_run("rescued_protos: no rescue when refcount zero",
              no_rescue_when_refcount_zero);
    utest_run("rescued_protos: two modules, one rescued one normal",
              two_modules_one_rescued_one_normal);
}
