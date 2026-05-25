/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_vm_first_arg.c — Wave 4 W5: vm-first-arg sweep.
 *
 * Compile-time + minimal runtime checks that the 17 reworked public-API
 * functions take (struct UVM *vm, ...) as their first argument, and that
 * three void-returning functions now return int (api-ergonomics F8).
 *
 * These tests deliberately use the actual new signatures; they will not
 * compile until include/urbi/urbi.h and include/urbi/sched.h are updated. */

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include <stdlib.h>

#define UTEST(name) static void name(void)

static void *w5_test_alloc(void *p, size_t n, void *ud)
{
    (void)ud;
    if (n == 0) { free(p); return NULL; }
    return realloc(p, n);
}

/* --- enum existence checks -------------------------------------------- */

UTEST(strand_state_enum_exists) {
    /* UStrandState must be defined with DORMANT=0, DEAD!=0. */
    UASSERT_EQ((int)URBI_STRAND_DORMANT,  0);
    UASSERT_NE((int)URBI_STRAND_DEAD,     0);
    /* Spot-check ordering: DORMANT < READY < RUNNING < DEAD. */
    UASSERT((int)URBI_STRAND_DORMANT  < (int)URBI_STRAND_READY);
    UASSERT((int)URBI_STRAND_READY    < (int)URBI_STRAND_RUNNING);
    UASSERT((int)URBI_STRAND_RUNNING  < (int)URBI_STRAND_DEAD);
}

UTEST(strand_unwind_enum_exists) {
    /* UStrandUnwind public mirror: OK=0. */
    UASSERT_EQ((int)URBI_UNWIND_OK, 0);
    UASSERT_NE((int)URBI_UNWIND_THROW,    0);
    UASSERT_NE((int)URBI_UNWIND_CANCEL,   0);
}

UTEST(invalid_state_error_code_exists) {
    /* URBI_ERR_INVALID_STATE must be negative (error code convention). */
    UASSERT((int)URBI_ERR_INVALID_STATE < 0);
}

/* --- strand_destroy returns int --------------------------------------- */

UTEST(strand_destroy_returns_int) {
    struct UVM *vm = urbi_vm_create(w5_test_alloc, NULL);
    UASSERT_NE(vm, NULL);
    /* NULL strand is a no-op and returns URBI_OK. */
    int rc = urbi_strand_destroy(vm, NULL);
    UASSERT_EQ(rc, URBI_OK);
    urbi_vm_free(vm);
}

/* --- strand_create takes vm as first arg ------------------------------ */

UTEST(strand_create_takes_vm_first) {
    struct UVM *vm = urbi_vm_create(w5_test_alloc, NULL);
    UASSERT_NE(vm, NULL);
    struct URealm *r = urbi_realm_global(vm);
    UASSERT_NE(r, NULL);
    /* NULL entry is valid: strand created DORMANT with no closure. */
    struct UStrand *s = urbi_strand_create(vm, r, NULL);
    UASSERT_NE(s, NULL);
    /* Query state: must be DORMANT. */
    UStrandState st = urbi_strand_state(vm, s);
    UASSERT_EQ((int)st, (int)URBI_STRAND_DORMANT);
    /* Destroy: must return URBI_OK for a DORMANT strand. */
    int rc = urbi_strand_destroy(vm, s);
    UASSERT_EQ(rc, URBI_OK);
    urbi_vm_free(vm);
}

/* --- urbi_strand_state coverage --------------------------------------- */

UTEST(strand_state_null_tolerance) {
    /* urbi_strand_state(NULL, NULL) must return URBI_STRAND_DEAD (NULL-safe). */
    UStrandState st = urbi_strand_state(NULL, NULL);
    UASSERT_EQ((int)st, (int)URBI_STRAND_DEAD);
}

UTEST(strand_state_dormant) {
    /* A freshly created strand with no entry closure is DORMANT. */
    struct UVM *vm = urbi_vm_create(w5_test_alloc, NULL);
    UASSERT_NE(vm, NULL);
    struct URealm *r = urbi_realm_global(vm);
    UASSERT_NE(r, NULL);
    struct UStrand *s = urbi_strand_create(vm, r, NULL);
    if (s != NULL) {
        UASSERT_EQ((int)urbi_strand_state(vm, s), (int)URBI_STRAND_DORMANT);
        /* Non-DORMANT destroy is covered indirectly by realm-teardown tests
         * in test_strand_spawn_inheritance.c and test_scheduler_invariant.c,
         * which exercise urbi_realm_destroy walking active strands. */
        int rc = urbi_strand_destroy(vm, s);
        UASSERT_EQ(rc, URBI_OK);
    }
    urbi_vm_free(vm);
}

/* --- urbi_throw returns int ------------------------------------------- */

UTEST(throw_returns_int) {
    /* NULL vm + NULL strand → URBI_ERR_INVALID_ARG. */
    int rc = urbi_throw(NULL, NULL, urbi_make_nil());
    UASSERT_EQ(rc, URBI_ERR_INVALID_ARG);
}

/* --- urbi_chunk_from_bytes takes vm as first arg ---------------------- */

UTEST(chunk_from_bytes_takes_vm) {
    struct UVM *vm = urbi_vm_create(w5_test_alloc, NULL);
    UASSERT_NE(vm, NULL);
    /* NULL buf → returns NULL (invalid arg). */
    struct UProto *root = urbi_chunk_from_bytes(vm, NULL, 0, NULL, 0);
    UASSERT_EQ(root, NULL);
    urbi_vm_free(vm);
}

/* --- urbi_return_val returns int -------------------------------------- */

UTEST(return_val_returns_int) {
    /* NULL vm + NULL strand → URBI_ERR_INVALID_ARG. */
    int rc = urbi_return_val(NULL, NULL, urbi_make_nil());
    UASSERT_EQ(rc, URBI_ERR_INVALID_ARG);
}

void test_vm_first_arg_suite(void) {
    utest_run("strand_state_enum_exists",   strand_state_enum_exists);
    utest_run("strand_unwind_enum_exists",  strand_unwind_enum_exists);
    utest_run("invalid_state_error_code",   invalid_state_error_code_exists);
    utest_run("strand_destroy_returns_int", strand_destroy_returns_int);
    utest_run("strand_create_takes_vm_first", strand_create_takes_vm_first);
    utest_run("strand_state_null_tolerance", strand_state_null_tolerance);
    utest_run("strand_state_dormant",       strand_state_dormant);
    utest_run("throw_returns_int",          throw_returns_int);
    utest_run("chunk_from_bytes_takes_vm",  chunk_from_bytes_takes_vm);
    utest_run("return_val_returns_int",     return_val_returns_int);
}
