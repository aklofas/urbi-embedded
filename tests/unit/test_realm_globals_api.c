/* SPDX-License-Identifier: BSD-3-Clause */
/* T75: public C API — urbi_realm_set_global / set_global_const / get_global.
 *
 * Verifies:
 *   1. urbi_realm_set_global installs a slot that a script can read.
 *   2. urbi_realm_set_global_const installs/updates a const slot that
 *      a script cannot overwrite (var write fails at runtime).
 *      N.B. CONSTANT enforcement in the IC is limited to slot indices 0-7 at
 *      M5 baseline (packed shape flags; M6 side-table tier lifts the cap).
 *      The test therefore exercises set_global_const on an existing built-in
 *      at slot index 0 ("Object"), which IS in the protected range.
 *   3. urbi_realm_get_global returns URBI_ERR_SLOT_NOT_FOUND when the name
 *      is absent.
 *   4. urbi_realm_set_global on an existing non-const slot overwrites it. */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "value/uarena.h"
#include "uast.h"
#include "uemit.h"
#include "ulex.h"
#include "umodule.h"
#include "uparse.h"
#include "uvm.h"
#include "urbi/urbi.h"
#include "realm/urealm.h"   /* URealm.global_object */

#define UTEST(name) static void name(void)

/* Helper: compile + run source under the VM's global Realm (same realm that
 * urbi_run_chunk always uses internally); returns URBI_OK or error code. */
static int compile_and_run(UVM *vm, const char *src, UValue *out_result)
{
    URealm *realm = urbi_realm_global(vm);
    if (realm == NULL) return URBI_ERR_OOM;

    ULexer   lex;
    UArena   arena;
    UModule  module = {0};
    UEmitter e;
    UParser  p;
    UAstNode *node;

    ulex_init(&lex, src, strlen(src));
    uarena_init(&arena, 4096);
    uemit_init(&e, &module, &arena, vm, NULL);
    uparse_init(&p, &lex, &arena);

    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) {
            uarena_destroy(&arena);
            umodule_destroy(&module);
            return URBI_ERR_COMPILE;
        }
        if (uemit_statement(&e, node) != EMIT_OK) {
            uarena_destroy(&arena);
            umodule_destroy(&module);
            return URBI_ERR_COMPILE;
        }
        uarena_reset(&arena);
    }
    if (uemit_finish(&e) != EMIT_OK) {
        uarena_destroy(&arena);
        umodule_destroy(&module);
        return URBI_ERR_COMPILE;
    }

    UValue result = {0};
    int rc = urbi_run_chunk(vm, realm, &module, &result);
    if (out_result != NULL) {
        *out_result = result;
    }
    uarena_destroy(&arena);
    umodule_destroy(&module);
    return rc;
}

/* Helpers to build UValue literals without <string.h>. */
static UValue make_int(int64_t n)
{
    UValue v;
    int i;
    v.kind = UVAL_INT;
    for (i = 0; i < 7; i++) v._pad[i] = 0;
    v.v.i = n;
    return v;
}

/* === Tests === */

UTEST(set_global_then_script_reads) {
    /* Install "myAnswer" = integer 42 on the VM's global realm via the C API,
     * then verify a script can read it back as an integer value.
     *
     * Note: urbi_run_chunk always executes scripts in the VM's global realm
     * (realm argument is reserved for future multi-realm scheduling). */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* Auto-create the global realm so we can install on it. */
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    int rc = urbi_realm_set_global(&vm, realm, "myAnswer", 8, make_int(42));
    UASSERT_EQ(URBI_OK, rc);

    UValue result = {0};
    int run_rc = compile_and_run(&vm, "myAnswer", &result);
    UASSERT_EQ(URBI_OK, run_rc);
    UASSERT_EQ((uint8_t)UVAL_INT, result.kind);
    UASSERT_EQ((int64_t)42, result.v.i);

    uvm_destroy(&vm);
}

UTEST(set_global_const_blocks_script_write) {
    /* urbi_realm_set_global_const on an existing built-in constant slot
     * ("Object", slot index 0) updates its value while preserving the
     * CONSTANT flag.  A subsequent script "var Object = 99" must still fail.
     *
     * "Object" is at slot index 0 in the global object (populated by
     * urbi_populate_realm_globals).  The IC checks packed shape flags for
     * indices 0-7, so CONSTANT enforcement is guaranteed here. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Re-install "Object" via set_global_const (no-op on value matters less
     * than confirming the call returns URBI_OK and const stays in effect). */
    UValue sentinel = make_int(99);
    int rc = urbi_realm_set_global_const(&vm, realm, "Object", 6, sentinel);
    UASSERT_EQ(URBI_OK, rc);

    /* Script write must fail: "Object" is const (slot index 0). */
    int run_rc = compile_and_run(&vm, "var Object = 42", NULL);
    UASSERT(run_rc != URBI_OK);
    /* Error message must mention the slot name. */
    UASSERT(strstr(vm.last_errmsg, "Object") != NULL);

    uvm_destroy(&vm);
}

UTEST(get_global_returns_slot_not_found_when_absent) {
    /* urbi_realm_get_global on a name that doesn't exist must return
     * URBI_ERR_SLOT_NOT_FOUND (not a crash, not URBI_OK). */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UValue out = {0};
    int rc = urbi_realm_get_global(&vm, realm,
                                   "totally_absent_xyz", 18, &out);
    UASSERT_EQ(URBI_ERR_SLOT_NOT_FOUND, rc);

    uvm_destroy(&vm);
}

UTEST(set_global_overwrites_non_const) {
    /* urbi_realm_set_global on a name that was already installed (non-const)
     * must update the value, and urbi_realm_get_global must return the
     * new value. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Install "counter" = 1 */
    int rc = urbi_realm_set_global(&vm, realm, "counter", 7, make_int(1));
    UASSERT_EQ(URBI_OK, rc);

    /* Overwrite "counter" = 2 */
    rc = urbi_realm_set_global(&vm, realm, "counter", 7, make_int(2));
    UASSERT_EQ(URBI_OK, rc);

    /* Read back via C API */
    UValue out = {0};
    rc = urbi_realm_get_global(&vm, realm, "counter", 7, &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((uint8_t)UVAL_INT, out.kind);
    UASSERT_EQ((int64_t)2, out.v.i);

    uvm_destroy(&vm);
}

void
test_realm_globals_api_suite(void)
{
    utest_run("set_global: C-installed slot readable from script",
              set_global_then_script_reads);
    utest_run("set_global_const: const-flagged slot rejects script write",
              set_global_const_blocks_script_write);
    utest_run("get_global: absent slot returns URBI_ERR_SLOT_NOT_FOUND",
              get_global_returns_slot_not_found_when_absent);
    utest_run("set_global: overwrites existing non-const slot",
              set_global_overwrites_non_const);
}
