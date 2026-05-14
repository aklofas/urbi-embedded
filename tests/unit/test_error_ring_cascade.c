/* SPDX-License-Identifier: BSD-3-Clause */
/* test_error_ring_cascade.c — TDD tests for the error ring buffer wrap
 * semantics (Gap P, v0.7.1).
 *
 * The error ring holds up to URBI_ERROR_RING_DEPTH (4) entries.  A 5th
 * error wraps, overwriting the oldest entry.  urbi_last_error always
 * returns the MOST RECENT entry.
 *
 * Three sub-tests:
 *   1. ring_stores_4_errors: trigger 4 errors; ring is full (count==4);
 *      urbi_last_error returns the 4th (most recent).
 *   2. fifth_error_wraps_oldest: trigger 5th error; urbi_last_error returns
 *      the 5th (most recent); ring count stays at 4.
 *   3. clear_error_empties_full_ring: after 5 errors, urbi_clear_error;
 *      urbi_last_error returns URBI_OK.
 *
 * Note: to inspect ALL ring entries (not just most-recent), tests include
 * vm/uvm.h for direct access to vm.error_ring.  This is a test-private
 * internal access — production code only uses urbi_last_error. */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"       /* UVM, UErrorRing, URBI_ERROR_RING_DEPTH */
#include "vm/uvm_error.h" /* urbi_set_error_internal (internal; test access) */
#include "realm/urealm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Helper: push N distinct errors into the ring using urbi_set_error_internal.
 * Uses code -1 for all entries; message is a single-char tag so tests can
 * distinguish entries by message[0]. */
static void push_errors(UVM *vm, int n)
{
    int i;
    char msg[4];
    for (i = 0; i < n; i++) {
        msg[0] = (char)('A' + (char)i);
        msg[1] = '\0';
        urbi_set_error_internal(vm, URBI_ERR_INVALID_ARG,
            msg, NULL, i, "test");
    }
}

/* =========================================================================
 * Sub-test 1: ring stores up to URBI_ERROR_RING_DEPTH errors.
 * ========================================================================= */

UTEST(ring_stores_4_errors)
{
    UVM vm;
    urbi_error_info_t info;
    int rc;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* Ring starts empty. */
    UASSERT_EQ(0U, vm.error_ring.count);

    push_errors(&vm, (int)URBI_ERROR_RING_DEPTH);

    /* Ring is full. */
    UASSERT_EQ((size_t)URBI_ERROR_RING_DEPTH, vm.error_ring.count);

    /* urbi_last_error returns the most recent (entry D, source_line 3). */
    rc = urbi_last_error(&vm, &info);
    UASSERT_EQ((int)URBI_ERR_INVALID_ARG, rc);
    UASSERT_EQ(3, info.source_line);   /* 4th push used line=3 */
    UASSERT(info.message[0] == 'D');   /* message tag 'D' */

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: 5th error wraps (oldest dropped); count stays at 4.
 * ========================================================================= */

UTEST(fifth_error_wraps_oldest)
{
    UVM vm;
    urbi_error_info_t info;
    int rc;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* Push 4 errors (fills ring). */
    push_errors(&vm, (int)URBI_ERROR_RING_DEPTH);
    UASSERT_EQ((size_t)URBI_ERROR_RING_DEPTH, vm.error_ring.count);

    /* Push 5th error — overwrites oldest slot (wrap). */
    urbi_set_error_internal(&vm, URBI_ERR_OOM,
        "fifth", NULL, 99, "test");

    /* Count remains at URBI_ERROR_RING_DEPTH. */
    UASSERT_EQ((size_t)URBI_ERROR_RING_DEPTH, vm.error_ring.count);

    /* Most recent is the 5th (code OOM, message "fifth", line 99). */
    rc = urbi_last_error(&vm, &info);
    UASSERT_EQ((int)URBI_ERR_OOM, rc);
    UASSERT_EQ(99, info.source_line);
    UASSERT(strcmp(info.message, "fifth") == 0);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: urbi_clear_error empties a full ring.
 * ========================================================================= */

UTEST(clear_empties_full_ring)
{
    UVM vm;
    urbi_error_info_t info;
    int rc;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* Fill ring + one wrap. */
    push_errors(&vm, (int)URBI_ERROR_RING_DEPTH + 1);
    UASSERT_EQ((size_t)URBI_ERROR_RING_DEPTH, vm.error_ring.count);

    /* Clear. */
    urbi_clear_error(&vm);
    UASSERT_EQ(0U, vm.error_ring.count);
    UASSERT_EQ(0U, vm.error_ring.head);

    /* urbi_last_error must return URBI_OK. */
    rc = urbi_last_error(&vm, &info);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ(0, info.code);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_error_ring_cascade_suite(void)
{
    utest_run("error_ring: ring stores up to URBI_ERROR_RING_DEPTH entries",
              ring_stores_4_errors);
    utest_run("error_ring: 5th error wraps oldest; most-recent is 5th",
              fifth_error_wraps_oldest);
    utest_run("error_ring: clear_error empties full ring",
              clear_empties_full_ring);
}
