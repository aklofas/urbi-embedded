/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UVM deferred slot-change ring (spec #4 §3.5).
 *
 * Checks:
 *   - deferred_slot_changes is non-NULL at create.
 *   - head and tail are both 0 at create (ring empty).
 *   - cap >= 16 (covers both footprint and default presets).
 *   - slot_change_reentrancy_warned is 0 at create.
 *   - VM-016: drain on an empty ring does not iterate (early return). */

#include "utest.h"
#include "vm/uvm.h"
#include "changed/uchanged_node.h"  /* urbi_drain_deferred_slot_changes */

static void uvm_deferred_slot_change_ring_alloc(void)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UASSERT(vm.deferred_slot_changes != NULL);
    UASSERT_EQ(0, (int)vm.deferred_slot_changes_head);
    UASSERT_EQ(0, (int)vm.deferred_slot_changes_tail);
    UASSERT(vm.deferred_slot_changes_cap >= 16);
    UASSERT_EQ(0, (int)vm.slot_change_reentrancy_warned);

    urbi_vm_destroy(&vm);
}

/* VM-016: when the deferred ring is empty (head == tail), drain returns
 * without entering the dispatch loop.  The body of the loop dereferences
 * d.parent->changed_events_head — if a poisoned ring entry were accessed
 * with parent==NULL on an empty ring, the test would crash.  We verify
 * the empty-ring path by writing a poison entry at the head index and
 * confirming drain returns cleanly without touching it. */
static void uvm_deferred_drain_empty_does_not_iterate(void)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Pre-condition: ring is empty (head == tail at init). */
    UASSERT_EQ((int)vm.deferred_slot_changes_head,
               (int)vm.deferred_slot_changes_tail);

    /* Poison the slot at head with parent==NULL key==NULL.  If drain
     * iterates over an empty ring, it would load this entry and call
     * d.parent->changed_events_head — segfault under any sanitizer. */
    uint16_t saved_head = vm.deferred_slot_changes_head;
    uint16_t saved_tail = vm.deferred_slot_changes_tail;
    vm.deferred_slot_changes[saved_head].parent    = NULL;
    vm.deferred_slot_changes[saved_head].key       = NULL;

    /* Call drain on the empty ring.  Must return cleanly. */
    urbi_drain_deferred_slot_changes(&vm);

    /* Post-condition: head/tail unchanged (no entries consumed). */
    UASSERT_EQ((int)vm.deferred_slot_changes_head, (int)saved_head);
    UASSERT_EQ((int)vm.deferred_slot_changes_tail, (int)saved_tail);

    urbi_vm_destroy(&vm);
}

void
test_uvm_deferred_ring_suite(void)
{
    printf("test_uvm_deferred_ring\n");
    utest_run("uvm_deferred_slot_change_ring_alloc",
              uvm_deferred_slot_change_ring_alloc);
    utest_run("uvm_deferred_drain_empty_does_not_iterate",
              uvm_deferred_drain_empty_does_not_iterate);
}
