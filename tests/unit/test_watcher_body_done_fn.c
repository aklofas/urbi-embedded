/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_watcher_body_done_fn.c — T33: reactive hook seam.
 *
 * Verifies the urbi_set_watcher_body_done_fn setter installs / uninstalls
 * the callback on UVM.watcher_body_done_fn and is NULL-safe on a NULL vm.
 *
 * Wave 1 ships only the seam (field exists, setter wires it, NULL-safe).
 * Wave 2 (ESP-IDF port) lands end-to-end tests that fire actual watcher
 * bodies and observe the callback. */

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

static int callback_fire_count;
static int last_completion_status;

static void
test_callback(struct UVM *vm, void *ud, urbi_watcher_handle_t handle, int status)
{
    (void)vm; (void)ud; (void)handle;
    callback_fire_count++;
    last_completion_status = status;
}

UTEST(watcher_body_done_fn_setter_installs_callback)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(rc, URBI_OK);

    /* Default is NULL after urbi_vm_init. */
    UASSERT(vm.watcher_body_done_fn == NULL);

    /* Install. */
    urbi_set_watcher_body_done_fn(&vm, test_callback, NULL);
    UASSERT(vm.watcher_body_done_fn == test_callback);

    /* Uninstall. */
    urbi_set_watcher_body_done_fn(&vm, NULL, NULL);
    UASSERT(vm.watcher_body_done_fn == NULL);

    urbi_vm_destroy(&vm);
}

UTEST(watcher_body_done_fn_null_safe)
{
    /* Setter with NULL vm must not crash. */
    urbi_set_watcher_body_done_fn(NULL, test_callback, NULL);
    /* PASS if we reach here. */
    UASSERT(1);
}

void
test_watcher_body_done_fn_suite(void)
{
    printf("test_watcher_body_done_fn\n");
    callback_fire_count = 0;
    last_completion_status = 0;
    utest_run("watcher_body_done_fn_setter_installs_callback",
              watcher_body_done_fn_setter_installs_callback);
    utest_run("watcher_body_done_fn_null_safe",
              watcher_body_done_fn_null_safe);
}
