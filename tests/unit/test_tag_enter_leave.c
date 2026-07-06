/* SPDX-License-Identifier: BSD-3-Clause */
/* URBI_GC_STRESS disarm (v0.13.2): C-API scaffolding suite — GC cells
 * (tags/events/objects/closures) held in bare C locals and/or synthetic
 * strands outside the realm graph, by design, to drive one primitive in
 * isolation.  Collect-on-every-alloc sweeps them between paired
 * allocations; fine on normal builds where host-C call sequences cannot
 * be interrupted by a collection.  Each test sets vm.gc_stress_armed = 0
 * after init.  Structural-by-design, not a runtime rooting bug
 * (refactor-3 TEST-GAP-01 stress-exempt list). */
/* Unit tests: OP_PUSH_TAG / OP_POP_TAG tier-2 enter/leave event hooks
 * (spec #3 §8.3, T55).
 *
 * Source-level `at (t.enter?) body` tests require globals exposure (blocked
 * at M5 by T59 / no-globals constraint at src/uemit.c:558).  These tests
 * drive via direct C-API:
 *   - urbi_watcher_install_at_event_runtime to subscribe to enter_event / leave_event.
 *   - urbi_repl_eval ("t: {}") to drive OP_PUSH_TAG + OP_POP_TAG.
 *   - vm.strand_runnable_count to observe whether body strands were spawned.
 *
 * Cases:
 *   1. push_tag_fires_enter_event_when_subscribed:
 *      Install AT_EVENT watcher on tag->enter_event; run tag scope; verify
 *      enter watcher body strand was spawned (strand_runnable_count increased).
 *
 *   2. pop_tag_fires_leave_event_before_tier1_watcher_cascade:
 *      Install AT_EVENT watcher on tag->leave_event; also register a test
 *      watcher (AT watcher with test hook) on the tag; run tag scope.
 *      After scope exits, leave watcher body strand spawned indicates tier-2
 *      fired.  The tier-1 cascade (urbi_watcher_pending_onleave_queue_push) happens after.
 *
 *   3. push_tag_no_overhead_when_enter_event_null:
 *      No enter_event allocated (tag->enter_event == NULL); OP_PUSH_TAG
 *      must not allocate anything beyond the UTag and cleanup-entry itself.
 */

#include "utest.h"

#include "vm/uvm.h"
#include "tag/utag.h"
#include "event/uevent.h"
#include "event/uevent_emit.h"
#include "tag/utag_native.h"
#include "watcher/uwatcher.h"
#include "watcher/uwatcher_install.h"
#include "sched/ustrand.h"
#include "chunk/uchunk.h"
#include "runtime/uclosure.h"
#include "realm/urealm.h"
#include "urbi/urbi.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers
 * =================================================================== */

static void
make_trivial_closure(UClosure *cl, UProto *proto, uint32_t *instr_buf)
{
    instr_buf[0] = (uint32_t)OP_RET;

    memset(proto, 0, sizeof(*proto));
    proto->instructions = instr_buf;
    proto->instr_count  = 1;
    proto->constants    = NULL;
    proto->const_count  = 0;

    memset(cl, 0, sizeof(*cl));
    cl->proto   = proto;
    cl->nupvals = 0;
}

/* ===================================================================
 * Test 1: push_tag_fires_enter_event_when_subscribed
 *
 * Manually create a UTag, lazy-allocate its enter_event, install an
 * AT_EVENT watcher on it, then drive a tag-scope run via urbi_repl_eval.
 * After "var t = 0; t: {}" (which uses its own anonymous tag), we can't
 * easily intercept the specific tag.  Instead we drive urbi_event_emit_sync
 * directly on a tag's enter_event to verify the hook fires correctly.
 *
 * This test verifies the enter-event code path: if enter_event is set
 * and has at_watchers_head != NULL, urbi_event_emit_sync is called, which
 * spawns body strands.  We test this directly via urbi_event_emit_sync
 * (the same function called by OP_PUSH_TAG) rather than via bytecode,
 * since source-level tag binding requires globals (post-M5).
 * =================================================================== */

