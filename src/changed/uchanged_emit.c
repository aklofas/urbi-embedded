/* SPDX-License-Identifier: BSD-3-Clause */
/* Slot-change emission slow path + deferred-emit ring drain.
 * Spec #4 §5.1, §5.3.
 *
 * urbi_emit_slot_change_slow — called when UGC_HAS_SLOT_CHANGE_EVENT is set.
 *   Re-entrancy guard: if a sync body is currently running on the scratch
 *   frame (in_watcher_eval/install/scratch set), route to the deferred ring
 *   with a one-shot URBI_LOG_WARN.  Otherwise walk the UChangedNode chain by
 *   USymbol identity and dispatch via c_event_emit_sync.
 *
 * urbi_drain_deferred_slot_changes — drain the per-VM deferred ring.
 *   Called at every safepoint BEFORE watcher_eval_dirty per spec §5.4. */

#include "changed/uchanged_node.h"      /* UChangedNode, urbi_emit_slot_change_slow,
#include "chunk/uchunk.h"
#include <stddef.h>
#include <stdint.h>
                                   urbi_drain_deferred_slot_changes */
#include "object/uobject.h"     /* UObject, struct UObject */
#include "event/uevent_emit.h"        /* c_event_emit_sync */
#include "vm/uvm.h"                /* UVM, UDeferredSlotChange */
#include "runtime/umacros.h"            /* URBI_INTERNAL_ASSERT */
#include "urbi/urbi.h"          /* URBI_LOG_WARN */

/* === urbi_emit_slot_change_slow (spec #4 §5.1) ===
 *
 * Invoked only when UGC_HAS_SLOT_CHANGE_EVENT is set on `parent` (the bit
 * test is in the inline urbi_emit_slot_change_if_subscribed in
 * uchanged_node.h).
 *
 * Re-entrancy: if any scratch-context flag is set the current call must
 * have originated from inside a sync slot-change body.  Route to the
 * deferred ring (urbi_drain_deferred_slot_changes runs at the next
 * safepoint before watcher_eval_dirty) and emit a one-shot URBI_LOG_WARN.
 *
 * WATCH-010 drain dependency: vm->watchers->in_eval is the at/whenever-cond
 * eval flag.  When set, this function MUST route through the deferred
 * ring — the at/whenever body wouldn't see its own write-during-eval
 * otherwise, since the body strand is driven by the same eval pass.
 * See uvm.h field comment on in_watcher_eval for the full invariant. */
void
urbi_emit_slot_change_slow(UVM *vm, UObject *parent,
                           USymbol *key, UValue new_value)
{
    if (vm->watchers->in_eval || vm->watchers->in_install || vm->watchers->in_scratch) {
        if (!vm->slot_change_reentrancy_warned) {
            vm->slot_change_reentrancy_warned = 1;
            if (vm->host_log_fn)
                vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                    "slot-change emit from scratch context — degraded to async");
        }
        /* Defer: drain runs at next safepoint (spec §5.4 ordering). */
        urbi_defer_slot_change(vm, parent, key, new_value);
        return;
    }

    /* Walk chain by USymbol pointer identity (interned → pointer-compare). */
    UChangedNode *n;
    for (n = parent->changed_events_head; n != NULL; n = n->next) {
        if (n->name == key) {
            c_event_emit_sync(vm, n->event, new_value);
            return;
        }
    }

    /* Bit 7 (UGC_HAS_SLOT_CHANGE_EVENT) is set but no chain entry matches
     * `key`.  This is the normal "subscriber on a different slot" case:
     * bit 7 is per-object, not per-slot, so a slot-change emit for any slot
     * on a subscribed object reaches here, but only one slot's UChangedNode
     * needs to match.  Silently return; the unmatched slot has no
     * subscribers. */
}

/* === urbi_defer_slot_change (spec #4 §5.3) ===
 *
 * Write (parent, key, new_value) to the tail of the deferred SPSC ring.
 * Ring-full: emit one-shot URBI_LOG_WARN and drop (no heap fallback). */
