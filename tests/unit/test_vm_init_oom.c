/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_vm_init_oom.c — T23 / VM-010 + VM-024.
 *
 * Closes VM-010 (smell) by exercising the new urbi_vm_init UErrCode return:
 * the three OOM-prone allocations (event_ring, deferred_slot_changes,
 * watcher_scratch_frame) now surface URBI_ERR_OOM to the caller rather than
 * silently leaving NULL pointers behind.
 *
 * Closes VM-024 (cov) by driving the failing-allocator pattern at multiple
 * allocation indices.  Each index that triggers URBI_ERR_OOM proves the
 * matching use-site guard is reachable; the destroy-after-OOM coverage
 * proves urbi_vm_destroy stays safe on partial-init state. */

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Failing allocator: returns NULL on the (fail_at)th allocating call
 * (zero-based), realloc-passes everything else.  Counts only calls with
 * nbytes > 0 (free-shaped calls are pass-through).
 *
 * UVMAllocFn signature per <urbi/types.h>:
 *   void *(*UVMAllocFn)(void *ptr, size_t nbytes, void *ud);
 */
typedef struct {
    int fail_at;     /* allocation index (0-based) that returns NULL */
    int call_count;  /* incremented on each call where nbytes > 0 */
} FailingAllocCtx;

static void *failing_alloc(void *ptr, size_t nbytes, void *ud) {
    FailingAllocCtx *c = (FailingAllocCtx *)ud;
    if (nbytes == 0) {
        free(ptr);
        return NULL;
    }
    if (c->call_count++ == c->fail_at) {
        return NULL;
    }
    return realloc(ptr, nbytes);
}

UTEST(uvm_init_returns_ok_on_success) {
    UVM vm;
    FailingAllocCtx c = {.fail_at = 99999, .call_count = 0};
    int rc = urbi_vm_init(&vm, failing_alloc, &c);
    UASSERT_EQ(URBI_OK, rc);
    urbi_vm_destroy(&vm);
}

/* Single [0,30) bisect pass that proves all three OOM contracts at once.
 * Previously three separate tests each walked the same range calling
 * urbi_vm_init+destroy 30 times (~90 cycles total); they only differed in
 * the post-condition asserted, so one pass records all three signals.
 *
 *   1. found_oom            — VM-010: at least one early allocation surfaces
 *                             URBI_ERR_OOM (the OOM guards are reachable).
 *   2. oom_index_count >= 2 — VM-024 coverage: at least two distinct
 *                             call-indices surface OOM, proving multiple
 *                             use-site guards fire (documented paths:
 *                             event_ring / deferred_slot_changes /
 *                             op_overload_ic, plus watcher_pool, intern
 *                             table, and other early allocs).
 *   3. destroy-safety       — urbi_vm_destroy MUST be callable on every
 *                             partial-init state without crashing; it runs
 *                             on each iteration below, so reaching the final
 *                             assertion proves no destroy aborted. */
UTEST(uvm_init_oom_bisect) {
    int found_oom = 0;
    int oom_index_count = 0;
    for (int fail_at = 0; fail_at < 30; fail_at++) {
        UVM vm;
        FailingAllocCtx c = {.fail_at = fail_at, .call_count = 0};
        memset(&vm, 0, sizeof(vm));
        int rc = urbi_vm_init(&vm, failing_alloc, &c);
        urbi_vm_destroy(&vm);  /* MUST NOT crash on any partial-init state */
        if (rc == URBI_ERR_OOM) {
            found_oom = 1;
            oom_index_count++;
        }
    }
    UASSERT(found_oom);            /* VM-010: guards reachable */
    UASSERT(oom_index_count >= 2); /* VM-024: multiple use-site guards fire */
}

void test_vm_init_oom_suite(void) {
    utest_run("uvm_init_returns_ok_on_success",
              uvm_init_returns_ok_on_success);
    utest_run("uvm_init_oom_bisect (found_oom + multi-path + destroy-safe)",
              uvm_init_oom_bisect);
}
