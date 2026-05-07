/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: WAITUNTIL strand-block + immediate-wake fast path (T40).
 * Spec #2 §7.7.
 *
 * Cases:
 *   1. waituntil_immediate_wake_when_cond_starts_true:
 *      When the cond hook returns a truthy value, install must unregister the
 *      watcher immediately and leave the strand NOT in WAITING state.
 *      vm->active_watchers_head must be NULL after install (watcher freed).
 *
 *   2. waituntil_blocks_strand_when_cond_starts_false:
 *      When the cond hook returns a falsy value, install must set the strand
 *      state to USTRAND_WAIT_WATCHER (0x32) and leave the watcher installed
 *      (vm->active_watchers_head non-NULL; waiter_strand == s). */

#include "utest.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"            /* USTRAND_WAIT_WATCHER, USTRAND_IS_WAITING */
#include "watcher/uwatcher.h"   /* UWATCHER_WAITUNTIL, urbi_watcher_unregister_internal */
#include "watcher/uwatcher_install.h"  /* install_watcher_runtime, UWatcherInstallResult */
#include "urbi/urbi.h"          /* URBI_LOG_WARN */
#include "umodule.h"            /* UValue, UVAL_BOOL, UVAL_NIL */

#include <string.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Log capture
 * =================================================================== */

static int g_warn_count;

static void
capture_log(struct UVM *vm, int level, const char *fmt, ...)
{
    (void)vm; (void)fmt;
    if (level == URBI_LOG_WARN)
        g_warn_count++;
}

static void reset_log(struct UVM *vm)
{
    g_warn_count = 0;
    vm->host_log_fn = capture_log;
}

/* ===================================================================
 * Cond hooks
 * =================================================================== */

/* hook_true: cond hook that returns bool-true (truthy). */
static void
hook_true(struct UVM *vm, struct UClosure *cond,
          UValue *out_result, int *out_threw)
{
    (void)vm; (void)cond;
    out_result->kind = (uint8_t)UVAL_BOOL;
    out_result->v.i  = 1;
    *out_threw = 0;
}

/* hook_false: cond hook that returns bool-false (falsy). */
static void
hook_false(struct UVM *vm, struct UClosure *cond,
           UValue *out_result, int *out_threw)
{
    (void)vm; (void)cond;
    out_result->kind = (uint8_t)UVAL_BOOL;
    out_result->v.i  = 0;
    *out_threw = 0;
}

/* ===================================================================
 * Test cases
 * =================================================================== */

/* 1. waituntil_immediate_wake_when_cond_starts_true
 *
 * When the cond evaluates truthy at install, install must:
 *   - Return URBI_INSTALL_OK.
 *   - Unregister the watcher immediately (vm->active_watchers_head == NULL).
 *   - Leave the strand NOT in WAITING state. */
UTEST(waituntil_immediate_wake_when_cond_starts_true)
{
    UVM    vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    reset_log(&vm);

    vm.test_install_cond_hook = hook_true;

    UWatcherInstallResult r = install_watcher_runtime(
        &vm, &s, UWATCHER_WAITUNTIL, NULL, NULL, NULL, &s);

    UASSERT_EQ((int)URBI_INSTALL_OK, (int)r);
    /* Watcher unregistered immediately — active list must be empty. */
    UASSERT(vm.active_watchers_head == NULL);
    /* Strand must NOT be in WAITING state. */
    UASSERT(!USTRAND_IS_WAITING(&s));

    vm.test_install_cond_hook = NULL;
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* 2. waituntil_blocks_strand_when_cond_starts_false
 *
 * When the cond evaluates falsy at install, install must:
 *   - Return URBI_INSTALL_OK.
 *   - Leave the strand in USTRAND_WAIT_WATCHER state (0x32).
 *   - Leave the watcher installed (vm->active_watchers_head non-NULL).
 *   - Have watcher->waiter_strand == &s. */
UTEST(waituntil_blocks_strand_when_cond_starts_false)
{
    UVM    vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    reset_log(&vm);

    vm.test_install_cond_hook = hook_false;

    UWatcherInstallResult r = install_watcher_runtime(
        &vm, &s, UWATCHER_WAITUNTIL, NULL, NULL, NULL, &s);

    UASSERT_EQ((int)URBI_INSTALL_OK, (int)r);
    /* Strand must be in WAITING state with WATCHER reason. */
    UASSERT(USTRAND_IS_WAITING(&s));
    UASSERT_EQ((int)USTRAND_WAIT_WATCHER, (int)s.state);
    /* Watcher must be installed and waiter_strand wired. */
    UASSERT(vm.active_watchers_head != NULL);
    UASSERT(vm.active_watchers_head->waiter_strand == &s);

    /* Clean up: unregister the installed watcher. */
    urbi_watcher_unregister_internal(&vm, vm.active_watchers_head);

    vm.test_install_cond_hook = NULL;
    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_waituntil_install_suite(void)
{
    printf("test_waituntil_install\n");
    utest_run("waituntil_immediate_wake_when_cond_starts_true",
              waituntil_immediate_wake_when_cond_starts_true);
    utest_run("waituntil_blocks_strand_when_cond_starts_false",
              waituntil_blocks_strand_when_cond_starts_false);
}