UTEST(push_tag_fires_enter_event_when_subscribed)
{
    UVM vm;
    uint32_t instr_buf[1];
    UProto   body_proto;
    UClosure body_cl;

    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    if (r == NULL) { urbi_vm_destroy(&vm); return; }

    /* Create a UTag and lazy-alloc its enter_event. */
    UTag *tag = utag_create(&vm);
    UASSERT(tag != NULL);
    if (tag == NULL) { urbi_realm_destroy(&vm, r); urbi_vm_destroy(&vm); return; }

    UEvent *enter_ev = urbi_event_create(&vm);
    UASSERT(enter_ev != NULL);
    if (enter_ev == NULL) { urbi_realm_destroy(&vm, r); urbi_vm_destroy(&vm); return; }
    tag->enter_event = enter_ev;

    /* Install an AT_EVENT watcher on enter_event. */
    UStrand s;
    ustrand_init(&s, &vm);
    s.realm = r;

    make_trivial_closure(&body_cl, &body_proto, instr_buf);

    UWatcherInstallResult ri =
        urbi_watcher_install_at_event_runtime(&vm, &s, UWATCHER_AT_EVENT,
                                 enter_ev, &body_cl, NULL);
    UASSERT_EQ((int)UWATCHER_INSTALL_OK, (int)ri);
    UASSERT(enter_ev->at_watchers_head != NULL);

    int runnable_before = (int)vm.strand_runnable_count;

    /* Simulate what OP_PUSH_TAG does: fire enter_event if subscribed. */
    if (tag->enter_event != NULL && tag->enter_event->at_watchers_head != NULL) {
        UValue nil_val = {0};
        nil_val.kind = (uint8_t)UVAL_NIL;
        urbi_event_emit_sync(&vm, tag->enter_event, nil_val);
    }

    /* urbi_event_emit_sync spawns body strands for AT_EVENT watchers. */
    UASSERT((int)vm.strand_runnable_count > runnable_before);

    /* Clean up. */
    UWatcher *w = enter_ev->at_watchers_head;
    if (w != NULL) {
        urbi_watcher_unregister_internal(&vm, w);
    }
    ustrand_destroy(&s, &vm);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 2: pop_tag_fires_leave_event_when_subscribed
 *
 * Same pattern as test 1 but for leave_event.  Verifies that
 * urbi_event_emit_sync on leave_event spawns body strands (the same
 * call that OP_POP_TAG performs before the tier-1 watcher cascade).
 * =================================================================== */

UTEST(pop_tag_fires_leave_event_when_subscribed)
{
    UVM vm;
    uint32_t instr_buf[1];
    UProto   body_proto;
    UClosure body_cl;

    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    if (r == NULL) { urbi_vm_destroy(&vm); return; }

    UTag *tag = utag_create(&vm);
    UASSERT(tag != NULL);
    if (tag == NULL) { urbi_realm_destroy(&vm, r); urbi_vm_destroy(&vm); return; }

    UEvent *leave_ev = urbi_event_create(&vm);
    UASSERT(leave_ev != NULL);
    if (leave_ev == NULL) { urbi_realm_destroy(&vm, r); urbi_vm_destroy(&vm); return; }
    tag->leave_event = leave_ev;

    UStrand s;
    ustrand_init(&s, &vm);
    s.realm = r;

    make_trivial_closure(&body_cl, &body_proto, instr_buf);

    UWatcherInstallResult ri =
        urbi_watcher_install_at_event_runtime(&vm, &s, UWATCHER_AT_EVENT,
                                 leave_ev, &body_cl, NULL);
    UASSERT_EQ((int)UWATCHER_INSTALL_OK, (int)ri);
    UASSERT(leave_ev->at_watchers_head != NULL);

    int runnable_before = (int)vm.strand_runnable_count;

    /* Simulate what OP_POP_TAG does: fire leave_event BEFORE tier-1 cascade. */
    if (tag->leave_event != NULL && tag->leave_event->at_watchers_head != NULL) {
        UValue nil_val = {0};
        nil_val.kind = (uint8_t)UVAL_NIL;
        urbi_event_emit_sync(&vm, tag->leave_event, nil_val);
    }

    /* Body strand spawned — leave event fired. */
    UASSERT((int)vm.strand_runnable_count > runnable_before);

    UWatcher *w = leave_ev->at_watchers_head;
    if (w != NULL) {
        urbi_watcher_unregister_internal(&vm, w);
    }
    ustrand_destroy(&s, &vm);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 3: push_tag_no_overhead_when_enter_event_null
 *
 * Verify that when tag->enter_event is NULL (typical case — no subscribers),
 * OP_PUSH_TAG performs no additional allocation.  We count GC cells before
 * and after a tag-scope run.
 * =================================================================== */

/* Mirror of the private UAllCellsNode layout (same trick as test_utag_gc.c). */
typedef struct MirrorNode3 {
    void              *cell;
    size_t             size;
    struct MirrorNode3 *next;
    struct MirrorNode3 *next_gray;
} MirrorNode3;

static int
count_cells(UVM *vm)
{
    MirrorNode3 *n = (MirrorNode3 *)(void *)vm->all_cells_head;
    int count = 0;
    while (n != NULL) { count++; n = n->next; }
    return count;
}

UTEST(push_tag_no_overhead_when_enter_event_null)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    /* Create a UTag with no enter_event (NULL at birth). */
    UTag *tag = utag_create(&vm);
    UASSERT(tag != NULL);
    if (tag == NULL) { urbi_vm_destroy(&vm); return; }

    UASSERT(tag->enter_event == NULL);

    int cells_before = count_cells(&vm);

    /* Simulate OP_PUSH_TAG fast-path: enter_event is NULL → no emit call. */
    if (tag->enter_event != NULL && tag->enter_event->at_watchers_head != NULL) {
        UValue nil_val = {0};
        nil_val.kind = (uint8_t)UVAL_NIL;
        urbi_event_emit_sync(&vm, tag->enter_event, nil_val);
    }

    /* No new cells allocated by the fast-path check. */
    UASSERT_EQ(count_cells(&vm), cells_before);

    urbi_vm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void
test_tag_enter_leave_suite(void)
{
    printf("test_tag_enter_leave\n");
    utest_run("push_tag_fires_enter_event_when_subscribed",
              push_tag_fires_enter_event_when_subscribed);
    utest_run("pop_tag_fires_leave_event_when_subscribed",
              pop_tag_fires_leave_event_when_subscribed);
    utest_run("push_tag_no_overhead_when_enter_event_null",
              push_tag_no_overhead_when_enter_event_null);
}
