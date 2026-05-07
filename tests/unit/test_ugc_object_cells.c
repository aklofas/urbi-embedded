/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: GC integration for the M4 object-model cell types.
 *
 * Verifies that allocations of UObject / UProtos / UShape / UProps /
 * USlotHandle / UModuleInstance / UProtoInstance via urbi_gc_alloc are
 * fully integrated with the incremental mark + sweep cycle:
 *   - Each UTYPE_* tag has a registered UType in vm->type_table[].
 *   - Allocations succeed and zero-initialize the cell.
 *   - A full GC cycle (urbi_gc_collect) reaches every cell without
 *     panicking on unknown tags or null walkers.
 *   - Unreferenced cells are reclaimed (sweep returns to IDLE with
 *     the all-cells list empty when no roots are live).
 *
 * USlotHandle / UModuleInstance / UProtoInstance get no-op walkers at
 * this task; later M4 tasks fill them in once their owning data lands. */

#include "utest.h"

#include "urbi/gc.h"
#include "gc/ugc_incremental.h"
#include "object/uobject.h"
#include "object/ushape.h"
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* Mirror of the private UAllCellsNode layout used by ugc_incremental.c
 * for counting live cells via vm->all_cells_head.  Same trick as
 * test_ugc_state_machine.c — first field is UCell *, then size_t, then
 * two next-pointers. */
typedef struct MirrorNode {
    void   *cell;
    size_t  size;
    struct MirrorNode *next;
    struct MirrorNode *next_gray;
} MirrorNode;

static int count_all_cells(UVM *vm) {
    MirrorNode *n = (MirrorNode *)(void *)vm->all_cells_head;
    int count = 0;
    while (n != NULL) {
        count++;
        n = n->next;
    }
    return count;
}

/* ===== Test 1: every M4 tag has a registered UType walker ===== */

UTEST(ugc_object_cells_types_registered) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT(vm.type_table[UTYPE_OBJECT]           != NULL);
    UASSERT(vm.type_table[UTYPE_PROTOS]           != NULL);
    UASSERT(vm.type_table[UTYPE_SHAPE]            != NULL);
    UASSERT(vm.type_table[UTYPE_PROPS]            != NULL);
    UASSERT(vm.type_table[UTYPE_SLOTHANDLE]       != NULL);
    UASSERT(vm.type_table[UTYPE_MODULE_INSTANCE]  != NULL);
    UASSERT(vm.type_table[UTYPE_PROTO_INSTANCE]   != NULL);

    /* Each registered descriptor has a non-NULL walk_payload (even the
     * no-op stubs install a callable function pointer so that the mark
     * dispatcher's `t->walk_payload != NULL` guard is exercised). */
    UASSERT(vm.type_table[UTYPE_OBJECT]->walk_payload          != NULL);
    UASSERT(vm.type_table[UTYPE_PROTOS]->walk_payload          != NULL);
    UASSERT(vm.type_table[UTYPE_SHAPE]->walk_payload           != NULL);
    UASSERT(vm.type_table[UTYPE_PROPS]->walk_payload           != NULL);
    UASSERT(vm.type_table[UTYPE_SLOTHANDLE]->walk_payload      != NULL);
    UASSERT(vm.type_table[UTYPE_MODULE_INSTANCE]->walk_payload != NULL);
    UASSERT(vm.type_table[UTYPE_PROTO_INSTANCE]->walk_payload  != NULL);

    uvm_destroy(&vm);
}

/* ===== Test 2: each cell type allocates and carries the right tag ===== */

UTEST(ugc_object_cells_alloc_each_type) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UCell *o   = urbi_gc_alloc(&vm, sizeof(UObject), UTYPE_OBJECT);
    UCell *up  = urbi_gc_alloc(&vm, sizeof(UProtos) + 4u * sizeof(UObject *),
                               UTYPE_PROTOS);
    UCell *s   = urbi_gc_alloc(&vm, sizeof(UShape),  UTYPE_SHAPE);
    UCell *p   = urbi_gc_alloc(&vm, sizeof(UProps),  UTYPE_PROPS);
    UCell *sh  = urbi_gc_alloc(&vm, 32u, UTYPE_SLOTHANDLE);
    UCell *mi  = urbi_gc_alloc(&vm, 32u, UTYPE_MODULE_INSTANCE);
    UCell *pi  = urbi_gc_alloc(&vm, 32u, UTYPE_PROTO_INSTANCE);

    UASSERT(o  != NULL); UASSERT_EQ(o->type_tag,  (int)UTYPE_OBJECT);
    UASSERT(up != NULL); UASSERT_EQ(up->type_tag, (int)UTYPE_PROTOS);
    UASSERT(s  != NULL); UASSERT_EQ(s->type_tag,  (int)UTYPE_SHAPE);
    UASSERT(p  != NULL); UASSERT_EQ(p->type_tag,  (int)UTYPE_PROPS);
    UASSERT(sh != NULL); UASSERT_EQ(sh->type_tag, (int)UTYPE_SLOTHANDLE);
    UASSERT(mi != NULL); UASSERT_EQ(mi->type_tag, (int)UTYPE_MODULE_INSTANCE);
    UASSERT(pi != NULL); UASSERT_EQ(pi->type_tag, (int)UTYPE_PROTO_INSTANCE);

    uvm_destroy(&vm);
}

