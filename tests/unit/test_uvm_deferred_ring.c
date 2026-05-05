/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UVM deferred slot-change ring (spec #4 §3.5).
 *
 * Checks:
 *   - deferred_slot_changes is non-NULL at create.
 *   - head and tail are both 0 at create (ring empty).
 *   - cap >= 16 (covers both footprint and default presets).
 *   - slot_change_reentrancy_warned is 0 at create. */

#include "utest.h"
#include "uvm.h"

static void uvm_deferred_slot_change_ring_alloc(void)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT(vm.deferred_slot_changes != NULL);
    UASSERT_EQ(0, (int)vm.deferred_slot_changes_head);
    UASSERT_EQ(0, (int)vm.deferred_slot_changes_tail);
    UASSERT(vm.deferred_slot_changes_cap >= 16);
    UASSERT_EQ(0, (int)vm.slot_change_reentrancy_warned);

    uvm_destroy(&vm);
}

void
test_uvm_deferred_ring_suite(void)
{
    printf("test_uvm_deferred_ring\n");
    utest_run("uvm_deferred_slot_change_ring_alloc",
              uvm_deferred_slot_change_ring_alloc);
}
