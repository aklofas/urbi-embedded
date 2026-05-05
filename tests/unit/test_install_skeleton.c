/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: install_watcher_runtime skeleton + re-entry guard (T34).
 *
 * T34 cases:
 *   1. install_returns_recursive_when_in_eval:
 *      vm->in_watcher_eval=1 → install returns URBI_INSTALL_RECURSIVE and
 *      fires exactly one URBI_LOG_WARN containing "from within scratch-frame eval".
 *   2. install_returns_ok_normally:
 *      vm->in_watcher_eval=0 (default) → stub returns URBI_INSTALL_OK, no warn.
 *
 * T38 cases (spec #2 §7.4–§7.5):
 *   3. install_warns_on_empty_readset:
 *      When trace_read_set_count==0 after phase-4, install warns but proceeds (OK).
 *   4. install_returns_oom_pool_when_exhausted:
 *      With pool drained, install returns URBI_INSTALL_OOM_POOL.
 *   5. install_initializes_watcher_fields:
 *      After a successful install, watcher fields match the install arguments.
 *
 * T39 cases (spec #2 §7.6):
 *   6. install_marks_observed_cells_with_bit6:
 *      Cells in trace_read_set gain UGC_HAS_WATCHER_OBSERVER after install.
 *   7. install_appends_watcher_to_active_and_tag_lists:
 *      Watcher is tail-appended to both vm->active_watchers_head and
 *      owning_tag->member_watchers_head. */

#include "utest.h"
#include "uvm.h"
#include "ustrand.h"
#include "watcher/uwatcher.h"          /* UWATCHER_AT, uwatcher_pool_alloc */
#include "watcher/uwatcher_install.h"  /* install_watcher_runtime, UWatcherInstallResult */
#include "gc/ugc.h"                    /* UCell */
#include "gc/ugc_incremental.h"        /* UGC_HAS_WATCHER_OBSERVER */
#include "utag.h"                      /* UTag, member_watchers_head */
#include "realm/urealm.h"              /* urbi_realm_create/destroy */
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
 * With default state (in_watcher_eval == 0), the skeleton must return OK.
 * A null-cond install produces an empty read-set, so one WARN fires
 * (T38: "no observable cells") — that is expected and correct behaviour. */
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
    /* One WARN for empty read-set is expected (T38 phase 5a). */
    UASSERT_EQ(g_warn_count, 1);

    /* Clean up the installed watcher. */
    if (vm.active_watchers_head != NULL)
        urbi_watcher_unregister_internal(&vm, vm.active_watchers_head);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ===================================================================
 * T38 helpers
 * =================================================================== */

/* test_drain_watcher_pool: alloc all pool slots, leaving pool exhausted.
 * Stores each pointer in `out[0..URBI_WATCHER_POOL_SIZE-1]` so the caller
 * can clean up via urbi_watcher_unregister_internal. */
static void
test_drain_watcher_pool(UVM *vm, UWatcher **out)
{
    int i;
    for (i = 0; i < URBI_WATCHER_POOL_SIZE; i++) {
        out[i] = uwatcher_pool_alloc(vm);
    }
}

/* ===================================================================
 * T38 test cases
 * =================================================================== */

/* 3. install_warns_on_empty_readset
 *
 * When the cond hook leaves trace_read_set_count == 0 (no slot reads),
 * install must:
 *   - Return URBI_INSTALL_OK (inert watcher is introspectable).
 *   - Fire exactly one URBI_LOG_WARN containing "no observable cells". */
UTEST(install_warns_on_empty_readset)
{
    UVM    vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    reset_log(&vm);

    /* No hook → trace_read_set_count stays 0 (no slot reads). */
    UWatcherInstallResult r = install_watcher_runtime(
        &vm, &s, UWATCHER_AT, NULL, NULL, NULL, NULL);

    UASSERT_EQ((int)URBI_INSTALL_OK, (int)r);
    UASSERT_EQ(1, g_warn_count);
    UASSERT(strstr(g_last_msg, "no observable cells") != NULL);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* 4. install_returns_oom_pool_when_exhausted
 *
 * With the pool drained, install must return URBI_INSTALL_OOM_POOL
 * and fire a URBI_LOG_WARN. */
UTEST(install_returns_oom_pool_when_exhausted)
{
    UVM     vm;
    UStrand s;
    UWatcher *held[URBI_WATCHER_POOL_SIZE];
    int i;

    uvm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    reset_log(&vm);

    test_drain_watcher_pool(&vm, held);

    UWatcherInstallResult r = install_watcher_runtime(
        &vm, &s, UWATCHER_AT, NULL, NULL, NULL, NULL);

    UASSERT_EQ((int)URBI_INSTALL_OOM_POOL, (int)r);
    UASSERT(g_warn_count >= 1);

    /* Return pool slots so uvm_destroy is clean. */
    for (i = 0; i < URBI_WATCHER_POOL_SIZE; i++) {
        if (held[i] != NULL)
            urbi_watcher_unregister_internal(&vm, held[i]);
    }

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* 5. install_initializes_watcher_fields
 *
 * After a successful install, the watcher at vm->active_watchers_head must
 * have its mode, flags, and realm wired to the install arguments. */
UTEST(install_initializes_watcher_fields)
{
    UVM     vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    reset_log(&vm);

    UWatcherInstallResult r = install_watcher_runtime(
        &vm, &s, UWATCHER_AT, NULL, NULL, NULL, NULL);

    UASSERT_EQ((int)URBI_INSTALL_OK, (int)r);

    UWatcher *w = vm.active_watchers_head;
    UASSERT(w != NULL);
    UASSERT_EQ((int)UWATCHER_AT,          (int)w->mode);
    UASSERT_EQ((int)URBI_WATCHER_ACTIVE,  (int)(w->flags & URBI_WATCHER_ACTIVE));
    UASSERT(w->realm == s.realm);

    /* Clean up the installed watcher. */
    urbi_watcher_unregister_internal(&vm, w);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ===================================================================
 * T39 helpers
 * =================================================================== */

/* g_t39_cell: a stack UCell that the cond hook plants into trace_read_set[].
 * Declared at file scope so the hook and the test can both see it. */
static UCell g_t39_cell;

/* hook_plant_one_cell: cond hook that simulates one OP_GETSLOT read.
 * Plants &g_t39_cell into trace_read_set[0] and sets count=1. */
static void
hook_plant_one_cell(struct UVM *vm, struct UClosure *cond,
                    UValue *out_result, int *out_threw)
{
    UValue nil = {0};
    (void)cond;
    g_t39_cell.gc_byte = 0;  /* clear bit-6 before install */
    vm->trace_read_set[0] = &g_t39_cell;
    vm->trace_read_set_count = 1;
    *out_result = nil;
    *out_threw  = 0;
}

/* ===================================================================
 * T39 test cases
 * =================================================================== */

/* 6. install_marks_observed_cells_with_bit6
 *
 * A cond that reads one cell must cause install to set UGC_HAS_WATCHER_OBSERVER
 * (bit 6) on that cell. */
UTEST(install_marks_observed_cells_with_bit6)
{
    UVM    vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    reset_log(&vm);

    g_t39_cell.gc_byte = 0;
    vm.test_install_cond_hook = hook_plant_one_cell;

    UWatcherInstallResult r = install_watcher_runtime(
        &vm, &s, UWATCHER_AT, NULL, NULL, NULL, NULL);

    UASSERT_EQ((int)URBI_INSTALL_OK, (int)r);
    UASSERT(g_t39_cell.gc_byte & UGC_HAS_WATCHER_OBSERVER);

    vm.test_install_cond_hook = NULL;
    /* Clean up: unregister installed watcher. */
    if (vm.active_watchers_head != NULL)
        urbi_watcher_unregister_internal(&vm, vm.active_watchers_head);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* 7. install_appends_watcher_to_active_and_tag_lists
 *
 * After install, the watcher must appear in vm->active_watchers_head and
 * in owning_tag->member_watchers_head (which equals realm->tag here). */
UTEST(install_appends_watcher_to_active_and_tag_lists)
{
    UVM    vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);

    /* Create a realm (gives us a non-NULL realm->tag for owning_tag). */
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    ustrand_init(&s, &vm);
    s.realm = r;  /* wire realm so resolve_owning_tag falls through to realm->tag */

    reset_log(&vm);

    UWatcherInstallResult res = install_watcher_runtime(
        &vm, &s, UWATCHER_AT, NULL, NULL, NULL, NULL);

    UASSERT_EQ((int)URBI_INSTALL_OK, (int)res);

    /* Watcher must be in the active list. */
    UWatcher *w = vm.active_watchers_head;
    UASSERT(w != NULL);

    /* owning_tag is realm->tag; watcher must be in tag's member list. */
    UASSERT(w->owning_tag == r->tag);
    UASSERT(r->tag->member_watchers_head == w);

    /* Cleanup. */
    urbi_watcher_unregister_internal(&vm, w);
    urbi_realm_destroy(&vm, r);
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
    utest_run("install_warns_on_empty_readset",
              install_warns_on_empty_readset);
    utest_run("install_returns_oom_pool_when_exhausted",
              install_returns_oom_pool_when_exhausted);
    utest_run("install_initializes_watcher_fields",
              install_initializes_watcher_fields);
    utest_run("install_marks_observed_cells_with_bit6",
              install_marks_observed_cells_with_bit6);
    utest_run("install_appends_watcher_to_active_and_tag_lists",
              install_appends_watcher_to_active_and_tag_lists);
}