/* ===== Test 3: full GC cycle over allocations of all 7 tags ===== */

/* Allocate one of each M4 cell type (zero-initialised; no inter-cell
 * references yet — those land at later M4 tasks).  A full mark+sweep
 * cycle must visit each cell's walker without panic and reclaim all
 * unreferenced cells. */
UTEST(ugc_object_cells_full_cycle_reclaims_unreferenced) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    (void)urbi_gc_alloc(&vm, sizeof(UObject), UTYPE_OBJECT);
    (void)urbi_gc_alloc(&vm, sizeof(UProtos) + 2u * sizeof(UObject *),
                        UTYPE_PROTOS);
    (void)urbi_gc_alloc(&vm, sizeof(UShape),  UTYPE_SHAPE);
    (void)urbi_gc_alloc(&vm, sizeof(UProps),  UTYPE_PROPS);
    (void)urbi_gc_alloc(&vm, 16u, UTYPE_SLOTHANDLE);
    (void)urbi_gc_alloc(&vm, 16u, UTYPE_MODULE_INSTANCE);
    (void)urbi_gc_alloc(&vm, 16u, UTYPE_PROTO_INSTANCE);

    UASSERT_EQ(count_all_cells(&vm), 7);

    /* No roots reference any of these cells — the sweep should reclaim
     * everything (modulo the lazy root_shape that may be allocated by
     * other paths; it isn't here). */
    urbi_gc_collect(&vm);

    UASSERT_EQ(urbi_gc_phase(&vm), (int)GC_PHASE_IDLE);
    UASSERT_EQ(count_all_cells(&vm), 0);
    UASSERT_EQ((int)urbi_gc_live_bytes(&vm), 0);

    uvm_destroy(&vm);
}

/* ===== Test 4: UShape walker traces parent + props_table children =====
 *
 * Test-local root provider: shades a single UCell* held in a file-static
 * pointer.  The provider mechanism normally walks UValue slots via the
 * mark callback; for direct UCell-headed roots like UShape* we bypass cb
 * and call gc_shade_gray directly (same routine the cb ultimately calls
 * for heap-bearing UValues).  Using gc_shade_gray instead of touching
 * gc_byte directly keeps the cell on the gray work-list so the mark
 * phase exercises its walker. */
static UCell *g_test_root_cell = NULL;

static void test_root_provider(UVM *vm, UGcRootCallback cb, void *ctx) {
    (void)cb; (void)ctx;
    if (g_test_root_cell != NULL) {
        gc_shade_gray(vm, g_test_root_cell);
    }
}

/* Sidecar mirror — only the `cell` field is read here, used to verify a
 * given UCell* is still in vm->all_cells_head after sweep. */
static int cell_is_alive(UVM *vm, UCell *target) {
    MirrorNode *n = (MirrorNode *)(void *)vm->all_cells_head;
    while (n != NULL) {
        if (n->cell == target) return 1;
        n = n->next;
    }
    return 0;
}

/* Build a UShape with a parent UShape and one UProps in its props_table.
 * Register a root provider that shades the leaf shape; after a full GC
 * cycle, parent and child_props must survive — proving the UShape walker
 * actually shaded them via parent / props_table[0]. */
