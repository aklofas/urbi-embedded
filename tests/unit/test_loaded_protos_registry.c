/* SPDX-License-Identifier: BSD-3-Clause */
/* Verify modules are registered in URealm.loaded_protos_head at
 * urbi_run_chunk entry and that module->owning_realm is set.
 * v0.9.0-repl Task 5. */

#include "utest.h"

#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include "urbi/urbi.h"
#include "module/umodule.h"
#include "realm/urealm.h"
#include "value/uarena.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "emit/uemit.h"
#include "vm/uvm.h"
#include "runtime/umacros.h"   /* urbi_zero */

#define UTEST(name) static void name(void)

/* Compile source using the standard emit pipeline. */
static int compile_src(const char *src, UVM *vm, UModule *m, UArena *arena)
{
    ULexer  lex;
    UParser p;
    UEmitter e;
    UAstNode *node;

    ulex_init(&lex, src, strlen(src));
    uemit_init(&e, m, arena, vm, NULL);
    uparse_init(&p, &lex, arena);

    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) return -1;
        if (uemit_statement(&e, node) != EMIT_OK) return -1;
        uarena_reset(arena);
    }
    return (uemit_finish(&e) == EMIT_OK) ? 0 : -1;
}

/* -----------------------------------------------------------------------
 * Test 1: urbi_run_chunk registers module in realm->loaded_protos_head
 * ----------------------------------------------------------------------- */

UTEST(run_chunk_registers_module)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* Compile a simple expression. */
    UArena arena;
    uarena_init(&arena, 4096);
    UModule mod;
    urbi_zero(&mod, sizeof(mod));
    int rc = compile_src("1 + 2 |", &vm, &mod, &arena);
    UASSERT_EQ(0, rc);

    /* Run via urbi_run_chunk. */
    UValue out;
    urbi_zero(&out, sizeof(out));
    int run_rc = urbi_run_chunk(&vm, realm, &mod, &out);
    UASSERT_EQ(URBI_OK, run_rc);

    /* Post-run: module registered at head of realm's list. */
    UASSERT(realm->loaded_protos_head == &mod);
    UASSERT(mod.owning_realm == realm);

    uarena_destroy(&arena);
    umodule_destroy(&mod, &vm);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Test 2: second urbi_run_chunk with same module is a no-op (idempotent)
 * ----------------------------------------------------------------------- */

UTEST(run_chunk_register_idempotent)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UArena arena;
    uarena_init(&arena, 4096);
    UModule mod;
    urbi_zero(&mod, sizeof(mod));
    int rc = compile_src("1 + 2 |", &vm, &mod, &arena);
    UASSERT_EQ(0, rc);

    /* First run — registers. */
    UValue out;
    urbi_zero(&out, sizeof(out));
    int run_rc = urbi_run_chunk(&vm, realm, &mod, &out);
    UASSERT_EQ(URBI_OK, run_rc);
    UASSERT(realm->loaded_protos_head == &mod);
    /* Snapshot the next_in_realm after first registration. */
    UModule *next_after_first = mod.next_in_realm;

    /* Second run — must not double-link (idempotent). */
    urbi_zero(&out, sizeof(out));
    run_rc = urbi_run_chunk(&vm, realm, &mod, &out);
    UASSERT_EQ(URBI_OK, run_rc);
    /* Head still points to mod — not re-inserted at a new position. */
    UASSERT(realm->loaded_protos_head == &mod);
    /* next_in_realm unchanged — mod was not re-linked onto itself. */
    UASSERT(mod.next_in_realm == next_after_first);

    uarena_destroy(&arena);
    umodule_destroy(&mod, &vm);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Suite entry
 * ----------------------------------------------------------------------- */

void
test_loaded_protos_registry_suite(void)
{
    utest_run("loaded_protos_registry: run_chunk registers module in realm",
              run_chunk_registers_module);
    utest_run("loaded_protos_registry: run_chunk register is idempotent",
              run_chunk_register_idempotent);
}
