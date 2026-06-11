/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit test: URBI_GC_STRESS collect-before-every-alloc mode
 * (refactor-3 TEST-GAP-01).
 *
 * Verifies (stress build only): an unrooted cell allocated and then
 * abandoned is swept by the forced collection that precedes the NEXT
 * allocation.  On non-stress builds the suite degenerates to a smoke
 * check that gc_stress_armed is set but inert. */

#include "utest.h"
#include "urbi/gc.h"
#include "gc/ugc_incremental.h"
#include "vm/uvm.h"
#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* Local mirror of UAllCellsNode's layout — same test-only coupling as
 * test_ugc_state_machine.c (field order verified against
 * ugc_incremental.c: cell, size, next, next_gray). */
typedef struct MirrorNode {
    void              *cell;
    size_t             size;
    struct MirrorNode *next;
    struct MirrorNode *next_gray;
} MirrorNode;

static int cell_on_all_cells_list(UVM *vm, const void *cell)
{
    const MirrorNode *n = (const MirrorNode *)(void *)vm->all_cells_head;
    while (n != NULL) {
        if (n->cell == cell) return 1;
        n = n->next;
    }
    return 0;
}

static int count_all_cells(UVM *vm)
{
    const MirrorNode *n = (const MirrorNode *)(void *)vm->all_cells_head;
    int count = 0;
    while (n != NULL) {
        count++;
        n = n->next;
    }
    return count;
}

/* Byte poked into the orphan's payload tail.  urbi_gc_alloc zero-inits
 * every new cell, so a swept-then-recycled address can never re-present
 * this pattern. */
#define ORPHAN_TAG_BYTE 0xA5U

UTEST(stress_mode_sweeps_unrooted_cell_on_next_alloc)
{
#if URBI_GC_STRESS
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(1U, vm.gc_stress_armed);

    UCell *orphan = urbi_gc_alloc(&vm, 64U, UTYPE_OBJECT);
    UASSERT(orphan != NULL);
    UASSERT(cell_on_all_cells_list(&vm, orphan));
    ((uint8_t *)(void *)orphan)[63] = (uint8_t)ORPHAN_TAG_BYTE;
    int live_before = count_all_cells(&vm);

    /* The next alloc force-collects first; orphan has no root anywhere.
     * NOTE the freed 64-byte block is typically recycled for `second`
     * itself, so address membership alone is NOT evidence of survival:
     * require the payload tag too (zeroed on reuse), and require the
     * live-cell count to stay flat (exactly one swept per one allocated). */
    UCell *second = urbi_gc_alloc(&vm, 64U, UTYPE_OBJECT);
    UASSERT(second != NULL);
    UASSERT_EQ(live_before, count_all_cells(&vm));
    UASSERT(!(cell_on_all_cells_list(&vm, orphan) &&
              ((uint8_t *)(void *)orphan)[63] == (uint8_t)ORPHAN_TAG_BYTE));

    urbi_vm_destroy(&vm);
#else
    /* Non-stress build: the field is still armed at init-end (always
     * present so layout matches stress builds) but urbi_gc_alloc never
     * reads it — allocations stay unperturbed. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(1U, vm.gc_stress_armed);

    UCell *orphan = urbi_gc_alloc(&vm, 64U, UTYPE_OBJECT);
    UASSERT(orphan != NULL);
    int live_before = count_all_cells(&vm);

    UCell *second = urbi_gc_alloc(&vm, 64U, UTYPE_OBJECT);
    UASSERT(second != NULL);
    /* No forced collection ran: the unrooted orphan is still live and the
     * all-cells list simply grew by one. */
    UASSERT_EQ(live_before + 1, count_all_cells(&vm));
    UASSERT(cell_on_all_cells_list(&vm, orphan));

    urbi_vm_destroy(&vm);
#endif
}

void test_gc_stress_mode_suite(void)
{
    utest_run("stress_mode_sweeps_unrooted_cell_on_next_alloc",
              stress_mode_sweeps_unrooted_cell_on_next_alloc);
}
