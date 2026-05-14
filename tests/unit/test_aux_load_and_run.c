/* SPDX-License-Identifier: BSD-3-Clause */
/* test_aux_load_and_run.c — TDD tests for urbi_aux_load_and_run composite
 * helper (Phase 9, v0.7.1).
 *
 * Sub-tests:
 *   1. valid_bytecode_runs_ok: compile "42" via urbi_compile_source, then
 *      load and run it via urbi_aux_load_and_run; out_result kind is int.
 *   2. invalid_bytes_returns_error: garbage bytes return an error code.
 *   3. null_vm_returns_invalid_arg: NULL vm → URBI_ERR_INVALID_ARG.
 *   4. null_bytecode_returns_invalid_arg: NULL bytecode → URBI_ERR_INVALID_ARG.
 *   5. out_result_null_ok: valid bytecode, NULL out_result — still URBI_OK. */

#include "utest.h"
#include "urbi/aux.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* =========================================================================
 * Sub-test 1: valid bytecode compiles and runs, result is an int.
 * ========================================================================= */

UTEST(valid_bytecode_runs_ok)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    unsigned char *bc  = NULL;
    size_t         bc_len = 0;
    char err[256] = {0};

    /* Compile a trivial expression. */
    int rc = urbi_compile_source(&vm, "42", 2, "test",
                                  &bc, &bc_len, err, sizeof err);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT(bc != NULL);
    UASSERT(bc_len > 0);

    /* Now load and run via the aux helper. */
    UValue result = urbi_make_nil();
    rc = urbi_aux_load_and_run(&vm, bc, bc_len, &result);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)result.kind);
    UASSERT_EQ(42LL, (long long)result.v.i);

    free(bc);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: garbage bytes return a non-OK error code.
 * ========================================================================= */

UTEST(invalid_bytes_returns_error)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    static const uint8_t junk[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01 };
    UValue result = urbi_make_nil();
    int rc = urbi_aux_load_and_run(&vm, junk, sizeof junk, &result);
    UASSERT(rc != URBI_OK);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: NULL vm → URBI_ERR_INVALID_ARG.
 * ========================================================================= */

UTEST(null_vm_invalid_arg)
{
    static const uint8_t dummy[] = { 0x01 };
    UValue result = urbi_make_nil();
    int rc = urbi_aux_load_and_run(NULL, dummy, sizeof dummy, &result);
    UASSERT_EQ(URBI_ERR_INVALID_ARG, rc);
}

/* =========================================================================
 * Sub-test 4: NULL bytecode → URBI_ERR_INVALID_ARG.
 * ========================================================================= */

UTEST(null_bytecode_invalid_arg)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UValue result = urbi_make_nil();
    int rc = urbi_aux_load_and_run(&vm, NULL, 0, &result);
    UASSERT_EQ(URBI_ERR_INVALID_ARG, rc);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 5: NULL out_result is valid (caller ignores return value).
 * ========================================================================= */

UTEST(null_out_result_ok)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    unsigned char *bc  = NULL;
    size_t         bc_len = 0;
    char err[256] = {0};

    int rc = urbi_compile_source(&vm, "1 + 2", 5, "test",
                                  &bc, &bc_len, err, sizeof err);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT(bc != NULL);

    rc = urbi_aux_load_and_run(&vm, bc, bc_len, NULL);
    UASSERT_EQ(URBI_OK, rc);

    free(bc);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_aux_load_and_run_suite(void)
{
    utest_run("aux_load_and_run: valid bytecode runs and result is int",
              valid_bytecode_runs_ok);
    utest_run("aux_load_and_run: garbage bytes return error",
              invalid_bytes_returns_error);
    utest_run("aux_load_and_run: NULL vm returns URBI_ERR_INVALID_ARG",
              null_vm_invalid_arg);
    utest_run("aux_load_and_run: NULL bytecode returns URBI_ERR_INVALID_ARG",
              null_bytecode_invalid_arg);
    utest_run("aux_load_and_run: NULL out_result is valid",
              null_out_result_ok);
}