UTEST(ugc_object_cells_ushape_walker_traces_children) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    urbi_gc_register_root_provider(&vm, test_root_provider);

    /* Allocate child cells first so they live earlier on all_cells_head
     * than the parent shape; the walker order is independent of list
     * order, so this only documents the layout. */
    UProps *child_props = (UProps *)urbi_gc_alloc(&vm, sizeof(UProps),
                                                  UTYPE_PROPS);
    UShape *parent     = (UShape *)urbi_gc_alloc(&vm, sizeof(UShape),
                                                 UTYPE_SHAPE);
    UASSERT(child_props != NULL);
    UASSERT(parent != NULL);

    UShape *shape = (UShape *)urbi_gc_alloc(&vm, sizeof(UShape),
                                            UTYPE_SHAPE);
    UASSERT(shape != NULL);

    /* props_table backing storage: allocate the UPropsTable wrapper cell
     * (T17) so walk_ushape can recover and shade it via offsetof.  Seed
     * entries[0] = child_props; the walker shades it via the wrapper. */
    UPropsTable *pt = (UPropsTable *)urbi_gc_alloc(&vm,
        sizeof(UPropsTable) + sizeof(UProps *), UTYPE_PROPS_TABLE);
    UASSERT(pt != NULL);
    pt->n          = 1u;
    pt->_pad       = 0u;
    pt->entries[0] = child_props;
    shape->parent      = parent;
    shape->props_table = pt->entries;
    shape->count       = 1u;

    /* Install shape as the only test root.  After the cycle, the mark
     * phase shades shape gray, walks it (shading parent + child_props),
     * and the sweep reclaims nothing reachable from the root. */
    g_test_root_cell = (UCell *)shape;

    urbi_gc_collect(&vm);

    UASSERT_EQ(urbi_gc_phase(&vm), (int)GC_PHASE_IDLE);

    /* All three cells must survive: the walker shaded parent + child_props
     * via the shape's parent and props_table fields.  If the walker were
     * a no-op (or skipped these fields) parent and child_props would be
     * dead-white at sweep time and reclaimed. */
    UASSERT(cell_is_alive(&vm, (UCell *)shape));
    UASSERT(cell_is_alive(&vm, (UCell *)parent));
    UASSERT(cell_is_alive(&vm, (UCell *)child_props));

    /* Clear the root before destroy so cleanup doesn't re-shade a freed
     * cell (uvm_destroy frees all live cells unconditionally). */
    g_test_root_cell = NULL;
    uvm_destroy(&vm);
}

/* ===== Test 5: UProtos walker traces items[] entries =====
 *
 * Allocate a UProtos block with two non-NULL UObject* entries (and one
 * NULL slot to exercise the skip path).  Root the UProtos via the test
 * provider; after a full cycle the two child UObjects must survive,
 * proving the walker shaded items[i]. */
UTEST(ugc_object_cells_uprotos_walker_traces_items) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    urbi_gc_register_root_provider(&vm, test_root_provider);

    UObject *child0 = (UObject *)urbi_gc_alloc(&vm, sizeof(UObject),
                                               UTYPE_OBJECT);
    UObject *child1 = (UObject *)urbi_gc_alloc(&vm, sizeof(UObject),
                                               UTYPE_OBJECT);
    UASSERT(child0 != NULL);
    UASSERT(child1 != NULL);

    UProtos *up = (UProtos *)urbi_gc_alloc(
            &vm, sizeof(UProtos) + 3u * sizeof(UObject *),
            UTYPE_PROTOS);
    UASSERT(up != NULL);
    up->n        = 3u;
    up->items[0] = child0;
    up->items[1] = NULL;     /* exercise the NULL-skip path */
    up->items[2] = child1;

    g_test_root_cell = (UCell *)up;

    urbi_gc_collect(&vm);

    UASSERT_EQ(urbi_gc_phase(&vm), (int)GC_PHASE_IDLE);
    UASSERT(cell_is_alive(&vm, (UCell *)up));
    UASSERT(cell_is_alive(&vm, (UCell *)child0));
    UASSERT(cell_is_alive(&vm, (UCell *)child1));

    g_test_root_cell = NULL;
    uvm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void test_ugc_object_cells_suite(void) {
    utest_run("ugc_object_cells: types registered",
              ugc_object_cells_types_registered);
    utest_run("ugc_object_cells: alloc each type",
              ugc_object_cells_alloc_each_type);
    utest_run("ugc_object_cells: full cycle reclaims unreferenced",
              ugc_object_cells_full_cycle_reclaims_unreferenced);
    utest_run("ugc_object_cells: ushape walker traces children",
              ugc_object_cells_ushape_walker_traces_children);
    utest_run("ugc_object_cells: uprotos walker traces items",
              ugc_object_cells_uprotos_walker_traces_items);
}
