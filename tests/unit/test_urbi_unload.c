/* SPDX-License-Identifier: BSD-3-Clause */
/* Verify urbi_unload(vm, module) public API.  v0.9.0-repl Task 11. */

#include "utest.h"

#include <string.h>
#include <stdlib.h>
#include <stddef.h>

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

/* Compile and load a source string into mod, registering it in realm. */
static int compile_and_load(const char *src, UVM *vm, URealm *realm,
                             UModule *mod, UArena *arena)
{
    ULexer  lex;
    UParser p;
    UEmitter e;
    UAstNode *node;

    ulex_init(&lex, src, strlen(src));
    uemit_init(&e, mod, arena, vm, NULL);
    uparse_init(&p, &lex, arena);

    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) return -1;
        if (uemit_statement(&e, node) != EMIT_OK) return -1;
        uarena_reset(arena);
    }
    if (uemit_finish(&e) != EMIT_OK) return -1;

    UValue out;
    urbi_zero(&out, sizeof(out));
    return urbi_run_chunk(vm, realm, mod, &out);
}

/* -----------------------------------------------------------------------
 * Test 1: urbi_unload immediately destroys a loaded module
 * ----------------------------------------------------------------------- */

UTEST(urbi_unload_immediate_destroy)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UArena arena;
    uarena_init(&arena, 4096);
    UModule mod;
    urbi_zero(&mod, sizeof(mod));

    int rc = compile_and_load("1 + 2 |", &vm, r, &mod, &arena);
    UASSERT_EQ(URBI_OK, rc);

    /* Module should be registered at the head of the realm list
     * (it was inserted most-recently; stdlib may be behind it). */
    UASSERT(r->loaded_protos_head == &mod);
    UASSERT(mod.owning_realm == r);
    /* Snapshot next_in_realm: there may be pre-existing modules (e.g.
     * vm->stdlib_module registered when the realm was populated). */
    UModule *next_behind_mod = mod.next_in_realm;

    /* Unload should succeed and unlink the module. */
    int unload_rc = urbi_unload(&vm, &mod);
    UASSERT_EQ(URBI_OK, unload_rc);

    /* After unload: mod is gone; head points past it (to whatever was behind). */
    UASSERT(r->loaded_protos_head == next_behind_mod);
    /* mod fields cleared. */
    UASSERT(mod.owning_realm == NULL);
    /* mod is not reachable anywhere in the realm list. */
    for (UModule *p = r->loaded_protos_head; p != NULL; p = p->next_in_realm) {
        UASSERT(p != &mod);
    }

    uarena_destroy(&arena);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Test 2: urbi_unload returns URBI_ERR_INVALID_ARG for bad arguments
 * ----------------------------------------------------------------------- */

UTEST(urbi_unload_invalid_args)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* NULL vm → URBI_ERR_INVALID_ARG. */
    UASSERT_EQ(URBI_ERR_INVALID_ARG, urbi_unload(NULL, NULL));

    /* Valid vm, NULL module → URBI_ERR_INVALID_ARG. */
    UASSERT_EQ(URBI_ERR_INVALID_ARG, urbi_unload(&vm, NULL));

    /* Module never bound to a realm (owning_realm == NULL) → URBI_ERR_INVALID_ARG. */
    UModule mod;
    urbi_zero(&mod, sizeof(mod));
    UASSERT_EQ(URBI_ERR_INVALID_ARG, urbi_unload(&vm, &mod));

    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Test 3: double urbi_unload returns URBI_ERR_INVALID_ARG on second call
 * ----------------------------------------------------------------------- */

UTEST(urbi_unload_double_unload)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UArena arena;
    uarena_init(&arena, 4096);
    UModule mod;
    urbi_zero(&mod, sizeof(mod));

    int rc = compile_and_load("1 + 2 |", &vm, r, &mod, &arena);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT(mod.owning_realm == r);

    /* First unload: succeeds. */
    UASSERT_EQ(URBI_OK, urbi_unload(&vm, &mod));

    /* Second unload: owning_realm is NULL → URBI_ERR_INVALID_ARG. */
    UASSERT_EQ(URBI_ERR_INVALID_ARG, urbi_unload(&vm, &mod));

    uarena_destroy(&arena);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Test 4: CHSTR-027 regression — urbi_repl_eval must heap-alloc UModule
 *         so each REPL line accumulates as a distinct entry in the realm's
 *         loaded_protos_head list.  Pre-v0.9.0 stack-alloc reused the same
 *         address across iterations, causing subsequent lines to collide.
 * ----------------------------------------------------------------------- */

UTEST(repl_eval_no_alias_across_lines)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    /* 50 REPL lines.  Each should heap-allocate its own UModule.  If the
     * pre-v0.9.0 stack-aliasing pattern reappeared, the realm registry
     * would see fewer entries (subsequent lines reusing the same address). */
    char buf[256];
    for (int i = 0; i < 50; i++) {
        char src[64];
        int slen = 0;
        /* Build "var x_NN = NN |" manually without snprintf dependency
         * (this file already includes <stdlib.h> and the runner has <stdio.h>;
         * snprintf is safe here since __STDC_HOSTED__ is defined for unit tests). */
        slen = snprintf(src, sizeof src, "var x_%d = %d |", i, i);
        buf[0] = '\0';
        int rc = urbi_repl_eval(&vm, r, src, (size_t)slen, buf, sizeof buf);
        UASSERT_EQ(URBI_OK, rc);
    }

    /* Walk registry — count user modules (skip vm->stdlib_module). */
    int user_count = 0;
    for (UModule *m = r->loaded_protos_head; m != NULL; m = m->next_in_realm) {
        if (m != vm.stdlib_module) user_count++;
    }
    /* Expect 50 distinct heap-allocated modules, one per REPL line. */
    UASSERT_EQ(50, user_count);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Test 5: urbi_realm_create_repl sets REALM_REPL flag
 * ----------------------------------------------------------------------- */

UTEST(realm_create_repl_sets_flag)
{
    UVM vm;
    urbi_zero(&vm, sizeof vm);
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_create_repl(&vm);
    UASSERT(r != NULL);
    UASSERT((r->flags & REALM_REPL) != 0);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Suite entry
 * ----------------------------------------------------------------------- */

void
test_urbi_unload_suite(void)
{
    utest_run("urbi_unload: immediate destroy and unlink from realm",
              urbi_unload_immediate_destroy);
    utest_run("urbi_unload: invalid args return URBI_ERR_INVALID_ARG",
              urbi_unload_invalid_args);
    utest_run("urbi_unload: double unload returns URBI_ERR_INVALID_ARG",
              urbi_unload_double_unload);
    utest_run("repl_eval: each line gets a distinct heap-alloc module (CHSTR-027)",
              repl_eval_no_alias_across_lines);
    utest_run("urbi_realm_create_repl: sets REALM_REPL flag",
              realm_create_repl_sets_flag);
}
