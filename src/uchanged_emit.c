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

#include "uchanged_node.h"      /* UChangedNode, urbi_emit_slot_change_slow,
                                   urbi_drain_deferred_slot_changes */
#include "object/uobject.h"     /* UObject, struct UObject */
#include "uevent_emit.h"        /* c_event_emit_sync */
#include "uvm.h"                /* UVM, UDeferredSlotChange */
#include "umacros.h"            /* URBI_INTERNAL_ASSERT */
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
 * safepoint before watcher_eval_dirty) and emit a one-shot URBI_LOG_WARN. */
void
urbi_emit_slot_change_slow(UVM *vm, UObject *parent,
                           USymbol *key, UValue new_value)
{
    if (vm->in_watcher_eval || vm->in_watcher_install || vm->in_watcher_scratch) {
        if (!vm->slot_change_reentrancy_warned) {
            vm->slot_change_reentrancy_warned = 1;
            if (vm->host_log_fn)
                vm->host_log_fn(vm, URBI_LOG_WARN,
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

    /* Bit 7 is set but no chain entry matches `key`.  This is a programming
     * error (bit 7 must only be set when at least one UChangedNode exists for
     * the object; individual keys may not match if the subscriber was for a
     * different slot — that is normal and we silently return). */
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

    uint16_t next = (uint16_t)((vm->deferred_slot_changes_tail + 1u)
                                % vm->deferred_slot_changes_cap);
    if (next == vm->deferred_slot_changes_head) {
        /* Ring full — drop and one-shot warn. */
        if (!vm->slot_change_ring_full_warned) {
            vm->slot_change_ring_full_warned = 1;
            if (vm->host_log_fn)
                vm->host_log_fn(vm, URBI_LOG_WARN,
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
 * safepoint BEFORE watcher_eval_dirty per spec §5.4 ordering. */
void
urbi_drain_deferred_slot_changes(UVM *vm)
{
    if (vm->deferred_slot_changes == NULL) return;

    while (vm->deferred_slot_changes_head != vm->deferred_slot_changes_tail) {
        UDeferredSlotChange d =
            vm->deferred_slot_changes[vm->deferred_slot_changes_head];

        /* Clear the slot before advancing (ring entries should not hold
         * stale pointers past their useful lifetime). */
        vm->deferred_slot_changes[vm->deferred_slot_changes_head] =
            (UDeferredSlotChange){ NULL, NULL, {0, {0}} };

        vm->deferred_slot_changes_head =
            (uint16_t)((vm->deferred_slot_changes_head + 1u)
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
