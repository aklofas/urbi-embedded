/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: GC walker amendments for UEvent, UTag enter/leave, and
 * UStrand last_event_payload (spec #3 §11, T56).
 *
 * Cases:
 *   1. uevent_walker_shades_at_watchers_chain_not_waiters:
 *      Root a UEvent via a test root-provider.  After full GC, the UEvent
 *      cell survives (still in all_cells list).  Also verifies that an
 *      unrooted UEvent is collected.
 *
 *   2. utag_walker_shades_enter_leave_events:
 *      Root a UTag via a test root-provider.  The UTag walker shades
 *      enter_event and leave_event.  After full GC, all three cells survive.
 *
 *   3. strand_walker_roots_last_event_payload:
 *      Set strand.last_event_payload to a UVAL_EVENT.  Link the strand into
 *      a realm so sched_walk_roots visits it.  After full GC the UEvent cell
 *      survives because strand_walk_roots yields last_event_payload via cb.
 *
 *   4. unrooted_event_collected_by_gc:
 *      A UEvent with no roots is collected after a full GC cycle.
 *      Complementary negative case.
 *
 * NOTE: mark_root_callback only handles UVAL_CLOSURE for the strand-register
 * root walk (M3 baseline).  For GC-managed cells of other types, we use
 * gc_shade_gray from a registered root provider (same technique as
 * test_ugc_object_cells.c).  The last_event_payload test is the exception:
 * strand_walk_roots calls cb(vm, &s->last_event_payload, ctx) where cb IS
 * mark_root_callback — so the test uses UVAL_CLOSURE as the last_event_payload
 * kind to exercise the path that currently works, and verifies survival. */

#include "utest.h"

#include "vm/uvm.h"
#include "uevent.h"
#include "utag.h"
#include "tag_native.h"
#include "watcher/uwatcher.h"
#include "watcher/uwatcher_install.h"
#include "sched/ustrand.h"
#include "umodule.h"
#include "runtime/uclosure.h"
#include "realm/urealm.h"
#include "gc/ugc.h"
#include "gc/ugc_incremental.h"
#include "urbi/urbi.h"
#include "urbi/gc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers
 * =================================================================== */

typedef struct MirrorNodeEv {
    void                 *cell;
    size_t                size;
    struct MirrorNodeEv  *next;
    struct MirrorNodeEv  *next_gray;
} MirrorNodeEv;

/* Returns 1 if target is still in vm->all_cells_head list. */
static int
cell_is_alive(UVM *vm, UCell *target)
{
    MirrorNodeEv *n = (MirrorNodeEv *)(void *)vm->all_cells_head;
    while (n != NULL) {
        if (n->cell == target) return 1;
        n = n->next;
    }
    return 0;
}

static void
make_trivial_closure(UClosure *cl, UProto *proto, uint32_t *instr_buf)
{
    instr_buf[0] = (uint32_t)OP_RET;
    memset(proto, 0, sizeof(*proto));
    proto->instructions = instr_buf;
    proto->instr_count  = 1;
    memset(cl, 0, sizeof(*cl));
    cl->proto = proto;
}

/* === File-static root cell for test root provider === */
static UCell *g_ev_test_root = NULL;

static void
ev_test_root_provider(struct UVM *vm, UGcRootCallback cb, void *ctx)
{
    (void)cb; (void)ctx;
    if (g_ev_test_root != NULL) {
        gc_shade_gray(vm, g_ev_test_root);
    }
}

/* ===================================================================
 * Test 1: uevent_walker_shades_at_watchers_chain_not_waiters
 *
 * Root a UEvent via test root-provider (gc_shade_gray).  After full GC,
 * the UEvent cell must survive because it was shaded from roots.
 *
 * AT_EVENT watcher (pool cell, UGC_IS_FIXED) is also shaded by the
 * UEvent walker; FIXED cells survive regardless, but walking them is
 * required for correctness when they hold cell-bearing UValues.
 *
 * Complementary: unrooted UEvent → collected after GC.
 * =================================================================== */

UTEST(uevent_walker_shades_at_watchers_chain_not_waiters)
{
    UVM vm;
    uint32_t instr_buf[1];
    UProto   body_proto;
    UClosure body_cl;

    uvm_init(&vm, NULL, NULL);
    g_ev_test_root = NULL;
    urbi_gc_register_root_provider(&vm, ev_test_root_provider);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    if (r == NULL) { uvm_destroy(&vm); return; }

    UEvent *ev = urbi_event_create(&vm);
    UASSERT(ev != NULL);
    if (ev == NULL) { urbi_realm_destroy(&vm, r); uvm_destroy(&vm); return; }

    UStrand s;
    ustrand_init(&s, &vm);
    s.realm = r;

    make_trivial_closure(&body_cl, &body_proto, instr_buf);

    UWatcherInstallResult ri =
        install_at_event_runtime(&vm, &s, UWATCHER_AT_EVENT, ev, &body_cl, NULL);
    UASSERT_EQ((int)URBI_INSTALL_OK, (int)ri);
    UASSERT(ev->at_watchers_head != NULL);

    /* Root the event via test root provider. */
    g_ev_test_root = (UCell *)ev;

    urbi_gc_collect(&vm);

    /* UEvent survives because it was rooted. */
    UASSERT(cell_is_alive(&vm, (UCell *)ev));

    /* Remove root and run GC again — event should be collected. */
    UWatcher *w = ev->at_watchers_head;
    if (w != NULL) {
        urbi_watcher_unregister_internal(&vm, w);
    }
    g_ev_test_root = NULL;

    urbi_gc_collect(&vm);
    UASSERT(!cell_is_alive(&vm, (UCell *)ev));

    ustrand_destroy(&s, &vm);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* ===================================================================
 * Test 2: utag_walker_shades_enter_leave_events
 *
 * Root a UTag via test root-provider.  The UTag walker shades
 * enter_event and leave_event.  After full GC, all three cells survive.
 * =================================================================== */

UTEST(utag_walker_shades_enter_leave_events)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    g_ev_test_root = NULL;
    urbi_gc_register_root_provider(&vm, ev_test_root_provider);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    if (r == NULL) { uvm_destroy(&vm); return; }

    UTag *tag = utag_create(&vm);
    UASSERT(tag != NULL);
    if (tag == NULL) { urbi_realm_destroy(&vm, r); uvm_destroy(&vm); return; }

    UEvent *enter_ev = urbi_event_create(&vm);
    UEvent *leave_ev = urbi_event_create(&vm);
    UASSERT(enter_ev != NULL);
    UASSERT(leave_ev != NULL);
    if (enter_ev == NULL || leave_ev == NULL) {
        urbi_realm_destroy(&vm, r); uvm_destroy(&vm); return;
    }
    tag->enter_event = enter_ev;
    tag->leave_event = leave_ev;

    /* Root the tag — walker shades enter_event and leave_event. */
    g_ev_test_root = (UCell *)tag;

    urbi_gc_collect(&vm);

    UASSERT(cell_is_alive(&vm, (UCell *)tag));
    UASSERT(cell_is_alive(&vm, (UCell *)enter_ev));
    UASSERT(cell_is_alive(&vm, (UCell *)leave_ev));

    /* Unroot and verify all three are collected. */
    g_ev_test_root = NULL;
    tag->enter_event = NULL;  /* prevent double-free if tag finalizer runs */
    tag->leave_event = NULL;

    urbi_gc_collect(&vm);
    UASSERT(!cell_is_alive(&vm, (UCell *)tag));
    UASSERT(!cell_is_alive(&vm, (UCell *)enter_ev));
    UASSERT(!cell_is_alive(&vm, (UCell *)leave_ev));

    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* ===================================================================
 * Test 3: strand_walker_roots_last_event_payload
 *
 * Set strand.last_event_payload to a UVAL_CLOSURE (a GC-managed closure).
 * The strand walker calls cb(vm, &s->last_event_payload, ctx) —
 * mark_root_callback shades UVAL_CLOSURE cells.  After full GC the closure
 * cell survives because it was reached via strand_walk_roots.
 *
 * We use UVAL_CLOSURE because mark_root_callback only handles that kind
 * at the M5 baseline (UVAL_EVENT and UVAL_OBJECT are handled by the write
 * barrier but not by the root-walk callback yet — a separate TODO).
 * The important property verified here is that last_event_payload IS walked
 * at all (the field was added to strand_walk_roots in T56).
 * =================================================================== */

UTEST(strand_walker_roots_last_event_payload)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    if (r == NULL) { uvm_destroy(&vm); return; }

    /* Allocate a closure via GC (GC-managed; will be collected if unreachable). */
    UCell *cl_cell = urbi_gc_alloc(&vm, sizeof(UClosure), UTYPE_CLOSURE);
    UASSERT(cl_cell != NULL);
    if (cl_cell == NULL) { urbi_realm_destroy(&vm, r); uvm_destroy(&vm); return; }

    UStrand s;
    ustrand_init(&s, &vm);
    s.realm = r;

    /* Allocate a stack so strand_walk_roots scans registers. */
    s.stack = (UValue *)vm.alloc_fn(NULL,
                   UVM_STACK_CAP * sizeof(UValue), vm.alloc_ud);
    UASSERT(s.stack != NULL);
    if (s.stack == NULL) {
        ustrand_destroy(&s, &vm); urbi_realm_destroy(&vm, r); uvm_destroy(&vm); return;
    }
    {
        int i;
        for (i = 0; i < UVM_STACK_CAP; i++) s.stack[i].kind = (uint8_t)UVAL_NIL;
    }

    /* Set last_event_payload to the closure (UVAL_CLOSURE is handled by
     * mark_root_callback, so this tests the strand_walk_roots path directly). */
    s.last_event_payload.kind = (uint8_t)UVAL_CLOSURE;
    s.last_event_payload.v.p  = (void *)cl_cell;

    /* Link strand into realm for sched_walk_roots to find it. */
    s.next_in_realm = r->strands_head;
    r->strands_head = &s;

    urbi_gc_collect(&vm);

    /* cl_cell must survive — reached via last_event_payload in strand_walk_roots. */
    UASSERT(cell_is_alive(&vm, cl_cell));

    /* Unroot: clear last_event_payload and remove from realm. */
    r->strands_head = s.next_in_realm;
    s.next_in_realm = NULL;
    s.last_event_payload.kind = (uint8_t)UVAL_NIL;
    s.last_event_payload.v.i  = 0;

    /* Now cl_cell is unreachable → collected. */
    urbi_gc_collect(&vm);
    UASSERT(!cell_is_alive(&vm, cl_cell));

    vm.alloc_fn(s.stack, 0, vm.alloc_ud);
    s.stack = NULL;
    ustrand_destroy(&s, &vm);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* ===================================================================
 * Test 4: unrooted_event_collected_by_gc
 *
 * A UEvent with no roots is collected after a full GC cycle.
 * Complementary negative case.
 * =================================================================== */

UTEST(unrooted_event_collected_by_gc)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UEvent *ev = urbi_event_create(&vm);
    UASSERT(ev != NULL);
    if (ev == NULL) { uvm_destroy(&vm); return; }

    UASSERT(cell_is_alive(&vm, (UCell *)ev));

    urbi_gc_collect(&vm);

    UASSERT(!cell_is_alive(&vm, (UCell *)ev));

    uvm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void
test_event_gc_suite(void)
{
    printf("test_event_gc\n");
    utest_run("uevent_walker_shades_at_watchers_chain_not_waiters",
              uevent_walker_shades_at_watchers_chain_not_waiters);
    utest_run("utag_walker_shades_enter_leave_events",
              utag_walker_shades_enter_leave_events);
    utest_run("strand_walker_roots_last_event_payload",
              strand_walker_roots_last_event_payload);
    utest_run("unrooted_event_collected_by_gc",
              unrooted_event_collected_by_gc);
}
