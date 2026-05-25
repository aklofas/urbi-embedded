/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: deferred slot-change ring is a GC root (W3/v0.10.2).
 *
 * Closes reactive audit F6, audit-1 F9, runtime-invariants F18 (extended).
 *
 * Cases:
 *   1. deferred_ring_empty_walk_yields_no_roots:
 *      Empty ring — walker callback is never invoked for ring entries.
 *      Baseline: walker is correctly registered and called but produces
 *      zero slots when head == tail.
 *
 *   2. deferred_ring_yields_parent_as_root:
 *      Push one entry into the deferred ring by hand (hand-writing into
 *      vm->deferred_slot_changes[], advancing the tail by one).  Call
 *      urbi_deferred_slot_changes_walk_roots directly.  Verify that the
 *      parent UObject pointer is yielded.
 *
 *   3. deferred_ring_yields_new_value_as_root:
 *      Same setup; verify that the new_value UValue is yielded via the
 *      callback.
 *
 *   4. deferred_ring_provider_registered_at_init:
 *      After urbi_vm_init, the provider
 *      urbi_deferred_slot_changes_walk_roots is in the root-provider list.
 *      Verified indirectly: call urbi_gc_walk_roots with a counting
 *      callback; root count INCREASES when a ring entry is present
 *      compared to when it is absent.
 *
 *   5. deferred_ring_provider_survives_gc_collect:
 *      Push a UVAL_OBJECT entry into the ring (bypassing the scratch guard
 *      by writing vm->deferred_slot_changes[] directly with tail+1), run
 *      urbi_gc_collect, and verify the parent UObject cell is still alive
 *      in vm->all_cells_head (i.e., the walker held it live during the
 *      mark phase).
 *
 * Test 5 is the load-bearing correctness check; tests 1-4 are structural
 * pinning for the walker shape and registration. */

#include "utest.h"

#include <stddef.h>
#include <string.h>
#include <stdint.h>

#include "vm/uvm.h"
#include "changed/uchanged_node.h"   /* urbi_deferred_slot_changes_walk_roots,
                                        UDeferredSlotChange */
#include "object/uobject.h"          /* urbi_object_alloc */
#include "gc/ugc_incremental.h"      /* gc_shade_gray */
#include "gc/ugc.h"                  /* UGcRootCallback */
#include "urbi/object.h"             /* URBI_ATOM_OBJECT */
#include "urbi/gc.h"                 /* urbi_gc_collect, urbi_gc_walk_roots */

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers
 * =================================================================== */

/* Mirror of the internal all-cells list node (same layout trick as
 * test_ugc_walk_roots.c and test_ref_gc_root.c). */
typedef struct MirrorNodeDR {
    void                  *cell;
    size_t                 size;
    struct MirrorNodeDR   *next;
    struct MirrorNodeDR   *next_gray;
} MirrorNodeDR;

static int
cell_is_alive(UVM *vm, UCell *target)
{
    MirrorNodeDR *n = (MirrorNodeDR *)(void *)vm->all_cells_head;
    while (n != NULL) {
        if (n->cell == (void *)target) return 1;
        n = n->next;
    }
    return 0;
}

/* Context passed to the probe callbacks. */
typedef struct RootProbe {
    /* Counting */
    int total_count;
    /* Specific search */
    void  *target_ptr;   /* UObject* cast to void* */
    int    seen_parent;  /* set if UVAL_OBJECT with v.p == target_ptr */
    int    seen_int42;   /* set if UVAL_INT with v.i == 42 */
} RootProbe;

static void
root_probe_cb(UVM *vm, UValue *root, void *ctx)
{
    RootProbe *p = (RootProbe *)ctx;
    (void)vm;
    p->total_count++;
    if (root->kind == UVAL_OBJECT && root->v.p == p->target_ptr)
        p->seen_parent = 1;
    if (root->kind == UVAL_INT && root->v.i == 42)
        p->seen_int42 = 1;
}

/* Push one entry into the ring by directly writing the tail slot and
 * advancing tail.  Bypasses the scratch-guard in urbi_defer_slot_change
 * so the test can inject entries without setting in_watcher_scratch. */
static void
ring_push_direct(UVM *vm, UObject *parent, UValue new_value)
{
    uint16_t tail = vm->deferred_slot_changes_tail;
    uint16_t cap  = vm->deferred_slot_changes_cap;
    uint16_t next = (uint16_t)((tail + 1U) % cap);
    /* Skip if full (shouldn't happen in these small tests). */
    if (next == vm->deferred_slot_changes_head) return;
    vm->deferred_slot_changes[tail].parent    = parent;
    vm->deferred_slot_changes[tail].key       = NULL;
    vm->deferred_slot_changes[tail].new_value = new_value;
    vm->deferred_slot_changes_tail = next;
}

/* ===================================================================
 * Test 1: empty ring walker reports nothing
 * =================================================================== */

