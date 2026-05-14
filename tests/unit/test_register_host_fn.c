/* SPDX-License-Identifier: BSD-3-Clause */
/* test_register_host_fn.c — TDD tests for urbi_register (Gap A): host fn
 * registered via urbi_register is script-callable with arg/return roundtrip.
 *
 * Three sub-tests:
 *   1. register_happy_path: fn returns integer 42 → script call returns 42.
 *   2. register_with_args: fn echoes first argument back → script "myCB(7)"
 *      returns 7.
 *   3. register_null_guards: NULL vm, NULL name, NULL fn each return
 *      URBI_ERR_INVALID_ARG without crashing. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* -------------------------------------------------------------------------
 * Host functions used as test targets.
 * Signature: int fn(UVM *vm, UValue self, UValue *args, uint8_t nargs,
 *                   UValue *out)
 * ------------------------------------------------------------------------- */

/* Returns integer 42 regardless of arguments. */
static int host_fn_returns42(struct UVM *vm, UValue self,
                              UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    *out = urbi_make_int(42);
    return 0;
}

/* Echoes first argument (expects UVAL_INT); returns UVAL_NIL on missing. */
static int host_fn_echo_first(struct UVM *vm, UValue self,
                               UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self;
    if (nargs < 1) {
        *out = urbi_make_nil();
        return 0;
    }
    *out = args[0];
    return 0;
}

/* =========================================================================
 * Sub-test 1: happy path — registered fn returns 42 from script call.
 * ========================================================================= */

UTEST(register_happy_path)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    int rc = urbi_register(&vm, NULL, "myFn", host_fn_returns42);
    UASSERT_EQ(URBI_OK, rc);

    UValue result = urbi_make_nil();
    rc = utest_e2e_compile_and_run(&vm, "myFn()", &result);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)result.kind, (int)UVAL_INT);
    UASSERT_EQ(result.v.i, (int64_t)42);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: argument roundtrip — fn echoes integer arg back.
 * ========================================================================= */

UTEST(register_with_args)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    int rc = urbi_register(&vm, NULL, "myCB", host_fn_echo_first);
    UASSERT_EQ(URBI_OK, rc);

    UValue result = urbi_make_nil();
    rc = utest_e2e_compile_and_run(&vm, "myCB(7)", &result);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)result.kind, (int)UVAL_INT);
    UASSERT_EQ(result.v.i, (int64_t)7);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: NULL guards — NULL vm, NULL name, NULL fn each return
 * URBI_ERR_INVALID_ARG without crashing.
 * ========================================================================= */

UTEST(register_null_guards)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* NULL vm */
    int rc = urbi_register(NULL, NULL, "myFn", host_fn_returns42);
    UASSERT_EQ(URBI_ERR_INVALID_ARG, rc);

    /* NULL name */
    rc = urbi_register(&vm, NULL, NULL, host_fn_returns42);
    UASSERT_EQ(URBI_ERR_INVALID_ARG, rc);

    /* NULL fn */
    rc = urbi_register(&vm, NULL, "myFn", NULL);
    UASSERT_EQ(URBI_ERR_INVALID_ARG, rc);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point
 * ========================================================================= */

void
test_register_host_fn_suite(void)
{
    utest_run("register: fn returns constant from script call",
              register_happy_path);
    utest_run("register: fn echoes integer argument (roundtrip)",
              register_with_args);
    utest_run("register: NULL vm/name/fn return URBI_ERR_INVALID_ARG",
              register_null_guards);
}
