/* SPDX-License-Identifier: BSD-3-Clause */
/* test_event_register_errors.c — TDD tests for urbi_event_register (Gap B)
 * error paths.
 *
 * Three sub-tests:
 *   1. register_dup_name_returns_invalid: registering the same name twice
 *      returns URBI_EVENT_ID_INVALID on the second call.
 *      (Phase 8 will add the error-code assertion for URBI_ERR_EVENT_NAME_TAKEN.)
 *   2. register_null_name_returns_invalid: NULL name returns
 *      URBI_EVENT_ID_INVALID.
 *   3. register_oom_uevent_alloc_returns_invalid: OOM during UEvent allocation
 *      (triggered via urbi_lock_heap) returns URBI_EVENT_ID_INVALID.
 */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* =========================================================================
 * Sub-test 1: duplicate name returns URBI_EVENT_ID_INVALID.
 *
 * Phase 8 note: when urbi_last_error lands, add:
 *   UASSERT_EQ((int)urbi_last_error(&vm).code, (int)URBI_ERR_EVENT_NAME_TAKEN);
 * ========================================================================= */

UTEST(register_dup_name_returns_invalid)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* First registration: must succeed. */
    urbi_event_id_t id1 = urbi_event_register(&vm, realm, "foo", NULL, NULL);
    UASSERT(id1 != URBI_EVENT_ID_INVALID);

    /* Second registration with same name: must fail. */
    /* TODO Phase 8: also assert urbi_last_error code == URBI_ERR_EVENT_NAME_TAKEN */
    urbi_event_id_t id2 = urbi_event_register(&vm, realm, "foo", NULL, NULL);
    UASSERT(id2 == URBI_EVENT_ID_INVALID);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: NULL name returns URBI_EVENT_ID_INVALID.
 * ========================================================================= */

UTEST(register_null_name_returns_invalid)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* NULL name. */
    urbi_event_id_t id = urbi_event_register(&vm, realm, NULL, NULL, NULL);
    UASSERT(id == URBI_EVENT_ID_INVALID);

    /* NULL vm. */
    id = urbi_event_register(NULL, realm, "bar", NULL, NULL);
    UASSERT(id == URBI_EVENT_ID_INVALID);

    /* NULL realm. */
    id = urbi_event_register(&vm, NULL, "bar", NULL, NULL);
    UASSERT(id == URBI_EVENT_ID_INVALID);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: OOM during UEvent alloc returns URBI_EVENT_ID_INVALID.
 *
 * urbi_lock_heap prevents urbi_gc_alloc from returning a new cell, causing
 * urbi_event_create to return NULL.  urbi_event_register must propagate
 * this as URBI_EVENT_ID_INVALID without crashing.
 * ========================================================================= */

UTEST(register_oom_uevent_alloc_returns_invalid)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Lock the heap so urbi_gc_alloc inside urbi_event_create returns NULL. */
    urbi_lock_heap(&vm);

    /* TODO Phase 8: also assert urbi_last_error code == URBI_ERR_OOM */
    urbi_event_id_t id = urbi_event_register(&vm, realm, "myOomEvent", NULL, NULL);
    UASSERT(id == URBI_EVENT_ID_INVALID);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_event_register_errors_suite(void)
{
    utest_run("event_register: dup name returns URBI_EVENT_ID_INVALID",
              register_dup_name_returns_invalid);
    utest_run("event_register: NULL vm/realm/name return URBI_EVENT_ID_INVALID",
              register_null_name_returns_invalid);
    utest_run("event_register: OOM on UEvent alloc returns URBI_EVENT_ID_INVALID",
              register_oom_uevent_alloc_returns_invalid);
}
