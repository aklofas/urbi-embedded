/* SPDX-License-Identifier: BSD-3-Clause */
/* test_error_string_lifetime.c — TDD tests for urbi_error_info_t string-
 * lifetime contract (Gap P, v0.7.1).
 *
 * Contract: const char* fields returned by urbi_last_error point into
 * UVM-owned ring-entry buffers.  They are valid until the next API call
 * that mutates error state (i.e. calls urbi_set_error_internal internally).
 *
 * Three sub-tests:
 *   1. string_ptr_valid_after_non_error_call: after getting a populated
 *      error info, calling a non-failing API does NOT push a new ring entry,
 *      so the pointer remains valid.
 *   2. string_ptr_still_readable_after_successful_call: capture message
 *      pointer; make a call that succeeds (no error published); pointer
 *      still readable, same content.
 *   3. new_error_changes_message: push a second error; urbi_last_error
 *      returns new message (old pointer may now be stale — the new ring
 *      entry overwrote it once the ring wraps, but with ring depth 4 and
 *      only 2 errors the OLD entry is still in the ring at a different slot,
 *      and most_recent now points to the new entry). */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Dummy host function for urbi_register tests. */
static int dummy_fn(struct UVM *vm, UValue self, UValue *args,
                    uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    *out = urbi_make_int(0);
    return URBI_OK;
}

/* =========================================================================
 * Sub-test 1: pointer valid after a call that does NOT set an error.
 * ========================================================================= */

UTEST(string_ptr_valid_after_non_error_call)
{
    UVM vm;
    urbi_error_info_t info;
    const char *msg_ptr;
    int rc;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Trigger an error to populate the ring. */
    urbi_event_register(&vm, realm, "foo", NULL, NULL);
    urbi_event_register(&vm, realm, "foo", NULL, NULL);  /* dup → error */

    rc = urbi_last_error(&vm, &info);
    UASSERT_EQ((int)URBI_ERR_EVENT_NAME_TAKEN, rc);
    UASSERT(info.message != NULL);
    msg_ptr = info.message;

    /* Register "bar" — this succeeds and does NOT push a ring entry.
     * The message pointer must still point to the same string. */
    rc = urbi_event_register(&vm, realm, "bar", NULL, NULL);
    UASSERT(rc != (int)URBI_EVENT_ID_INVALID);
    (void)rc;  /* suppress "set but not used" */

    /* Pointer is still valid — ring entry not overwritten. */
    UASSERT(msg_ptr[0] != '\0');  /* non-empty */

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: content same after a non-failing call.
 * ========================================================================= */

UTEST(string_content_unchanged_after_success_call)
{
    UVM vm;
    urbi_error_info_t info1, info2;
    int rc1, rc2;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Trigger dup error. */
    urbi_event_register(&vm, realm, "alpha", NULL, NULL);
    urbi_event_register(&vm, realm, "alpha", NULL, NULL);

    rc1 = urbi_last_error(&vm, &info1);
    UASSERT_EQ((int)URBI_ERR_EVENT_NAME_TAKEN, rc1);

    /* Register a different name (success, no ring mutation). */
    urbi_event_register(&vm, realm, "beta", NULL, NULL);

    /* Second call to urbi_last_error must return same code + same message content. */
    rc2 = urbi_last_error(&vm, &info2);
    UASSERT_EQ(rc1, rc2);
    UASSERT_EQ(info1.code, info2.code);

    /* Both point to the same ring entry — same pointer. */
    UASSERT(info1.message == info2.message);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: new error changes urbi_last_error result.
 *
 * Ring depth is 4.  After 2 errors, most_recent advances to the new entry.
 * urbi_last_error returns the new (most recent) entry's message — different
 * from the first.
 * ========================================================================= */

UTEST(new_error_changes_last_error_result)
{
    UVM vm;
    urbi_error_info_t info1, info2;
    int rc1, rc2;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Error 1: dup event name. */
    urbi_event_register(&vm, realm, "x", NULL, NULL);
    urbi_event_register(&vm, realm, "x", NULL, NULL);
    rc1 = urbi_last_error(&vm, &info1);
    UASSERT_EQ((int)URBI_ERR_EVENT_NAME_TAKEN, rc1);

    /* Error 2: register host fn with NULL fn argument. */
    urbi_register(&vm, realm, "badFn", NULL);
    rc2 = urbi_last_error(&vm, &info2);
    UASSERT_EQ((int)URBI_ERR_INVALID_ARG, rc2);

    /* Most-recent is now the INVALID_ARG error. */
    UASSERT(info1.message != info2.message);  /* different ring entries */
    UASSERT_EQ((int)URBI_ERR_INVALID_ARG, info2.code);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_error_string_lifetime_suite(void)
{
    utest_run("error_string_lifetime: ptr valid after non-failing call",
              string_ptr_valid_after_non_error_call);
    utest_run("error_string_lifetime: content unchanged after success call",
              string_content_unchanged_after_success_call);
    utest_run("error_string_lifetime: new error changes last_error result",
              new_error_changes_last_error_result);
}