void
urbi_defer_slot_change(UVM *vm, UObject *parent,
                       USymbol *key, UValue new_value)
{
    if (vm->deferred_slot_changes == NULL) return;   /* OOM at init */

    uint16_t next = (uint16_t)((vm->deferred_slot_changes_tail + 1U)
                                % vm->deferred_slot_changes_cap);
    if (next == vm->deferred_slot_changes_head) {
        /* Ring full — drop and one-shot warn. */
        if (!vm->slot_change_ring_full_warned) {
            vm->slot_change_ring_full_warned = 1;
            if (vm->host_log_fn)
                vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                    "slot-change deferred ring full — entry dropped");
        }
        return;
    }

    vm->deferred_slot_changes[vm->deferred_slot_changes_tail] =
        (UDeferredSlotChange){ parent, key, new_value };
    vm->deferred_slot_changes_tail = next;
}

/* === urbi_drain_deferred_slot_changes (spec #4 §5.3) ===
 *
 * Pop each entry from head to tail and re-emit at top level (no scratch
 * context now, so c_event_emit_sync runs normally).  Called at every
 * safepoint BEFORE watcher_eval_dirty per spec §5.4 ordering.
 *
 * VM-016: the empty-ring fast path returns before any work.  Every
 * safepoint calls this drain unconditionally; the typical safepoint
 * has nothing pending (deferred entries are pushed only from within
 * watcher-scratch context — vm->watchers->in_scratch != 0 — and drained
 * once the scratch frame returns).  An explicit head-equals-tail early
 * return makes the no-work path two loads + a branch and avoids the
 * cost of entering the while loop's condition + post-condition cleanup
 * tail.  The earlier `while` self-guard remains as a defense for the
 * normal multi-entry case. */
void
urbi_drain_deferred_slot_changes(UVM *vm)
{
    if (vm->deferred_slot_changes == NULL) return;
    if (vm->deferred_slot_changes_head == vm->deferred_slot_changes_tail)
        return;

    while (vm->deferred_slot_changes_head != vm->deferred_slot_changes_tail) {
        UDeferredSlotChange d =
            vm->deferred_slot_changes[vm->deferred_slot_changes_head];

        /* Clear the slot before advancing (ring entries should not hold
         * stale pointers past their useful lifetime). */
        {
            UValue nil = {0};
            vm->deferred_slot_changes[vm->deferred_slot_changes_head] =
                (UDeferredSlotChange){ .parent = NULL, .key = NULL, .new_value = nil };
        }

        vm->deferred_slot_changes_head =
            (uint16_t)((vm->deferred_slot_changes_head + 1U)
                        % vm->deferred_slot_changes_cap);

        /* Walk chain and dispatch.  No scratch context at this point so
         * c_event_emit_sync runs inline without looping back into defer. */
        UChangedNode *n;
        for (n = d.parent->changed_events_head; n != NULL; n = n->next) {
            if (n->name == d.key) {
                c_event_emit_sync(vm, n->event, d.new_value);
                break;
            }
        }
    }
}

/* === urbi_deferred_slot_changes_walk_roots (W3/v0.10.2) ===
 *
 * GC root provider for vm->deferred_slot_changes[head..tail].
 * Yields each (parent, new_value) pair as roots so a GC slice between
 * urbi_defer_slot_change and urbi_drain_deferred_slot_changes leaves
 * the ring contents reachable.  Closes reactive audit F6.
 *
 * Today the cooperative invariant says no GC slice fires in that window,
 * so the walker is correctness-preserving (it doesn't change which objects
 * are kept alive in well-formed runs).  Under future preemption upgrades
 * (v1.x scheduler), the walker becomes load-bearing and prevents UAF in
 * the drain path.
 *
 * Registered with urbi_gc_register_root_provider in urbi_vm_init. */
void
urbi_deferred_slot_changes_walk_roots(struct UVM *vm,
                                       UGcRootCallback cb, void *ctx)
{
    if (vm->deferred_slot_changes == NULL) return;
    uint16_t i = vm->deferred_slot_changes_head;
    while (i != vm->deferred_slot_changes_tail) {
        UDeferredSlotChange *d = &vm->deferred_slot_changes[i];
        if (d->parent != NULL) {
            UValue v = { .kind = (uint8_t)UVAL_OBJECT, .v.p = d->parent };
            cb(vm, &v, ctx);
        }
        cb(vm, &d->new_value, ctx);
        i = (uint16_t)((i + 1U) % vm->deferred_slot_changes_cap);
    }
}
