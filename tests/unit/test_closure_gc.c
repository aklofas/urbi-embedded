/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_closure_gc.c — v0.8.4 Option B Step D (T18)
 *
 * GC reachability regression tests for UClosure + UUpvalCell promotion.
 * Pins three anchor categories introduced by Step C-2 (GC-managed closures):
 *
 *   Scenario 1 — realm-global anchor survival:
 *     A closure stored as a realm global must survive urbi_gc_force_full.
 *     Root path: global_object slot → mark_root_callback (UVAL_CLOSURE) →
 *     gc_shade_gray → closure survives sweep.
 *
 *   Scenario 2 — heapified-upval reachability:
 *     A closure with a captured-and-closed UUpvalCell keeps the cell alive
 *     across multiple GC cycles.  Root path: realm-global closure →
 *     walk_uclosure shades upvals[0] → walk_upvalcell survives sweep.
 *
 *   Scenario 3 — multi-cycle watcher closure survival:
 *     A closure stored as w->condition survives repeated GC cycles while the
 *     watcher is active, then is collected once the watcher is unregistered.
 *     Root path: watcher_table_walk_roots yields UVAL_CLOSURE →
 *     mark_root_callback shades closure → closure survives sweep.
 *     Negative proof: after watcher removal the closure is collected.
 *
 * Implementation note on cell_is_alive:
 *   vm->all_cells_head stores a sidecar UAllCellsNode list.  This test uses
 *   the same mirror-struct trick as test_ugc_state_machine.c and
 *   test_event_gc.c to walk the list without coupling to the private
 *   UAllCellsNode definition.  Layout: { UCell *cell, size_t size,
 *   MirrorNode *next, MirrorNode *next_gray }. */

#include "utest.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "gc/ugc.h"               /* UTYPE_CLOSURE, UTYPE_UPVAL_CELL, UCell */
#include "gc/ugc_incremental.h"   /* UGC_IS_PINNED (for reference) */
#include "runtime/uclosure.h"     /* UClosure, UUpvalCell full definitions */
#include "watcher/uwatcher.h"     /* UWatcher, UWATCHER_AT */
#include "twatcher_install_helper.h"
#include "urbi/urbi.h"            /* urbi_vm_init/destroy, urbi_make_* etc. */
#include "urbi/gc.h"              /* urbi_gc_alloc, urbi_gc_force_full */
#include "chunk/umodule.h"       /* UVAL_CLOSURE, UValue */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Shared helpers
 * =================================================================== */

/* Mirror of the private UAllCellsNode struct (first 4 fields matching the
 * sidecar layout in ugc_incremental.c).  Used only for list walking. */
typedef struct MirrorNode {
    void              *cell;
    size_t             size;
    struct MirrorNode *next;
    struct MirrorNode *next_gray;
} MirrorNode;

/* Returns 1 if target is still on vm->all_cells_head (i.e. GC-managed and
 * not yet freed by a sweep), 0 otherwise.  Does NOT dereference target. */
static int
cell_is_alive(UVM *vm, UCell *target)
{
    MirrorNode *n = (MirrorNode *)(void *)vm->all_cells_head;
    while (n != NULL) {
        if (n->cell == target) return 1;
        n = n->next;
    }
    return 0;
}

/* No-op native function used as the body of dummy test closures.
 * urbi_make_native_closure requires fn != NULL. */
static int
test_noop_fn(struct UVM *vm, UValue self, UValue *args,
             uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    *out = urbi_make_nil();
    return 0;
}

/* ===================================================================
 * Scenario 1 — realm-global anchor survival
 *
 * A closure stored in a realm global slot must survive a full GC cycle.
 * The root path is: realm global_object slot (UVAL_CLOSURE) →
 * mark_root_callback shades the UClosure cell → cell survives sweep.
 * =================================================================== */
