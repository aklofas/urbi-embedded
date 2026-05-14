/* SPDX-License-Identifier: BSD-3-Clause */
/* test_last_error_populated.c — TDD tests for urbi_last_error / urbi_clear_error
 * (Gap P, v0.7.1).
 *
 * Three sub-tests:
 *   1. last_error_ok_after_success: register "foo" succeeds; urbi_last_error
 *      returns URBI_OK (ring empty because no error was set).
 *   2. last_error_populated_on_dup: register "foo" twice; second call fails
 *      with URBI_EVENT_ID_INVALID; urbi_last_error returns
 *      URBI_ERR_EVENT_NAME_TAKEN with non-NULL message.
 *   3. clear_error_empties_ring: after sub-test 2, urbi_clear_error;
 *      urbi_last_error returns URBI_OK + zero info. */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* =========================================================================
 * Sub-test 1: success path leaves ring empty (urbi_last_error == URBI_OK).
 * ========================================================================= */

UTEST(last_error_ok_after_success)
{
    UVM vm;
    urbi_error_info_t info;
    int rc;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* First registration must succeed. */
    urbi_event_id_t id = urbi_event_register(&vm, realm, "foo", NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* No error was set on the success path. */
    rc = urbi_last_error(&vm, &info);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ(0, info.code);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: duplicate name → urbi_last_error returns
 *             URBI_ERR_EVENT_NAME_TAKEN with non-NULL, non-empty message.
 * ========================================================================= */

UTEST(last_error_populated_on_dup)
{
    UVM vm;
    urbi_error_info_t info;
    int rc;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* First registration. */
    urbi_event_id_t id1 = urbi_event_register(&vm, realm, "foo", NULL, NULL);
    UASSERT(id1 != URBI_EVENT_ID_INVALID);

    /* Second registration with same name must fail. */
    urbi_event_id_t id2 = urbi_event_register(&vm, realm, "foo", NULL, NULL);
    UASSERT_EQ((int)URBI_EVENT_ID_INVALID, (int)id2);

    /* urbi_last_error must be populated with URBI_ERR_EVENT_NAME_TAKEN. */
    rc = urbi_last_error(&vm, &info);
    UASSERT_EQ((int)URBI_ERR_EVENT_NAME_TAKEN, rc);
    UASSERT_EQ((int)URBI_ERR_EVENT_NAME_TAKEN, info.code);
    UASSERT(info.message != NULL);
    UASSERT(info.message[0] != '\0');  /* non-empty message */

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: urbi_clear_error empties the ring.
 * ========================================================================= */

UTEST(clear_error_empties_ring)
{
    UVM vm;
    urbi_error_info_t info;
    int rc;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Trigger an error. */
    urbi_event_register(&vm, realm, "foo", NULL, NULL);
    urbi_event_register(&vm, realm, "foo", NULL, NULL);  /* dup → error */

    /* Confirm error is present before clearing. */
    rc = urbi_last_error(&vm, &info);
    UASSERT_EQ((int)URBI_ERR_EVENT_NAME_TAKEN, rc);

    /* Clear the ring. */
    urbi_clear_error(&vm);

    /* Now urbi_last_error must return URBI_OK with zeroed info. */
    rc = urbi_last_error(&vm, &info);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ(0, info.code);
    UASSERT_EQ(0, info.source_line);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_last_error_populated_suite(void)
{
    utest_run("last_error: URBI_OK after successful register",
              last_error_ok_after_success);
    utest_run("last_error: URBI_ERR_EVENT_NAME_TAKEN on dup register",
              last_error_populated_on_dup);
    utest_run("last_error: clear_error empties ring",
              clear_error_empties_ring);
}
