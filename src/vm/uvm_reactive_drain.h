/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_reactive_drain.h — vm_reactive_drain: centralised reactive pump helper.
 *
 * Refactor-3 VM-05/VM-20, SCHED-02, SCHED-12.
 *
 * Called from:
 *   - urbi_step pre-loop pump (SCHED-02: host writes reach parked waituntil)
 *   - dispatch safepoint: replaces the open-coded onleave/drain/eval trio
 *   - post-native-call arm in OP_CALL: replaces the open-coded drain/eval block
 *   - vm_arith_method_fallback{,_unary} / vm_cmp_method_fallback on OK path
 *
 * Guard (SCHED-12): no-op when already inside an eval/install/scratch context.
 * The enclosing pass will drain on return.  Previously each call site had its
 * own open-coded guard; centralising removes the risk of a new call site
 * forgetting the guard. */

#ifndef URBI_VM_REACTIVE_DRAIN_H
#define URBI_VM_REACTIVE_DRAIN_H

#include "vm/uvm.h"
#include "watcher/uwatcher.h"       /* drain_pending_onleave_queue, watcher_eval_dirty */
#include "changed/uchanged_node.h"  /* urbi_drain_deferred_slot_changes */

/* vm_reactive_drain — pump the reactive pipeline one pass.
 *
 * Order (spec §6.5):
 *   1. pending-onleave FIFO (tag-stop callbacks)
 *   2. deferred slot-change ring
 *   3. dirty watcher eval
 *
 * No-op when vm->watchers->in_eval / in_install / in_scratch is set (SCHED-12).
 * Freestanding-safe: no heap allocation, no I/O. */
static inline void
vm_reactive_drain(struct UVM *vm)
{
    if (vm->watchers->in_eval || vm->watchers->in_install
        || vm->watchers->in_scratch)
        return;
    if (vm->pending_onleave_head) drain_pending_onleave_queue(vm);
    urbi_drain_deferred_slot_changes(vm);
    if (vm->watchers->dirty_count > 0) watcher_eval_dirty(vm);
}

#endif /* URBI_VM_REACTIVE_DRAIN_H */
