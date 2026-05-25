/* SPDX-License-Identifier: BSD-3-Clause */
/* W2/v0.10.2: AT_EVENT watcher fully unlinks from event chain on tag-stop.
 *
 * Closes reactive audit F2.
 *
 * Cases:
 *   1. at_event_unlink_from_event_chain_on_tag_stop:
 *      Install AT_EVENT watcher under a realm tag.  Tag-stop the realm.
 *      Assert e->at_watchers_head is NULL immediately after tag-stop
 *      (before any safepoint drain).
 *
 *   2. at_event_emit_after_tag_stop_no_spawn:
 *      Install AT_EVENT watcher under a realm tag.  Tag-stop.  Emit the
 *      event.  Step.  Assert strand_runnable_count did not increase —
 *      no zombie body strand was spawned.
 *
 *   3. at_event_tag_stop_emit_stress:
 *      Alternate tag-stop and emit 50 times.  After each cycle the
 *      at_watchers_head must be empty and no new strands must appear.
 *
 *   4. at_event_sync_unlink_from_event_chain_on_tag_stop:
 *      Same as case 1 but with UWATCHER_AT_EVENT_SYNC mode.
 *
 *   5. whenever_event_unlink_from_event_chain_on_tag_stop:
 *      Same as case 1 but with UWATCHER_WHENEVER_EVENT (W0 prereq).
 */

#include "utest.h"

#include "event/uevent.h"
#include "event/uevent_emit.h"
#include "watcher/uwatcher.h"
#include "watcher/uwatcher_install.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "tag/utag.h"
#include "chunk/uchunk.h"
#include "runtime/uclosure.h"
#include "urbi/urbi.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers
 * =================================================================== */

/* Count watchers on e->at_watchers_head. */
static int
at_watchers_count(UEvent *e)
{
    int n = 0;
    struct UWatcher *w = e->at_watchers_head;
    while (w) { n++; w = w->next_in_event; }
    return n;
}

/* Build a minimal UClosure wrapping a stack-local UProto with a single
 * OP_RET instruction.  proto/closure/instr storage is caller-provided. */
static void
make_ret_closure(UClosure *cl, UProto *proto, uint32_t *instr_buf)
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

/* Install an AT_EVENT (or AT_EVENT_SYNC / WHENEVER_EVENT) watcher under
 * the realm's tag.  Uses install_at_event_runtime via a stack-local
 * UStrand whose realm is wired so resolve_owning_tag's fallthrough
 * returns realm->tag. */
static UWatcherInstallResult
install_event_watcher(struct UVM *vm, struct URealm *realm,
                      uint8_t mode, struct UEvent *e,
                      struct UClosure *body)
{
    UStrand scratch_strand;
    ustrand_init(&scratch_strand, vm);
    scratch_strand.realm = realm;   /* resolve_owning_tag fallthrough → realm->tag */
    scratch_strand.state = USTRAND_STATE_RUNNING;

    UWatcherInstallResult rc =
        install_at_event_runtime(vm, &scratch_strand, mode, e, body, NULL);

    ustrand_destroy(&scratch_strand, vm);
    return rc;
}

/* ===================================================================
 * Case 1: at_event_unlink_from_event_chain_on_tag_stop
 *
 * Install one AT_EVENT watcher under realm->tag.  Tag-stop the realm.
 * at_watchers_head must be NULL IMMEDIATELY — before any safepoint drain.
 * =================================================================== */
UTEST(at_event_unlink_from_event_chain_on_tag_stop)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;
    UValue   nil = {0};

    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_ret_closure(&body_cl, &proto, instr);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    UWatcherInstallResult rc =
        install_event_watcher(&vm, r, UWATCHER_AT_EVENT, e, &body_cl);
    UASSERT_EQ((int)URBI_INSTALL_OK, (int)rc);

    /* Pre-condition: watcher must be on the event chain. */
    UASSERT_EQ(1, at_watchers_count(e));

    /* Tag-stop the realm. */
    int sr = urbi_tag_stop(&vm, r->tag, nil);
    UASSERT_EQ(URBI_OK, sr);

    /* W2 invariant: at_watchers_head must be empty IMMEDIATELY after tag-stop,
     * before any safepoint drain runs. */
    UASSERT_EQ(0, at_watchers_count(e));

    /* Drain + destroy. */
    drain_pending_onleave_queue(&vm);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 2: at_event_emit_after_tag_stop_no_spawn
 *
 * After tag-stop, emit must NOT spawn a body strand.
 * =================================================================== */
