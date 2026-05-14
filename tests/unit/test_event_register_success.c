/* SPDX-License-Identifier: BSD-3-Clause */
/* test_event_register_success.c — TDD tests for urbi_event_register (Gap B)
 * success path.
 *
 * Four sub-tests:
 *   1. register_returns_valid_id: urbi_event_register("foo") returns an id
 *      that is != URBI_EVENT_ID_INVALID.
 *   2. register_installs_realm_global: after registration the realm-global
 *      "foo" exists and its UValue kind is UVAL_EVENT.
 *   3. register_registry_entry_matches: registry lookup by id returns the
 *      same UEvent pointer as the realm-global value.
 *   4. register_two_names_two_ids: registering two distinct names yields
 *      two distinct, valid ids. */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "event/uevent_registry.h"   /* UEventRegistry, uevent_registry_lookup_by_id */
#include "event/uevent_native.h"     /* uvalue_as_event, UVAL_EVENT */
#include "runtime/umacros.h"         /* urbi_strlen */

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* =========================================================================
 * Sub-test 1: urbi_event_register returns a valid id.
 * ========================================================================= */

UTEST(register_returns_valid_id)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    urbi_event_id_t id = urbi_event_register(&vm, realm, "foo", NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: realm-global "foo" exists after registration and is UVAL_EVENT.
 * ========================================================================= */

UTEST(register_installs_realm_global)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    urbi_event_id_t id = urbi_event_register(&vm, realm, "myEvent", NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* Read back the realm-global. */
    UValue out = urbi_make_nil();
    int rc = urbi_realm_get_global(&vm, realm, "myEvent",
                                   urbi_strlen("myEvent"), &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)out.kind, (int)UVAL_EVENT);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: registry lookup by id returns the same UEvent as the realm-global.
 * ========================================================================= */

UTEST(register_registry_entry_matches)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    urbi_event_id_t id = urbi_event_register(&vm, realm, "bar", NULL, NULL);
    UASSERT(id != URBI_EVENT_ID_INVALID);

    /* Registry lookup by id. */
    UEventRegistryEntry *entry =
        uevent_registry_lookup_by_id(&vm.event_registry, id);
    UASSERT(entry != NULL);
    UASSERT(entry->event != NULL);

    /* Realm-global must point to the same UEvent. */
    UValue out = urbi_make_nil();
    int rc = urbi_realm_get_global(&vm, realm, "bar",
                                   urbi_strlen("bar"), &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)out.kind, (int)UVAL_EVENT);
    UASSERT(uvalue_as_event(out) == entry->event);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 4: two distinct names yield two distinct valid ids.
 * ========================================================================= */

UTEST(register_two_names_two_ids)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    urbi_event_id_t id1 = urbi_event_register(&vm, realm, "evt1", NULL, NULL);
    urbi_event_id_t id2 = urbi_event_register(&vm, realm, "evt2", NULL, NULL);

    UASSERT(id1 != URBI_EVENT_ID_INVALID);
    UASSERT(id2 != URBI_EVENT_ID_INVALID);
    /* The two ids must be distinct. */
    UASSERT(id1 != id2);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_event_register_success_suite(void)
{
    utest_run("event_register: returns valid id",
              register_returns_valid_id);
    utest_run("event_register: installs realm-global of kind UVAL_EVENT",
              register_installs_realm_global);
    utest_run("event_register: registry entry matches realm-global UEvent",
              register_registry_entry_matches);
    utest_run("event_register: two distinct names yield two distinct ids",
              register_two_names_two_ids);
}