UTEST(realm_global_closure_survives_gc)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Allocate a GC-managed native closure. */
    UClosure *cl = urbi_make_native_closure(&vm, test_noop_fn);
    UASSERT(cl != NULL);

    /* Verify the closure cell is on the GC heap. */
    UASSERT(cell_is_alive(&vm, (UCell *)cl));

    /* Store closure in a realm global (name "gc_test_anchor"). */
    UValue cl_val = urbi_make_closure(cl);
    int rc = urbi_realm_set_global(&vm, realm, "gc_test_anchor",
                                   sizeof("gc_test_anchor") - 1U, cl_val);
    UASSERT_EQ(rc, URBI_OK);

    /* Force a complete GC cycle (MARK_ROOTS → MARK → SWEEP → IDLE). */
    urbi_gc_force_full(&vm);

    /* Closure must still be alive: the realm global is a root. */
    UASSERT(cell_is_alive(&vm, (UCell *)cl));

    /* Retrieve the slot and verify it still points to the same closure. */
    UValue out = urbi_make_nil();
    rc = urbi_realm_get_global(&vm, realm, "gc_test_anchor",
                               sizeof("gc_test_anchor") - 1U, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_CLOSURE);
    UASSERT(urbi_value_as_closure(out) == cl);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Scenario 2 — heapified-upval reachability
 *
 * A UClosure with a captured (heapified) UUpvalCell must keep the cell
 * alive as long as the closure is reachable from a GC root.
 *
 * Approach: build a GC-managed UClosure + UUpvalCell directly via
 * urbi_gc_alloc (using the same UTYPE_CLOSURE / UTYPE_UPVAL_CELL tags
 * that the VM uses internally).  Root the closure via a realm global.
 * Verify both cells survive repeated GC cycles.  Also verify the value
 * stored in the heapified cell is intact after GC.
 *
 * Root path:
 *   realm global (UVAL_CLOSURE) → mark_root_callback shades cl →
 *   walk_uclosure: gc_shade_gray(vm, &upval->cell) →
 *   walk_upvalcell: cb(vm, &upval->u.value, ctx) for on_heap=true →
 *   mark_root_callback (upval.u.value is UVAL_INT — not heap-bearing,
 *   no further shading; INT is kept because its cell is already traced).
 *   Both cl and upval survive sweep because both were shaded gray→black.
 * =================================================================== */
UTEST(heapified_upval_survives_gc)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Allocate a GC-managed UUpvalCell and place a known value in it.
     * on_heap=true: the cell owns the value copy (not a stack alias). */
    UCell *uvc = urbi_gc_alloc(&vm, sizeof(UUpvalCell), UTYPE_UPVAL_CELL);
    UASSERT(uvc != NULL);
    UUpvalCell *upval = (UUpvalCell *)uvc;
    upval->on_heap     = true;
    upval->u.value     = urbi_make_int(42);
    upval->next        = NULL;

    /* Allocate a GC-managed UClosure with one upval slot.
     * sizeof(UClosure) already includes the first trailing FAM slot (upvals[1]).
     * Set nupvals=1 so walk_uclosure traces upvals[0]. */
    UCell *clc = urbi_gc_alloc(&vm, sizeof(UClosure), UTYPE_CLOSURE);
    UASSERT(clc != NULL);
    UClosure *cl = (UClosure *)clc;
    cl->proto      = NULL;   /* native-fn style; no refcount adjustment needed */
    cl->proto_inst = NULL;
    cl->nupvals    = 1;
    cl->native_fn  = test_noop_fn;
    cl->upvals[0]  = upval;

    /* Root the closure via a realm global. */
    UValue cl_val = urbi_make_closure(cl);
    int rc = urbi_realm_set_global(&vm, realm, "gc_test_upval",
                                   sizeof("gc_test_upval") - 1U, cl_val);
    UASSERT_EQ(rc, URBI_OK);

    /* Force two consecutive GC cycles.  Each cycle must keep both cells. */
    urbi_gc_force_full(&vm);
    UASSERT(cell_is_alive(&vm, (UCell *)cl));
    UASSERT(cell_is_alive(&vm, &upval->cell));

    /* Verify the captured value is intact. */
    UASSERT_EQ(upval->u.value.kind, (uint8_t)UVAL_INT);
    UASSERT_EQ(upval->u.value.v.i, (int64_t)42);

    urbi_gc_force_full(&vm);
    UASSERT(cell_is_alive(&vm, (UCell *)cl));
    UASSERT(cell_is_alive(&vm, &upval->cell));
    UASSERT_EQ(upval->u.value.v.i, (int64_t)42);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Scenario 3 — multi-cycle watcher closure survival + negative proof
 *
 * A closure stored as w->condition must survive repeated GC cycles while
 * the watcher is active in vm->active_watchers_head.
 *
 * Root path:
 *   watcher_table_walk_roots (root provider) yields UVAL_CLOSURE for each
 *   non-NULL w->condition → mark_root_callback shades the UClosure cell →
 *   walk_uclosure (nupvals=0 for native closure, no upvals to trace) →
 *   cell survives sweep.
 *
 * Negative proof:
 *   After urbi_watcher_unregister_internal removes the watcher from
 *   vm->active_watchers_head, the closure is no longer yielded by
 *   watcher_table_walk_roots.  If no other root holds the closure, a
 *   subsequent urbi_gc_force_full collects it.  This test verifies the
 *   closure IS collected after watcher removal, confirming the watcher
 *   was the sole root.
 *
 * Implementation note: this test installs the watcher with a no-op
 * condition hook to prevent invoke_condition_closure from dispatching the
 * native closure through urbi_run_closure_on_scratch (which would crash
 * because native closures have cl->proto == NULL, and strand_arm_from_closure
 * dereferences proto->instructions unconditionally).  The hook is only
 * installed temporarily for the GC cycles; during the multi-cycle loop no
 * watcher eval runs, so no hook is needed there either.
 * =================================================================== */

