/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: install_watcher_runtime skeleton + re-entry guard (T34).
 *
 * T34 cases:
 *   1. install_returns_recursive_when_in_eval:
 *      vm->in_watcher_eval=1 → install returns URBI_INSTALL_RECURSIVE and
 *      fires exactly one URBI_LOG_WARN containing "from within scratch-frame eval".
 *   2. install_returns_ok_normally:
 *      vm->in_watcher_eval=0 (default) → stub returns URBI_INSTALL_OK, no warn. */

#include "utest.h"
#include "uvm.h"
#include "ustrand.h"
#include "watcher/uwatcher.h"          /* UWATCHER_AT */
#include "watcher/uwatcher_install.h"  /* install_watcher_runtime, UWatcherInstallResult */
#include "urbi/urbi.h"                 /* URBI_LOG_WARN */

#include <string.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Log capture
 * =================================================================== */

static int g_warn_count;
static int g_total_count;
/* last message buffer */
static char g_last_msg[256];

static void
capture_log(struct UVM *vm, int level, const char *fmt, ...)
{
    (void)vm;
    g_total_count++;
    if (level == URBI_LOG_WARN) {
        g_warn_count++;
        /* Copy format string as a stand-in for the message. */
        strncpy(g_last_msg, fmt, sizeof(g_last_msg) - 1);
        g_last_msg[sizeof(g_last_msg) - 1] = '\0';
    }
}

static void reset_log(struct UVM *vm)
{
    g_warn_count  = 0;
    g_total_count = 0;
    g_last_msg[0] = '\0';
    vm->host_log_fn = capture_log;
}

/* ===================================================================
 * Test cases
 * =================================================================== */

/* 1. install_returns_recursive_when_in_eval
 *
 * With vm->in_watcher_eval = 1, install_watcher_runtime must:
 *   - Return URBI_INSTALL_RECURSIVE.
 *   - Fire exactly one URBI_LOG_WARN containing "from within scratch-frame eval". */
UTEST(install_returns_recursive_when_in_eval)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    reset_log(&vm);

    vm.in_watcher_eval = 1;

    UWatcherInstallResult r = install_watcher_runtime(
        &vm, &s, UWATCHER_AT, NULL, NULL, NULL, NULL);

    UASSERT_EQ((int)r, (int)URBI_INSTALL_RECURSIVE);
    UASSERT_EQ(g_warn_count, 1);
    /* Verify the message mentions the expected phrase. */
    UASSERT(strstr(g_last_msg, "from within scratch-frame eval") != NULL);

    vm.in_watcher_eval = 0;

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* 2. install_returns_ok_normally
 *
 * With default state (in_watcher_eval == 0), the skeleton must return OK
 * and emit no warnings. */
UTEST(install_returns_ok_normally)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    reset_log(&vm);

    UWatcherInstallResult r = install_watcher_runtime(
        &vm, &s, UWATCHER_AT, NULL, NULL, NULL, NULL);

    UASSERT_EQ((int)r, (int)URBI_INSTALL_OK);
    UASSERT_EQ(g_warn_count, 0);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_install_skeleton_suite(void)
{
    printf("test_install_skeleton\n");
    utest_run("install_returns_recursive_when_in_eval",
              install_returns_recursive_when_in_eval);
    utest_run("install_returns_ok_normally",
              install_returns_ok_normally);
}
