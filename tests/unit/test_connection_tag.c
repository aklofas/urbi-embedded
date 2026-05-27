/* SPDX-License-Identifier: BSD-3-Clause */
/* test_connection_tag.c — v0.10.10-job-introspection / D7-E unit test for
 * the Lobby.connectionTag slot.
 *
 * connectionTag is a UVAL_TAG slot on the per-realm Lobby instance that
 * points at the realm's root tag (realm->tag).  W4 installs it in:
 *   - urbi_lobby_register_session (REPL session path) on the session's
 *     per-session Lobby instance (session_realm->global_object).
 *   - urbi_lobby_native_register_globals (default-realm path) on
 *     vm->lobby_proto via the C-side slot setter (which bypasses the
 *     URBI_OBJ_FLAG_READONLY OP_SETSLOT guard).
 *
 * This test exercises the default-realm path: a freshly-initialised UVM has
 * a single global realm whose lobby_proto carries the connectionTag slot
 * pointing at the global realm's tag.
 */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"

#define UTEST(name) static void name(void)

/* Test 1: Lobby.connectionTag is a UVAL_TAG and is the same value
 * across two reads (slot identity).  The value formats as "<Tag>"
 * (the realm-root tag has no name). */
UTEST(connection_tag_is_stable_tag_value)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    char buf[64];
    int rc = urbi_repl_eval(&vm, NULL,
                            "Lobby.connectionTag",
                            (size_t)strlen("Lobby.connectionTag"),
                            buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_STR_EQ(buf, "<Tag>");

    /* Same value on a second read. */
    rc = urbi_repl_eval(&vm, NULL,
                        "Lobby.connectionTag == Lobby.connectionTag",
                        (size_t)strlen("Lobby.connectionTag == Lobby.connectionTag"),
                        buf, sizeof(buf));
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_STR_EQ(buf, "true");

    urbi_vm_destroy(&vm);
}

/* ===== Suite ===== */
void test_connection_tag_suite(void);

void
test_connection_tag_suite(void)
{
    utest_run("connection_tag_is_stable_tag_value",
              connection_tag_is_stable_tag_value);
}