/* No-op condition hook: returns nil; prevents real bytecode dispatch. */
static UValue
noop_condition_hook(struct UVM *vm, struct UWatcher *w)
{
    (void)vm; (void)w;
    UValue nil = {0};
    return nil;
}

UTEST(watcher_closure_survives_multi_gc_then_collected)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Allocate a GC-managed native closure for use as the watcher condition. */
    UClosure *cl = urbi_make_native_closure(&vm, test_noop_fn);
    UASSERT(cl != NULL);
    UASSERT(cell_is_alive(&vm, (UCell *)cl));

    /* Install the watcher with the closure as condition.
     * Set the hook so install-time seed (via urbi_watcher_install_for_test)
     * uses the hook to get nil; prevents real bytecode dispatch. */
    vm.test_watcher_condition_hook = noop_condition_hook;
    UWatcher *w = urbi_watcher_install_for_test(
        &vm, UWATCHER_AT, NULL,
        /*condition=*/cl,
        /*body=*/     NULL,
        /*onleave=*/  NULL,
        NULL, 0U);
    UASSERT(w != NULL);
    vm.test_watcher_condition_hook = NULL;   /* clear — no eval in this test */

    /* Watcher is on active_watchers_head; closure rooted via it.
     * Run 3 full GC cycles — closure must survive each one. */
    int i;
    for (i = 0; i < 3; i++) {
        urbi_gc_force_full(&vm);
        UASSERT(cell_is_alive(&vm, (UCell *)cl));
        UASSERT(vm.active_watchers_head == w);
    }

    /* Unregister watcher — closure no longer reachable from any root.
     * (No realm global, no strand register, no other watcher.) */
    urbi_watcher_unregister_internal(&vm, w);
    UASSERT(vm.active_watchers_head == NULL);

    /* One more GC cycle — closure must now be collected. */
    urbi_gc_force_full(&vm);
    UASSERT(!cell_is_alive(&vm, (UCell *)cl));

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_closure_gc_suite(void)
{
    printf("test_closure_gc\n");
    utest_run("realm_global_closure_survives_gc",
              realm_global_closure_survives_gc);
    utest_run("heapified_upval_survives_gc",
              heapified_upval_survives_gc);
    utest_run("watcher_closure_survives_multi_gc_then_collected",
              watcher_closure_survives_multi_gc_then_collected);
}
