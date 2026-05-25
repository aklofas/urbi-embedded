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
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "watcher/uwatcher.h"          /* UWATCHER_AT, uwatcher_pool_alloc */
#include "watcher/uwatcher_install.h"  /* install_watcher_runtime, UWatcherInstallResult */
#include "gc/ugc.h"                    /* UCell */
#include "gc/ugc_incremental.h"        /* UGC_HAS_WATCHER_OBSERVER */
#include "tag/utag.h"                      /* UTag, member_watchers_head */
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
capture_log(struct UVM *vm, void *ud, int level, const char *fmt, ...)
{
    (void)vm; (void)ud;
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

/* === Forward declarations needed by tests that call the T39 hook early === */
static UCell g_t39_cell;
static void hook_plant_one_cell(struct UVM *vm, struct UClosure *cond,
                                UValue *out_result, int *out_threw);

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

    urbi_vm_init(&vm, NULL, NULL);
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
    urbi_vm_destroy(&vm);
}

/* 2. install_returns_ok_normally
 *
 * With default state (in_watcher_eval == 0) and a cond hook that plants one
 * observable cell into the read-set, install must return URBI_INSTALL_OK.
 *
 * W0/v0.10.2 update: empty read-set is now a hard reject (Phase 5a).  The
 * hook_plant_one_cell is required for a clean OK path.  The empty-read-set
 * rejection is tested separately (install_warns_on_empty_readset below). */