UTEST(at_event_emit_after_tag_stop_no_spawn)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;
    UValue   nil = {0};

    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_ret_closure(&body_cl, &proto, instr);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    UWatcherInstallResult rc =
        install_event_watcher(&vm, r, UWATCHER_AT_EVENT, e, &body_cl);
    UASSERT_EQ((int)URBI_INSTALL_OK, (int)rc);

    /* Tag-stop. */
    int sr = urbi_tag_stop(&vm, r->tag, nil);
    UASSERT_EQ(URBI_OK, sr);
    UASSERT_EQ(0, at_watchers_count(e));

    /* Defence-in-depth: emit on the stopped event must NOT spawn a body. */
    int runnable_before = (int)vm.strand_runnable_count;
    c_event_emit_async(&vm, e, nil);
    /* Step briefly to let any runnable strands execute. */
    urbi_step(&vm, 256, NULL);

    /* No new strand must have been enqueued as runnable. */
    UASSERT_EQ(runnable_before, (int)vm.strand_runnable_count);

    drain_pending_onleave_queue(&vm);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 3: at_event_tag_stop_emit_stress
 *
 * 50 iterations: each reinstalls the watcher, tag-stops, emits.
 * at_watchers_head must be empty after each stop.
 * No new strands after each emit.
 * =================================================================== */
UTEST(at_event_tag_stop_emit_stress)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;
    UValue   nil = {0};

    urbi_vm_init(&vm, NULL, NULL);
    make_ret_closure(&body_cl, &proto, instr);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    for (int i = 0; i < 50; i++) {
        /* Fresh realm each iteration (re-creates the tag). */
        URealm *r = urbi_realm_create(&vm);
        UASSERT(r != NULL);

        UWatcherInstallResult rc =
            install_event_watcher(&vm, r, UWATCHER_AT_EVENT, e, &body_cl);
        UASSERT_EQ((int)URBI_INSTALL_OK, (int)rc);
        UASSERT_EQ(1, at_watchers_count(e));

        /* Tag-stop. */
        int sr = urbi_tag_stop(&vm, r->tag, nil);
        UASSERT_EQ(URBI_OK, sr);
        UASSERT_EQ(0, at_watchers_count(e));

        /* Emit must not spawn. */
        int runnable_before = (int)vm.strand_runnable_count;
        c_event_emit_async(&vm, e, nil);
        urbi_step(&vm, 64, NULL);
        UASSERT_EQ(runnable_before, (int)vm.strand_runnable_count);

        drain_pending_onleave_queue(&vm);
        urbi_realm_destroy(&vm, r);
    }

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 4: at_event_sync_unlink_from_event_chain_on_tag_stop
 *
 * Same as case 1 but using UWATCHER_AT_EVENT_SYNC mode.
 * =================================================================== */
UTEST(at_event_sync_unlink_from_event_chain_on_tag_stop)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;
    UValue   nil = {0};

    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_ret_closure(&body_cl, &proto, instr);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    UWatcherInstallResult rc =
        install_event_watcher(&vm, r, UWATCHER_AT_EVENT_SYNC, e, &body_cl);
    UASSERT_EQ((int)URBI_INSTALL_OK, (int)rc);
    UASSERT_EQ(1, at_watchers_count(e));

    int sr = urbi_tag_stop(&vm, r->tag, nil);
    UASSERT_EQ(URBI_OK, sr);

    /* AT_EVENT_SYNC must also be unlinked immediately. */
    UASSERT_EQ(0, at_watchers_count(e));

    drain_pending_onleave_queue(&vm);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 5: whenever_event_unlink_from_event_chain_on_tag_stop
 *
 * Same as case 1 but using UWATCHER_WHENEVER_EVENT (W0 prereq).
 * =================================================================== */
UTEST(whenever_event_unlink_from_event_chain_on_tag_stop)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;
    UValue   nil = {0};

    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_ret_closure(&body_cl, &proto, instr);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    UWatcherInstallResult rc =
        install_event_watcher(&vm, r, UWATCHER_WHENEVER_EVENT, e, &body_cl);
    UASSERT_EQ((int)URBI_INSTALL_OK, (int)rc);
    UASSERT_EQ(1, at_watchers_count(e));

    int sr = urbi_tag_stop(&vm, r->tag, nil);
    UASSERT_EQ(URBI_OK, sr);

    /* WHENEVER_EVENT must also be unlinked immediately. */
    UASSERT_EQ(0, at_watchers_count(e));

    drain_pending_onleave_queue(&vm);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry point.
 * =================================================================== */

void
test_at_event_unlink_on_tag_stop_suite(void)
{
    utest_run("at_event: watcher unlinks from event chain on tag-stop (AT_EVENT)",
              at_event_unlink_from_event_chain_on_tag_stop);
    utest_run("at_event: emit after tag-stop does not spawn body strand",
              at_event_emit_after_tag_stop_no_spawn);
    utest_run("at_event: tag-stop/emit stress (50 cycles, no leaks)",
              at_event_tag_stop_emit_stress);
    utest_run("at_event: AT_EVENT_SYNC also unlinks from event chain on tag-stop",
              at_event_sync_unlink_from_event_chain_on_tag_stop);
    utest_run("at_event: WHENEVER_EVENT also unlinks from event chain on tag-stop",
              whenever_event_unlink_from_event_chain_on_tag_stop);
}