UTEST(deferred_ring_empty_walk_yields_no_roots)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Ring is empty at init. */
    UASSERT_EQ((int)vm.deferred_slot_changes_head,
               (int)vm.deferred_slot_changes_tail);

    RootProbe probe = {0};
    urbi_deferred_slot_changes_walk_roots(&vm, root_probe_cb, &probe);

    /* No entries walked — callback never invoked for ring slots. */
    UASSERT_EQ(probe.total_count, 0);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 2: ring entry yields parent UObject as a root
 * =================================================================== */

UTEST(deferred_ring_yields_parent_as_root)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *parent = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(parent != NULL);
    if (parent == NULL) { urbi_vm_destroy(&vm); return; }

    UValue new_value;
    new_value.kind = UVAL_NIL;
    new_value.v.i = 0;

    ring_push_direct(&vm, parent, new_value);

    RootProbe probe = {0};
    probe.target_ptr = parent;
    urbi_deferred_slot_changes_walk_roots(&vm, root_probe_cb, &probe);

    UASSERT(probe.seen_parent);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 3: ring entry yields new_value as a root
 * =================================================================== */

UTEST(deferred_ring_yields_new_value_as_root)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *parent = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(parent != NULL);
    if (parent == NULL) { urbi_vm_destroy(&vm); return; }

    UValue new_value;
    new_value.kind  = UVAL_INT;
    new_value.v.i = 42;

    ring_push_direct(&vm, parent, new_value);

    RootProbe probe = {0};
    probe.target_ptr = parent;
    urbi_deferred_slot_changes_walk_roots(&vm, root_probe_cb, &probe);

    UASSERT(probe.seen_int42);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 4: provider registered at init — walk_roots count increases
 *         when a ring entry is present vs absent
 * =================================================================== */

UTEST(deferred_ring_provider_registered_at_init)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *parent = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(parent != NULL);
    if (parent == NULL) { urbi_vm_destroy(&vm); return; }

    /* Count roots with empty ring. */
    RootProbe before = {0};
    urbi_gc_walk_roots(&vm, root_probe_cb, &before);

    /* Push one entry into the ring. */
    UValue v;
    v.kind  = UVAL_INT;
    v.v.i = 99;
    ring_push_direct(&vm, parent, v);

    /* Count roots with one ring entry (parent + v = 2 extra callbacks). */
    RootProbe after = {0};
    urbi_gc_walk_roots(&vm, root_probe_cb, &after);

    /* The deferred_ring provider must have added exactly 2 more slots:
     * the parent UVAL_OBJECT and the new_value UVAL_INT. */
    UASSERT_EQ(after.total_count - before.total_count, 2);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 5: ring entry keeps parent UObject alive through GC collect
 *
 * This is the load-bearing correctness test: without the root provider,
 * a GC collect run while a ring entry holds a parent pointer would leave
 * that UObject unreachable and swept.  With the provider registered,
 * the mark phase shades the parent gray → it survives.
 * =================================================================== */

/* Shading root provider: gc_shade_gray on UVAL_OBJECT, called from the
 * GC mark phase via mark_root_callback.  This follows the pattern in
 * test_ref_gc_root.c: mark_root_callback handles UVAL_OBJECT correctly.
 * We rely on it here — no custom provider needed for shading. */

UTEST(deferred_ring_provider_keeps_parent_alive_across_gc)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Allocate a GC-managed UObject with NO other root. */
    UObject *parent = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(parent != NULL);
    if (parent == NULL) { urbi_vm_destroy(&vm); return; }

    /* Pre-condition: the object is in the all-cells list. */
    UASSERT(cell_is_alive(&vm, (UCell *)parent));

    /* Push the object into the deferred ring as a parent pointer. */
    UValue v;
    v.kind  = UVAL_INT;
    v.v.i = 0;
    ring_push_direct(&vm, parent, v);

    /* Run a full GC collect.
     * urbi_deferred_slot_changes_walk_roots is registered and yields
     * a UVAL_OBJECT wrapping parent → mark_root_callback shades parent
     * gray → the object survives the sweep phase. */
    urbi_gc_collect(&vm);

    /* Post-condition: parent still alive (root provider kept it). */
    UASSERT(cell_is_alive(&vm, (UCell *)parent));

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry point
 * =================================================================== */

void
test_deferred_slot_change_ring_roots_suite(void)
{
    printf("  [deferred_slot_change_ring_roots]\n");
    utest_run("deferred_ring_empty_walk_yields_no_roots",
              deferred_ring_empty_walk_yields_no_roots);
    utest_run("deferred_ring_yields_parent_as_root",
              deferred_ring_yields_parent_as_root);
    utest_run("deferred_ring_yields_new_value_as_root",
              deferred_ring_yields_new_value_as_root);
    utest_run("deferred_ring_provider_registered_at_init",
              deferred_ring_provider_registered_at_init);
    utest_run("deferred_ring_provider_keeps_parent_alive_across_gc",
              deferred_ring_provider_keeps_parent_alive_across_gc);
}
