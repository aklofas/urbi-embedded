/* SPDX-License-Identifier: BSD-3-Clause */
/* test_register_dup_name.c — TDD tests for urbi_register dup-name and OOM
 * rejection (Gap A, v0.7.1).
 *
 * v1.0 CONSTANT enforcement note: the v1.0 packed-nibble shape model
 * enforces CONSTANT for slot indices 0-7 only (8-slot limit in the
 * UShape.flags uint32_t).  The global realm pre-populates 15+ slots, so
 * user-registered names land at slot >= 8 where CONSTANT enforcement is
 * best-effort (set_global_const succeeds but install_property is skipped).
 * This is the documented behaviour pinned in test_realm_globals_api.c
 * (set_global_const_past_slot_7_installs_without_const_enforcement).
 *
 * Four sub-tests:
 *   1. dup_name_second_call_succeeds: re-registering slot >= 8 is URBI_OK
 *      (best-effort CONSTANT at v1.0; documented limitation).
 *   2. dup_name_second_fn_visible: after the second call, the script sees
 *      the second function's result (value was overwritten).
 *   3. register_then_get_via_c_api: urbi_realm_get_global retrieves the
 *      installed closure UValue.
 *   4. oom_on_closure_alloc: heap-lock before urbi_register → URBI_ERR_OOM. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* -------------------------------------------------------------------------
 * Host functions used as test targets.
 * ------------------------------------------------------------------------- */

static int fn_returns_1(struct UVM *vm, UValue self,
                        UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    *out = urbi_make_int(1);
    return 0;
}

static int fn_returns_2(struct UVM *vm, UValue self,
                        UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    *out = urbi_make_int(2);
    return 0;
}

/* =========================================================================
 * Sub-test 1: re-registering a slot->=8 name is URBI_OK at v1.0.
 *
 * The global realm starts with 15+ constant slots populated (slots 0-14+).
 * Any user-registered name lands at slot >= 15, beyond the 8-slot packed-
 * nibble CONSTANT enforcement range.  urbi_realm_set_global_const finds the
 * slot is not marked CONSTANT and succeeds.  This is the documented v1.0
 * behaviour (see design-risks entry S-globals-cap-8).
 * ========================================================================= */

UTEST(dup_name_second_call_succeeds)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    int rc = urbi_register(&vm, NULL, "myRegFn", fn_returns_1);
    UASSERT_EQ(URBI_OK, rc);

    /* v1.0: slot >= 8 — no CONSTANT enforcement; second call is URBI_OK. */
    rc = urbi_register(&vm, NULL, "myRegFn", fn_returns_2);
    UASSERT_EQ(URBI_OK, rc);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: after second registration, script sees the second fn's value.
 * ========================================================================= */

UTEST(dup_name_second_fn_visible)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    int rc = urbi_register(&vm, NULL, "myRegFn2", fn_returns_1);
    UASSERT_EQ(URBI_OK, rc);

    rc = urbi_register(&vm, NULL, "myRegFn2", fn_returns_2);
    UASSERT_EQ(URBI_OK, rc);

    /* After the second registration, the script must see fn_returns_2's value. */
    UValue result = urbi_make_nil();
    rc = utest_e2e_compile_and_run(&vm, "myRegFn2()", &result);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)result.kind, (int)UVAL_INT);
    UASSERT_EQ(result.v.i, (int64_t)2);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: urbi_realm_get_global retrieves the installed closure.
 * ========================================================================= */

UTEST(register_then_get_via_c_api)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    int rc = urbi_register(&vm, NULL, "myRegFn3", fn_returns_1);
    UASSERT_EQ(URBI_OK, rc);

    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UValue out = urbi_make_nil();
    rc = urbi_realm_get_global(&vm, realm, "myRegFn3", 8, &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)out.kind, (int)UVAL_CLOSURE);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 4: heap-locked before urbi_register → URBI_ERR_OOM.
 * urbi_lock_heap disables all further GC allocations (Phase 13 / T145).
 * urbi_make_native_closure allocates a UClosure → must fail.
 * ========================================================================= */

UTEST(oom_on_closure_alloc)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    urbi_lock_heap(&vm);

    int rc = urbi_register(&vm, NULL, "locked", fn_returns_1);
    UASSERT_EQ(URBI_ERR_OOM, rc);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point
 * ========================================================================= */

void
test_register_dup_name_suite(void)
{
    utest_run("register: re-register slot->=8 name succeeds (v1.0 best-effort)",
              dup_name_second_call_succeeds);
    utest_run("register: second fn visible after re-registration",
              dup_name_second_fn_visible);
    utest_run("register: get_global retrieves installed closure",
              register_then_get_via_c_api);
    utest_run("register: heap-locked → URBI_ERR_OOM",
              oom_on_closure_alloc);
}
