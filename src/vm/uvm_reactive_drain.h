/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_reactive_drain.h — vm_reactive_drain: centralised reactive pump helper.
 *
 * Refactor-3 VM-05/VM-20, SCHED-02, SCHED-12.
 *
 * Called from:
 *   - urbi_step pre-loop idle drain (SCHED-02: host writes reach parked
 *     waituntil) — bounded_whenever = 1
 *   - urbi_step post-loop Step-4b drain (flat strands that completed without a
 *     safepoint; cascade resolution within the step) — bounded_whenever = 1
 *   - dispatch safepoint: replaces the open-coded onleave/drain/eval trio
 *     — bounded_whenever = 0 (active level-trigger)
 *   - post-native-call arm in OP_CALL — bounded_whenever = 0
 *   - vm_arith_method_fallback{,_unary} / vm_cmp_method_fallback OK path
 *     — bounded_whenever = 0
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
#include <stdint.h>

/* vm_reactive_drain — pump the reactive pipeline one pass.
 *
 * Order (spec §6.5):
 *   1. pending-onleave FIFO (tag-stop callbacks)
 *   2. deferred slot-change ring
 *   3. dirty watcher eval
 *
 * bounded_whenever selects the WHENEVER firing semantics for this pass:
 *   0 (level)  — active-dispatch drains (dispatcher safepoint, post-native
 *                call, operator-fallback): fire every truthy pass.  Bounded by
 *                the finite number of safepoints during active execution.
 *   1 (edge)   — idle / boundary drains (urbi_step pre-loop idle drain +
 *                post-loop Step-4b drain): fire only on the rising edge.  This
 *                is the SCHED-02 storm guard — a level-whenever whose body
 *                re-dirties its own observed object (observer_dirty is
 *                cell-agnostic) would otherwise spin unboundedly while the VM is
 *                idle.  See the WHENEVER branch in uwatcher_eval.c for the full
 *                termination argument.
 *
 * No-op when vm->watchers->in_eval / in_install / in_scratch is set (SCHED-12).
 * Freestanding-safe: no heap allocation, no I/O. */
static inline void
vm_reactive_drain(struct UVM *vm, int bounded_whenever)
{
    if (vm->watchers->in_eval || vm->watchers->in_install
        || vm->watchers->in_scratch)
        return;
    if (vm->pending_onleave_head) drain_pending_onleave_queue(vm);
    urbi_drain_deferred_slot_changes(vm);
    if (vm->watchers->dirty_count > 0) {
        uint8_t saved = vm->watchers->whenever_edge_only;
        vm->watchers->whenever_edge_only = (uint8_t)(bounded_whenever ? 1 : 0);
        watcher_eval_dirty(vm);
        vm->watchers->whenever_edge_only = saved;
    }
}

#endif /* URBI_VM_REACTIVE_DRAIN_H */
