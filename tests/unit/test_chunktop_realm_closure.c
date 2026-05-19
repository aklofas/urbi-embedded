/* SPDX-License-Identifier: BSD-3-Clause */
/* test_chunktop_realm_closure — task #22 regression.
 *
 * Bug: `Realm.fn = function () {...}` at chunk-top fataled with
 * "TypeError: SETSLOT: receiver is not an Object".
 *
 * Root cause (`emit_function_literal` in `src/emit/uemit_stmt.c`): the
 * closure-destination register was computed from `current_fs->freereg`,
 * but freereg tracks only the local-zone floor (locals + params +
 * r_global_slot reservation).  Live temps above the floor — including
 * the `Realm` GETSLOT result returned by `emit_ident_arm`'s realm-global
 * fallback — are tracked via `e->next_reg`, which drifts above
 * `freereg` as the floor sits still.  `emit_function_literal` also
 * clobbered `e->next_reg` mid-helper (`e->next_reg = child_fs->freereg`)
 * without restoring the parent's value, so the post-close `dst` lookup
 * couldn't recover from `next_reg` either.
 *
 * Pre-fix bytecode for `Realm.fn = function () { 1 }`:
 *     LOAD_REALM_GLOBAL R0          ; R0 = global_object
 *     GETSLOT R1, R0, ic=0          ; R1 = Realm
 *     CLOSURE R1, P0                ; R1 = closure  (CLOBBERS R1!)
 *     SETSLOT R1, R1, ic=1          ; R1.fn = R1 — receiver R1 is now a
 *                                   ; closure, not an Object → fatal
 *
 * Post-fix the closure lands strictly above the live `Realm` temp. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "chunk/uchunk.h"
#include "value/uarena.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* === Test 1: minimal canonical repro ================================== */
UTEST(realm_assign_closure_minimal)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "Realm.fn = function () { 1 }",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Confirm the slot was actually set with a callable closure. */
    UValue fn = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "fn", 2, &fn));
    UASSERT_EQ((int)UVAL_CLOSURE, (int)fn.kind);

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

/* === Test 2: two-statement design-risks repro ========================= */
UTEST(realm_assign_closure_followed_by_int_assign)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "Realm.fn = function () { 1 };"
        "Realm.x  = 0",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    UValue fn = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "fn", 2, &fn));
    UASSERT_EQ((int)UVAL_CLOSURE, (int)fn.kind);

    UValue x = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "x", 1, &x));
    UASSERT_EQ((int)UVAL_INT, (int)x.kind);
    UASSERT_EQ(0LL, x.v.i);

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

/* === Test 3: var-decl variant — `var fn = function () {...}` at
 * chunk-top also routes through the realm-global SETSLOT path and could
 * exhibit the same drift.  Confirms the fix isn't narrowly tied to the
 * `Realm.X = ...` syntax. ============================================ */
UTEST(chunktop_var_decl_closure)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var fn = function () { 7 };"
        "var x  = 0",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    UValue fn = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "fn", 2, &fn));
    UASSERT_EQ((int)UVAL_CLOSURE, (int)fn.kind);

    UValue x = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "x", 1, &x));
    UASSERT_EQ((int)UVAL_INT, (int)x.kind);

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

/* === Suite entry. ====================================================== */
void
test_chunktop_realm_closure_suite(void)
{
    utest_run("chunktop_realm_closure: minimal repro (task #22)",
              realm_assign_closure_minimal);
    utest_run("chunktop_realm_closure: closure-then-int-assign",
              realm_assign_closure_followed_by_int_assign);
    utest_run("chunktop_realm_closure: var-decl closure variant",
              chunktop_var_decl_closure);
}
