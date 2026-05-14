/* SPDX-License-Identifier: BSD-3-Clause */
/* test_aux_event_table.c — TDD tests for urbi_aux_register_event_table
 * (Phase 9, v0.7.1).
 *
 * Sub-tests:
 *   1. batch_register_all_ok: register 3 events from a table — all 3 ids
 *      populated; all 3 retrievable via urbi_realm_get_global.
 *   2. dup_name_stops_at_second: 2nd entry has a duplicate name — returns
 *      error code; 2nd out_id is URBI_EVENT_ID_INVALID; first event remains
 *      registered (no rollback). */

#include "utest.h"
#include "urbi/aux.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"  /* UVM struct for stack allocation */

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* =========================================================================
 * Sub-test 1: all 3 events registered successfully.
 * ========================================================================= */

UTEST(batch_register_all_ok)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    struct URealm *realm = urbi_realm_global(&vm);

    urbi_event_id_t id0 = URBI_EVENT_ID_INVALID;
    urbi_event_id_t id1 = URBI_EVENT_ID_INVALID;
    urbi_event_id_t id2 = URBI_EVENT_ID_INVALID;

    urbi_aux_event_decl_t table[] = {
        { "sensorA", NULL, NULL, &id0 },
        { "sensorB", NULL, NULL, &id1 },
        { "sensorC", NULL, NULL, &id2 },
    };

    int rc = urbi_aux_register_event_table(&vm, realm, table, 3);
    UASSERT_EQ(URBI_OK, rc);

    UASSERT(id0 != URBI_EVENT_ID_INVALID);
    UASSERT(id1 != URBI_EVENT_ID_INVALID);
    UASSERT(id2 != URBI_EVENT_ID_INVALID);

    /* All three distinct. */
    UASSERT(id0 != id1);
    UASSERT(id1 != id2);
    UASSERT(id0 != id2);

    /* All three retrievable as realm globals (kind == UVAL_EVENT). */
    UValue out = urbi_make_nil();
    UASSERT_EQ(URBI_OK,
        urbi_realm_get_global(&vm, realm, "sensorA", 7, &out));
    UASSERT_EQ((int)UVAL_EVENT, (int)out.kind);

    out = urbi_make_nil();
    UASSERT_EQ(URBI_OK,
        urbi_realm_get_global(&vm, realm, "sensorB", 7, &out));
    UASSERT_EQ((int)UVAL_EVENT, (int)out.kind);

    out = urbi_make_nil();
    UASSERT_EQ(URBI_OK,
        urbi_realm_get_global(&vm, realm, "sensorC", 7, &out));
    UASSERT_EQ((int)UVAL_EVENT, (int)out.kind);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: duplicate name stops at 2nd entry; first event stays.
 * ========================================================================= */

UTEST(dup_name_stops_at_second)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    struct URealm *realm = urbi_realm_global(&vm);

    urbi_event_id_t id0 = URBI_EVENT_ID_INVALID;
    urbi_event_id_t id1 = URBI_EVENT_ID_INVALID;

    urbi_aux_event_decl_t table[] = {
        { "unique",  NULL, NULL, &id0 },
        { "unique",  NULL, NULL, &id1 },  /* duplicate — should fail */
    };

    int rc = urbi_aux_register_event_table(&vm, realm, table, 2);
    UASSERT(rc != URBI_OK);  /* error on 2nd entry */

    /* First event was registered successfully (no rollback). */
    UASSERT(id0 != URBI_EVENT_ID_INVALID);

    /* Second entry failed — out_id must be URBI_EVENT_ID_INVALID. */
    UASSERT_EQ((int)URBI_EVENT_ID_INVALID, (int)id1);

    /* First event is still in the realm global table. */
    UValue out = urbi_make_nil();
    UASSERT_EQ(URBI_OK,
        urbi_realm_get_global(&vm, realm, "unique", 6, &out));
    UASSERT_EQ((int)UVAL_EVENT, (int)out.kind);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: empty table is a no-op returning URBI_OK.
 * ========================================================================= */

UTEST(empty_table_ok)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    struct URealm *realm = urbi_realm_global(&vm);

    int rc = urbi_aux_register_event_table(&vm, realm, NULL, 0);
    UASSERT_EQ(URBI_OK, rc);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_aux_event_table_suite(void)
{
    utest_run("aux_event_table: batch register 3 events all succeed",
              batch_register_all_ok);
    utest_run("aux_event_table: dup name stops at 2nd entry, no rollback",
              dup_name_stops_at_second);
    utest_run("aux_event_table: empty table returns URBI_OK",
              empty_table_ok);
}
