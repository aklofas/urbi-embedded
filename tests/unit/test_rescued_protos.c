/* SPDX-License-Identifier: BSD-3-Clause */
/* test_rescued_protos — Phase 2 Task 9 of v0.8.1-uproto-root (re-ordered
 * to land before Task 8 per controller plan revision).
 *
 * Verifies the whole-root_proto rescue path: when uchunk_destroy fires
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
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* Compile `src` into *out_mod.  Returns true on success.
 * Does NOT run the chunk — caller inspects module fields directly.
 * Owns its own arena so callers need not manage one. */
static bool
rp_compile_chunk(UVM *vm, UProto *out_mod, const char *src)
{
    UArena arena;
    uarena_init(&arena, 4096);

    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UEmitter e;
    uemit_init(&e, out_mod, &arena, vm, NULL);

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
    return ok;
}

/* ---- test cases ---- */

/* Case 1: when root_proto->refcount > 0 (strand still bound), uchunk_destroy
 * must rescue root_proto to vm->rescued_protos rather than freeing it.
 * The strand is then destroyed (dec refcount), and vm_destroy frees
 * rescued_protos cleanly. */
UTEST(whole_root_proto_rescue_when_refcount_nonzero)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Use heap-allocated module (heap_allocated=true) so that vm_destroy's
     * rescued_protos sweep can safely free it via alloc_fn. */
    UProto *module = (UProto *)vm.alloc_fn(NULL, sizeof(UProto), vm.alloc_ud);
    UASSERT(module != NULL);
    memset(module, 0, sizeof(*module));
    module->heap_allocated = true;
    module->alloc_fn = vm.alloc_fn;
    module->alloc_ud = vm.alloc_ud;

    /* Compile a chunk with a nested proto so the rescue path exercises
     * nested[] ownership transfer. */
    UASSERT(rp_compile_chunk(&vm, module, "var f = function () { 1 };"));

    /* Bind a strand — this bumps root->refcount to 1. */
    UStrand *s = urbi_strand_create_for_module(&vm, realm, module);
    UASSERT(s != NULL);
    UASSERT((unsigned)module->refcount > (unsigned)0);

    /* Destroy the module while the strand is still alive.
     * Should rescue the root UProto, NOT free it. */
    uchunk_destroy(module, &vm);

    /* rescued_protos must be non-NULL and point to the rescued root. */
    UASSERT(vm.rescued_protos != NULL);
    UASSERT(vm.rescued_protos == module);

    /* Destroy the strand — decrements root->refcount back toward 0. */
    urbi_strand_destroy(&vm, s);

    /* vm_destroy must free rescued_protos cleanly (no leaks, no double-free). */
    urbi_vm_destroy(&vm);
}

/* Case 2: when root_proto->refcount == 0, uchunk_destroy must NOT rescue
 * anything — the normal free path runs and vm->rescued_protos stays NULL. */
UTEST(no_rescue_when_refcount_zero)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UProto module = {0};

    /* Compile but do NOT create a strand → refcount stays at 0. */
    UASSERT(rp_compile_chunk(&vm, &module, "1 + 2;"));
    UASSERT_EQ((unsigned)0, (unsigned)module.refcount);

    /* Destroy module — no strand bound, refcount is 0, normal path runs. */
    uchunk_destroy(&module, &vm);

    /* Nothing should have been rescued. */
    UASSERT(vm.rescued_protos == NULL);

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

    /* Module A — will be rescued (strand alive at destroy time).
     * Must be heap-allocated so vm_destroy's rescued_protos sweep can free it. */
    UProto *ma = (UProto *)vm.alloc_fn(NULL, sizeof(UProto), vm.alloc_ud);
    UASSERT(ma != NULL);
    memset(ma, 0, sizeof(*ma));
    ma->heap_allocated = true;
    ma->alloc_fn = vm.alloc_fn;
    ma->alloc_ud = vm.alloc_ud;
    UASSERT(rp_compile_chunk(&vm, ma, "var g = function () { 2 };"));
    UStrand *sa = urbi_strand_create_for_module(&vm, realm, ma);
    UASSERT(sa != NULL);

    /* Module B — will NOT be rescued (no strand). Stack-allocated is fine. */
    UProto mb = {0};
    UASSERT(rp_compile_chunk(&vm, &mb, "3 + 4;"));
    UASSERT_EQ((unsigned)0, (unsigned)mb.refcount);

    /* Destroy B first (normal path, no rescue). */
    uchunk_destroy(&mb, &vm);
    UASSERT(vm.rescued_protos == NULL);

    /* Destroy A while strand alive (rescue path). */
    uchunk_destroy(ma, &vm);
    UASSERT(vm.rescued_protos != NULL);
    UASSERT(vm.rescued_protos == ma);

    /* Clean up the strand. */
    urbi_strand_destroy(&vm, sa);

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
