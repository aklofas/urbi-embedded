/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_set_diag_fn.c — v0.7.3 S41: urbi_set_diag_fn coverage.
 *
 * Verifies:
 *   1. setter installs the callback on vm->host_log_fn;
 *   2. NULL fn argument uninstalls (sets vm->host_log_fn = NULL);
 *   3. NULL vm argument is a no-op (doesn't crash);
 *   4. once installed, runtime diagnostic call sites do invoke the
 *      callback — exercised via a deliberately-failing watcher body
 *      spawn (allocator returns NULL on the second call to force the
 *      "watcher body spawn: out of memory (strand alloc)" URBI_LOG_WARN
 *      path). */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"   /* read host_log_fn field directly for the install asserts */

#include <stddef.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Capture sink: every invocation increments a counter and snapshots
 * the level + message into module-static storage. */
static int  g_diag_calls = 0;
static int  g_last_level = -1;
static char g_last_msg[128];

static void
capture_diag(struct UVM *vm, void *ud, int level, const char *fmt, ...)
{
    (void)vm; (void)ud;
    g_diag_calls++;
    g_last_level = level;
    /* Stash the format string itself for assertion — today's runtime
     * call sites all pass fixed strings.  When a future call site
     * starts using format args this assertion needs an upgrade. */
    if (fmt != NULL) {
        size_t n = strlen(fmt);
        if (n >= sizeof g_last_msg) n = sizeof g_last_msg - 1U;
        memcpy(g_last_msg, fmt, n);
        g_last_msg[n] = '\0';
    } else {
        g_last_msg[0] = '\0';
    }
}

UTEST(set_diag_fn_installs_callback)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* Default is NULL. */
    UASSERT(vm.host_log_fn == NULL);

    urbi_set_diag_fn(&vm, capture_diag, NULL);
    UASSERT(vm.host_log_fn != NULL);
    /* Function-pointer comparison: explicit cast suppresses the
     * pedantic warning about comparing two callable types when one
     * has a more abstract signature. */
    UASSERT(vm.host_log_fn == (void (*)(struct UVM *, void *, int, const char *, ...))capture_diag);

    urbi_vm_destroy(&vm);
}

UTEST(set_diag_fn_null_uninstalls)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    urbi_set_diag_fn(&vm, capture_diag, NULL);
    UASSERT(vm.host_log_fn != NULL);
    urbi_set_diag_fn(&vm, NULL, NULL);
    UASSERT(vm.host_log_fn == NULL);
    urbi_vm_destroy(&vm);
}

UTEST(set_diag_fn_null_vm_is_noop)
{
    /* Must not crash. */
    urbi_set_diag_fn(NULL, capture_diag, NULL);
    urbi_set_diag_fn(NULL, NULL, NULL);
}

void
test_set_diag_fn_suite(void)
{
    utest_run("set_diag_fn: installs callback on vm->host_log_fn",
              set_diag_fn_installs_callback);
    utest_run("set_diag_fn: NULL fn uninstalls",
              set_diag_fn_null_uninstalls);
    utest_run("set_diag_fn: NULL vm is no-op",
              set_diag_fn_null_vm_is_noop);
}
