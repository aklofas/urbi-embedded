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
#include "chunk/uchunk_strand.h"
#include "chunk/umodule.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "vm/uvm.h"
#include "value/uarena.h"
#include "lex/ulex.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "emit/uemit.h"
#include "utest_e2e_helpers.h"

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

/* Task 7: drive a freshly-created loader strand to completion. */
UTEST(loader_drive_completes_trivial_chunk)
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

    UValue result = {0};
    int rc = uchunk_loader_drive(&vm, s, &result);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)result.kind);
    UASSERT_EQ((int64_t)42, result.v.i);

    /* Strand died; refcount discharged via ustrand_destroy. */
    /* Note: depending on whether the scheduler's quiescence sweep
     * has run, the strand may still exist as a DEAD pool entry.
     * The refcount discharge happens at strand_destroy time. */

    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

/* Task 7: loader strand parks on a waituntil whose cond starts false.
 *
 * waituntil (false) installs a watcher that is never satisfied; the strand
 * parks with USTRAND_WAIT_WATCHER (USTRAND_WAITING | USTRAND_REASON_WATCHER).
 * The driver must return URBI_OK with out_result = nil and leave the strand
 * alive in realm->strands_head.  sleep() is not available as a registered
 * builtin at this milestone; waituntil(false) is the canonical park primitive. */
UTEST(loader_drive_parks_on_sleep)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);

    UArena arena;
    UModule module = {0};
    uarena_init(&arena, 4096);
    /* waituntil (false) parks with USTRAND_WAIT_WATCHER; cond starts false
     * so the watcher installs and the strand remains WAITING. */
    UASSERT(compile_chunk(&vm, &arena, &module,
        "waituntil (false)"));

    UStrand *s = urbi_strand_create_for_module(&vm, realm, &module);
    UASSERT(s != NULL);

    UValue result = {0};
    int rc = uchunk_loader_drive(&vm, s, &result);
    UASSERT_EQ(URBI_OK, rc);
    /* Strand parked on watcher; result = nil. */
    UASSERT_EQ((int)UVAL_NIL, (int)result.kind);
    /* Strand state is parked (WAITING upper nibble), not DEAD and not RUNNING. */
    UASSERT(USTRAND_IS_WAITING(s));
    /* root_proto->refcount > 0 — strand-bind still live. */
    UASSERT(module.root_proto != NULL);
    UASSERT((unsigned)module.root_proto->refcount > 0);

    /* Cleanup: explicitly destroy the strand so the realm shutdown
     * isn't holding live work.  This discharges the module refcount. */
    urbi_strand_destroy(s);

    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

/* Task 8: urbi_run_chunk + persistent loader strand makes chunk-top `&` work. */
UTEST(run_chunk_chunktop_amp_works)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);

    UArena arena;
    UModule module = {0};
    uarena_init(&arena, 4096);
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var a = 0; var b = 0; Realm.a = 1 & Realm.b = 2", NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Drain any leftover work. */
    for (int i = 0; i < 100; i++) {
        UStepResult r = urbi_step(&vm, 1000, NULL);
        if (r == URBI_STEP_QUIESCENT) break;
    }

    UValue a = {0}, b = {0};
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, realm, "a", 1, &a));
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, realm, "b", 1, &b));
    UASSERT_EQ((int64_t)1, a.v.i);
    UASSERT_EQ((int64_t)2, b.v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

/* Task 8: chunk-top `,` (fork-detach) works.
 * Both sides of `,` execute as fork-detach children.  Realm.a and Realm.b
 * are set by the two parallel branches; after driving to quiescent both
 * must have their assigned values. */
UTEST(run_chunk_chunktop_comma_works)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);

    UArena arena;
    UModule module = {0};
    uarena_init(&arena, 4096);
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "Realm.a = 99 , Realm.b = 100", NULL);
    UASSERT_EQ(URBI_OK, rc);
    for (int i = 0; i < 100; i++) {
        UStepResult r = urbi_step(&vm, 1000, NULL);
        if (r == URBI_STEP_QUIESCENT) break;
    }

    UValue a = {0};
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, realm, "a", 1, &a));
    UASSERT_EQ((int64_t)99, a.v.i);

    UValue b = {0};
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, realm, "b", 1, &b));
    UASSERT_EQ((int64_t)100, b.v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

/* Task 8: function call from chunk-top with internal fork works. */
UTEST(run_chunk_chain_call_forks)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);

    UArena arena;
    UModule module = {0};
    uarena_init(&arena, 4096);
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var a = 0; var b = 0;"
        "var f = function () {"
        "  Realm.a = 7 , Realm.b = 8"
        "};"
        "f()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);
    for (int i = 0; i < 100; i++) {
        UStepResult r = urbi_step(&vm, 1000, NULL);
        if (r == URBI_STEP_QUIESCENT) break;
    }

    UValue a = {0}, b = {0};
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, realm, "a", 1, &a));
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, realm, "b", 1, &b));
    UASSERT_EQ((int64_t)7, a.v.i);
    UASSERT_EQ((int64_t)8, b.v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

/* Task 8: loader strand parks on chunk-top waituntil (waituntil(false)
 * is one of the few park triggers available pre-Task-9; sleep() is not
 * a registered global at this milestone). */
UTEST(run_chunk_parks_on_waituntil)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *realm = urbi_realm_global(&vm);

    UArena arena;
    UModule module = {0};
    uarena_init(&arena, 4096);
    /* Set a sentinel before the park to verify side effects up to the
     * park happened, then waituntil(false) parks forever. */
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var x = 0; Realm.x = 42; waituntil (false)", NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Realm.x should be 42 (side effect ran before the park). */
    UValue x = {0};
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, realm, "x", 1, &x));
    UASSERT_EQ((int64_t)42, x.v.i);

    /* root_proto->refcount > 0 (loader strand parked, still bound). */
    UASSERT(module.root_proto != NULL);
    UASSERT((unsigned)module.root_proto->refcount > 0);

    /* Note: we cannot let urbi_vm_destroy run with a parked strand AND
     * a heap-allocated module, because the parked strand keeps the
     * module ref alive past umodule_destroy.  For this test we accept
     * the parked-forever strand will be torn down by urbi_vm_destroy
     * (which kills all realm strands first → drops bindings →
     * refcount → 0 → immediate free). */

    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
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
    /* v0.8.1 Phase 2: strand-bind refcount is on root_proto, not module. */
    UASSERT(module.root_proto != NULL);
    UASSERT_EQ((unsigned)1, (unsigned)module.root_proto->refcount);

    urbi_strand_destroy(s);  /* tears down + drops root_proto refcount */
    UASSERT_EQ((unsigned)0, (unsigned)module.root_proto->refcount);

    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

void test_loader_strand_persistence_suite(void)
{
    utest_run("loader_strand: drive completes trivial chunk",
              loader_drive_completes_trivial_chunk);
    utest_run("loader_strand: drive parks on waituntil(false)",
              loader_drive_parks_on_sleep);
    utest_run("loader_strand: create_for_module returns non-transient",
              strand_create_for_module_returns_non_transient);
    utest_run("loader_strand: run_chunk chunktop `&` works",
              run_chunk_chunktop_amp_works);
    utest_run("loader_strand: run_chunk chunktop `,` works",
              run_chunk_chunktop_comma_works);
    utest_run("loader_strand: run_chunk chain-call forks",
              run_chunk_chain_call_forks);
    utest_run("loader_strand: run_chunk parks on waituntil",
              run_chunk_parks_on_waituntil);
}