UTEST(install_returns_ok_normally)
{
    UVM vm;
    UStrand s;

    urbi_vm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    reset_log(&vm);

    /* Plant one cell so the read-set is non-empty. */
    g_t39_cell.gc_byte = 0;
    vm.test_install_cond_hook = hook_plant_one_cell;

    UWatcherInstallResult r = install_watcher_runtime(
        &vm, &s, UWATCHER_AT, NULL, NULL, NULL, NULL);

    UASSERT_EQ((int)r, (int)URBI_INSTALL_OK);
    UASSERT_EQ(g_warn_count, 0);  /* no warn when read-set is non-empty */

    vm.test_install_cond_hook = NULL;

    /* Clean up the installed watcher. */
    if (vm.active_watchers_head != NULL)
        urbi_watcher_unregister_internal(&vm, vm.active_watchers_head);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
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
 * W0/v0.10.2 update: when trace_read_set_count == 0 (no slot reads) for an
 * AT/WHENEVER watcher, install must now:
 *   - Return URBI_INSTALL_NO_OBSERVABLE_CELLS (hard reject; not OK).
 *   - Fire exactly one URBI_LOG_WARN containing "no observable cells".
 *   - NOT install a watcher (active_watchers_head stays NULL).
 *
 * Prior behavior was warn-and-proceed; this changed at W0/v0.10.2 to close
 * reactive F1 (whenever (e?) was silently no-op'd by the old path). */
UTEST(install_warns_on_empty_readset)
{
    UVM    vm;
    UStrand s;

    urbi_vm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    reset_log(&vm);

    /* No hook → trace_read_set_count stays 0 (no slot reads). */
    UWatcherInstallResult r = install_watcher_runtime(
        &vm, &s, UWATCHER_AT, NULL, NULL, NULL, NULL);

    UASSERT_EQ((int)URBI_INSTALL_NO_OBSERVABLE_CELLS, (int)r);
    UASSERT_EQ(1, g_warn_count);
    UASSERT(strstr(g_last_msg, "no observable cells") != NULL);
    /* No watcher installed — active_watchers_head must still be NULL. */
    UASSERT(vm.active_watchers_head == NULL);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* 4. install_returns_oom_pool_when_exhausted
 *
 * With the pool drained AND a non-empty read-set (hook_plant_one_cell passes
 * Phase 5a), install must return URBI_INSTALL_OOM_POOL and fire a WARN.
 *
 * W0/v0.10.2 update: the cond hook is now required; without it, Phase 5a
 * rejects with INSTALL_NO_OBSERVABLE_CELLS before reaching the pool check. */
UTEST(install_returns_oom_pool_when_exhausted)
{
    UVM     vm;
    UStrand s;
    UWatcher *held[URBI_WATCHER_POOL_SIZE];
    int i;

    urbi_vm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    reset_log(&vm);

    test_drain_watcher_pool(&vm, held);

    /* Plant one cell so Phase 5a (empty-read-set check) is bypassed. */
    g_t39_cell.gc_byte = 0;
    vm.test_install_cond_hook = hook_plant_one_cell;

    UWatcherInstallResult r = install_watcher_runtime(
        &vm, &s, UWATCHER_AT, NULL, NULL, NULL, NULL);

    UASSERT_EQ((int)URBI_INSTALL_OOM_POOL, (int)r);
    UASSERT(g_warn_count >= 1);

    vm.test_install_cond_hook = NULL;

    /* Return pool slots so urbi_vm_destroy is clean. */
    for (i = 0; i < URBI_WATCHER_POOL_SIZE; i++) {
        if (held[i] != NULL)
            urbi_watcher_unregister_internal(&vm, held[i]);
    }

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* 5. install_initializes_watcher_fields
 *
 * After a successful install, the watcher at vm->active_watchers_head must
 * have its mode, flags, and realm wired to the install arguments.
 *
 * W0/v0.10.2 update: hook_plant_one_cell is required to pass Phase 5a
 * (empty-read-set rejection). */
UTEST(install_initializes_watcher_fields)
{
    UVM     vm;
    UStrand s;

    urbi_vm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    reset_log(&vm);

    /* Plant one cell so the install succeeds through Phase 5a. */
    g_t39_cell.gc_byte = 0;
    vm.test_install_cond_hook = hook_plant_one_cell;

    UWatcherInstallResult r = install_watcher_runtime(
        &vm, &s, UWATCHER_AT, NULL, NULL, NULL, NULL);

    UASSERT_EQ((int)URBI_INSTALL_OK, (int)r);

    UWatcher *w = vm.active_watchers_head;
    UASSERT(w != NULL);
    UASSERT_EQ((int)UWATCHER_AT,          (int)w->mode);
    UASSERT_EQ((int)URBI_WATCHER_ACTIVE,  (int)(w->flags & URBI_WATCHER_ACTIVE));
    UASSERT(w->realm == s.realm);

    vm.test_install_cond_hook = NULL;

    /* Clean up the installed watcher. */
    urbi_watcher_unregister_internal(&vm, w);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T39 helpers
 * =================================================================== */

/* g_t39_cell is forward-declared near the top of this file (before test 2).
 * hook_plant_one_cell is also forward-declared there; body definition follows. */

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

    urbi_vm_init(&vm, NULL, NULL);
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
    urbi_vm_destroy(&vm);
}

/* 7. install_appends_watcher_to_active_and_tag_lists
 *
 * After install, the watcher must appear in vm->active_watchers_head and
 * in owning_tag->member_watchers_head (which equals realm->tag here). */
UTEST(install_appends_watcher_to_active_and_tag_lists)
{
    UVM    vm;
    UStrand s;

    urbi_vm_init(&vm, NULL, NULL);

    /* Create a realm (gives us a non-NULL realm->tag for owning_tag). */
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    ustrand_init(&s, &vm);
    s.realm = r;  /* wire realm so resolve_owning_tag falls through to realm->tag */

    reset_log(&vm);

    /* W0/v0.10.2: Phase 5a rejects empty read-sets; plant one cell. */
    g_t39_cell.gc_byte = 0;
    vm.test_install_cond_hook = hook_plant_one_cell;

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
    urbi_vm_destroy(&vm);
}

/* WATCH-005: hook that populates trace_read_set_count without arming the
 * VM trace dispatch.  Pairs with install_oom_pool_clears_trace_state
 * below: the hook sets trace_read_set_count to a non-zero value during
 * Phase 3, then Phase 5b's pool-alloc fails and we observe whether
 * Phase 5b's fall-through resets trace state. */
static void
hook_populate_trace(struct UVM *vm, struct UClosure *cond,
                    UValue *out_result, int *out_threw)
{
    UValue nil = {0};
    (void)cond;
    /* Simulate the trace probe collecting one slot read. */
    vm->trace_read_set_count = 1U;
    vm->trace_read_set[0]    = (struct UCell *)0x1;  /* sentinel */
    *out_result = nil;
    *out_threw  = 0;
}

/* WATCH-005: install_oom_pool_clears_trace_state
 *
 * When install_watcher_runtime fails at Phase 5b (pool exhaustion), it
 * must NOT leak trace_read_set_count populated during Phase 3 — any
 * unrelated reader (or the next install attempt before its own reset)
 * would observe stale state otherwise.  The audit (WATCH-005) flagged
 * this as benign-today (next install resets at lines 149-150) but
 * fragile to refactors.
 *
 * Pre-fix: trace_read_set_count survives the OOM_POOL return.
 * Post-fix: trace state is cleared on fall-through. */
UTEST(install_oom_pool_clears_trace_state)
{
    UVM     vm;
    UStrand s;
    UWatcher *held[URBI_WATCHER_POOL_SIZE];
    int i;

    urbi_vm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);
    reset_log(&vm);

    /* Drain the pool BEFORE installing the cond hook so the hook will
     * have run (Phase 3) but Phase 5b alloc will fail. */
    test_drain_watcher_pool(&vm, held);

    vm.test_install_cond_hook = hook_populate_trace;

    UWatcherInstallResult r = install_watcher_runtime(
        &vm, &s, UWATCHER_AT, NULL, NULL, NULL, NULL);

    UASSERT_EQ((int)URBI_INSTALL_OOM_POOL, (int)r);
    /* Post-fix invariant: trace state cleared on fall-through. */
    UASSERT_EQ(0, (int)vm.trace_read_set_count);
    UASSERT_EQ(0, (int)vm.trace_overflow);

    vm.test_install_cond_hook = NULL;

    /* Return pool slots so urbi_vm_destroy is clean. */
    for (i = 0; i < URBI_WATCHER_POOL_SIZE; i++) {
        if (held[i] != NULL)
            urbi_watcher_unregister_internal(&vm, held[i]);
    }

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
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
    utest_run("install_oom_pool_clears_trace_state",
              install_oom_pool_clears_trace_state);
    utest_run("install_initializes_watcher_fields",
              install_initializes_watcher_fields);
    utest_run("install_marks_observed_cells_with_bit6",
              install_marks_observed_cells_with_bit6);
    utest_run("install_appends_watcher_to_active_and_tag_lists",
              install_appends_watcher_to_active_and_tag_lists);
}
