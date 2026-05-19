/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi_realm_destroy must walk loaded_protos_head and unload each
 * non-stdlib module after the existing strand+namespace+tag teardown.
 * v0.9.0-repl Task 12. */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "urbi/urbi.h"
#include "chunk/umodule.h"
#include "realm/urealm.h"
#include "value/uarena.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "emit/uemit.h"
#include "vm/uvm.h"
#include "runtime/umacros.h"   /* urbi_zero */

#define UTEST(name) static void name(void)

/* Compile and run a source string under the given realm.
 * Returns the urbi_run_chunk return code.
 * Each call uses its own arena so that oversized emitter chunks (UFuncState
 * is ~16 KB; default chunk_size is 4 KB) are freed on return and never
 * orphaned by a subsequent compile that reuses the same arena. */
static int compile_and_load(const char *src, UVM *vm, URealm *realm,
                             UModule *mod)
{
    UArena  arena;
    ULexer  lex;
    UParser p;
    UEmitter e;
    UAstNode *node;
    int rc;

    uarena_init(&arena, 4096);
    ulex_init(&lex, src, strlen(src));
    uemit_init(&e, mod, &arena, vm, NULL);
    uparse_init(&p, &lex, &arena);

    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) { uarena_destroy(&arena); return -1; }
        if (uemit_statement(&e, node) != EMIT_OK) { uarena_destroy(&arena); return -1; }
        uarena_reset(&arena);
    }
    if (uemit_finish(&e) != EMIT_OK) { uarena_destroy(&arena); return -1; }
    uarena_destroy(&arena);

    UValue out;
    urbi_zero(&out, sizeof(out));
    rc = urbi_run_chunk(vm, realm, mod, &out);
    return rc;
}

/* -----------------------------------------------------------------------
 * Test 1: urbi_realm_destroy walks loaded_protos_head and clears
 *         owning_realm on each non-stdlib module.
 * ----------------------------------------------------------------------- */

UTEST(realm_destroy_unloads_modules)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UModule mod1;
    UModule mod2;
    urbi_zero(&mod1, sizeof(mod1));
    urbi_zero(&mod2, sizeof(mod2));

    int rc1 = compile_and_load("1 |", &vm, r, &mod1);
    UASSERT_EQ(URBI_OK, rc1);
    UASSERT(mod1.owning_realm == r);

    int rc2 = compile_and_load("2 |", &vm, r, &mod2);
    UASSERT_EQ(URBI_OK, rc2);
    UASSERT(mod2.owning_realm == r);

    /* Both user modules are registered in this realm. */
    {
        int found1 = 0, found2 = 0;
        for (UModule *m = r->loaded_protos_head; m != NULL; m = m->next_in_realm) {
            if (m == &mod1) found1 = 1;
            if (m == &mod2) found2 = 1;
        }
        UASSERT(found1);
        UASSERT(found2);
    }

    /* Snapshot stdlib pointer before destroy. */
    UModule *stdlib = vm.stdlib_module;

    /* Destroy the realm — should unload both user modules, skipping stdlib. */
    urbi_realm_destroy(&vm, r);

    /* Both user module owning_realm fields must be NULL after destroy. */
    UASSERT(mod1.owning_realm == NULL);
    UASSERT(mod2.owning_realm == NULL);

    /* Stdlib must still be intact (not freed). */
    if (stdlib != NULL) {
        /* stdlib->owning_realm may be NULL now (back-pointer cleared) but the
         * module struct itself must not have been destroyed.  Verify by checking
         * the VM's stdlib pointer hasn't been zeroed. */
        UASSERT(vm.stdlib_module == stdlib);
    }

    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Test 2: destroying the first realm (which owns stdlib in loaded_protos_head)
 *         does not crash and leaves stdlib usable for the second realm.
 * ----------------------------------------------------------------------- */

UTEST(realm_destroy_stdlib_exclusion)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* Create two realms.  Stdlib is registered in realm A (the first created).
     * Realm B's loaded_protos_head contains only its own modules (stdlib
     * silently skips because owning_realm != NULL). */
    URealm *a = urbi_realm_create(&vm);
    URealm *b = urbi_realm_create(&vm);
    UASSERT(a != NULL);
    UASSERT(b != NULL);

    /* Run a trivial module under realm A. */
    UModule mod_a;
    urbi_zero(&mod_a, sizeof(mod_a));
    int rca = compile_and_load("3 |", &vm, a, &mod_a);
    UASSERT_EQ(URBI_OK, rca);
    UASSERT(mod_a.owning_realm == a);

    UModule *stdlib = vm.stdlib_module;

    /* Destroy realm A.  Stdlib exclusion must prevent a double-free of
     * vm->stdlib_module.  The VM should not crash. */
    urbi_realm_destroy(&vm, a);

    UASSERT(mod_a.owning_realm == NULL);

    /* Stdlib module pointer still valid — not freed. */
    if (stdlib != NULL) {
        UASSERT(vm.stdlib_module == stdlib);
    }

    /* Realm B must still be functional: eval a simple expression. */
    char buf[128];
    int rcb = urbi_repl_eval(&vm, b, "1 + 1", 5, buf, sizeof(buf));
    UASSERT_EQ(URBI_OK, rcb);

    urbi_realm_destroy(&vm, b);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Suite entry
 * ----------------------------------------------------------------------- */

void
test_realm_destroy_with_parked_loader_suite(void)
{
    utest_run("realm_destroy: walks loaded_protos_head and unloads modules",
              realm_destroy_unloads_modules);
    utest_run("realm_destroy: stdlib exclusion — first realm destroy safe",
              realm_destroy_stdlib_exclusion);
}
