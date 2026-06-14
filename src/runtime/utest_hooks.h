/* SPDX-License-Identifier: BSD-3-Clause */
/* === W3/v0.10.4: UTestHooks extracted from struct UVM per audit-1 F8. ===
 *
 * Bundles the four watcher/install test-seam function pointers that were
 * previously inline fields on struct UVM.  Structured form allows future
 * addition of test seams without growing the UVM root struct.
 *
 * Allocation:
 *   utest_hooks_create is called from urbi_vm_init; vm->test_hooks is
 *   always non-NULL in hosted builds where an allocator is wired.
 *   Production callers in src/watcher/ NULL-check vm->test_hooks before
 *   dereferencing, matching the pre-W3 pattern of checking each hook
 *   function pointer individually.
 *
 * Test callers set hooks via vm->test_hooks->watcher_condition etc.
 * after urbi_vm_init (which guarantees vm->test_hooks != NULL).
 * === */

#ifndef URBI_TEST_HOOKS_H
#define URBI_TEST_HOOKS_H

#include "urbi/types.h"

struct UVM;
struct UWatcher;
struct UClosure;
struct UEvent;

typedef struct UTestHooks {
    /* test_watcher_condition_hook: replaces invoke_condition_closure when
     * non-NULL.  Tests install this to feed deterministic condition values
     * for edge/level firing tests.  NULL → invoke_condition_closure runs
     * the real cond closure. */
    UValue (*watcher_condition)(struct UVM *vm, struct UWatcher *w);

    /* test_watcher_fire_hook: invoked by spawn_body_coroutine when non-NULL.
     * Tests install this to observe watcher body fires.  NULL → real body
     * spawn. */
    void   (*watcher_fire)(struct UVM *vm, struct UWatcher *w);

    /* test_watcher_onleave_hook: test seam for run_watcher_onleave.
     * NULL → run_watcher_onleave runs the real onleave path. */
    void   (*watcher_onleave)(struct UVM *vm, struct UWatcher *w);

    /* test_install_cond_hook: install-time cond-eval test seam.
     * When non-NULL, install_watcher_runtime calls this hook instead of the
     * real urbi_run_closure_on_scratch.  Signature:
     *   hook(vm, cond, out_result, out_threw)
     * NULL → real urbi_run_closure_on_scratch. */
    void   (*install_cond)(struct UVM *vm, struct UClosure *cond,
                           UValue *out_result, int *out_threw);

    /* after_sync_body: called by run_event_body_on_scratch after a sync event
     * body finishes (in_scratch already cleared).  Tests use this to install
     * new watchers mid-emit to exercise the SCHED-18 tail-pin invariant.
     * NULL → no-op.  w->event carries the event being emitted. */
    void   (*after_sync_body)(struct UVM *vm, struct UWatcher *w);
} UTestHooks;

/* utest_hooks_create: allocate a zeroed UTestHooks for vm.  All function
 * pointer fields initialise to NULL (hook disabled → real path taken by
 * callers in src/watcher/).  Returns NULL on OOM or when no allocator is
 * wired (freestanding builds); callers NULL-check vm->test_hooks.
 * utest_hooks_destroy: free th.  NULL-tolerant. */
UTestHooks *utest_hooks_create(struct UVM *vm);
void        utest_hooks_destroy(struct UVM *vm, UTestHooks *th);

#endif /* URBI_TEST_HOOKS_H */
