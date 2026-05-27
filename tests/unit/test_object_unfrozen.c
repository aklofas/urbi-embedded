/* SPDX-License-Identifier: BSD-3-Clause */
/* test_object_unfrozen.c — v0.10.11 W5 D5 unfreeze tests.
 *
 * Three tests:
 *   1. Object atom proto is NOT readonly after D5.
 *   2. Lobby proto is still readonly (session-registry invariant).
 *   3. `var Object.x = v; Object.x` round-trip succeeds via script.
 */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "object/uobject.h"
#include "realm/urealm.h"
#include "vm/uvm.h"

#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* === Test 1: Object atom proto is NOT readonly post-D5 ================ */
UTEST(object_proto_not_readonly)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    /* urbi_realm_global boots the stdlib, setting up all atom protos. */
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UValue obj_v;
    int rc = urbi_realm_get_global(&vm, r, "Object", 6, &obj_v);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)obj_v.kind, (int)UVAL_OBJECT);

    UObject *root = (UObject *)obj_v.v.p;
    UASSERT(root != NULL);
    UASSERT((root->flags & URBI_OBJ_FLAG_READONLY) == 0U);

    urbi_vm_destroy(&vm);
}

/* === Test 2: Lobby proto is still readonly (D5 keeps it frozen) ======= */
UTEST(lobby_proto_still_readonly)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);
    UASSERT(vm.lobby_proto != NULL);
    UASSERT((vm.lobby_proto->flags & URBI_OBJ_FLAG_READONLY) != 0U);

    urbi_vm_destroy(&vm);
}

/* === Test 3: `var Object.x = v; Object.x` round-trip succeeds ========= */
UTEST(var_object_slot_succeeds)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UValue out;
    int rc = utest_e2e_compile_and_run(
        &vm,
        "var Object.global_marker = 42; Object.global_marker",
        &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ((long long)out.v.i, 42LL);

    urbi_vm_destroy(&vm);
}

/* ===== Suite ===== */
void test_object_unfrozen_suite(void);

void
test_object_unfrozen_suite(void)
{
    printf("test_object_unfrozen\n");
    utest_run("object_proto_not_readonly",  object_proto_not_readonly);
    utest_run("lobby_proto_still_readonly", lobby_proto_still_readonly);
    utest_run("var_object_slot_succeeds",   var_object_slot_succeeds);
}
