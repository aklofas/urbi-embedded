/* SPDX-License-Identifier: BSD-3-Clause */
/* spawn_body_coroutine — M3 stub for watcher body execution.  Row 11.
 *
 * Called by watcher_eval_dirty when a watcher fires (rising edge for
 * AT/AT_SYNC, level for WHENEVER).
 *
 * M3 stub contract (spec §6.8):
 *  - If vm->test_watcher_fire_hook is non-NULL, delegate to it.
 *  - Otherwise: no-op.  M3 has no urbiscript `at` syntax to install
 *    watchers from scripts; production-mode fires are unreachable
 *    without explicit test-hook setup.
 *
 * M5 deferral (real implementation):
 *  1. Pool-alloc body strand from coroutine pool
 *     (URBI_COROUTINE_POOL_SIZE; exhaust per w->exhaust_policy).
 *  2. Inherit ambient tag chain (chain = [w->owning_tag]) via spec §4.
 *  3. Bind w->body as the entry closure.
 *  4. urbi_strand_start(new_body). */

#include "uwatcher.h"
#include "uvm.h"
#include "urbi.h"          /* URBI_ASSERT_NOT_ISR */

void
spawn_body_coroutine(struct UVM *vm, struct UWatcher *w)
{
    URBI_ASSERT_NOT_ISR(vm);

    if (vm->test_watcher_fire_hook != NULL) {
        vm->test_watcher_fire_hook(vm, w);
        return;
    }
    /* M5 owns the real implementation; no-op at M3.
     * Spec §6.8 plan code shows URBI_INTERNAL_ASSERT here — we keep no-op
     * because a hosted assert() would crash production binaries, and
     * URBI_BUILD_TESTS gating doesn't exist in this codebase.  T34 + T35
     * reviews accepted this judgment call. */
    (void)vm;
    (void)w;
}
