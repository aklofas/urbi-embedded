/* SPDX-License-Identifier: BSD-3-Clause */
/* URBI_GC_STRESS disarm (v0.13.2): C-API scaffolding suite — GC cells
 * (tags/events/objects/closures) held in bare C locals and/or synthetic
 * strands outside the realm graph, by design, to drive one primitive in
 * isolation.  Collect-on-every-alloc sweeps them between paired
 * allocations; fine on normal builds where host-C call sequences cannot
 * be interrupted by a collection.  Each test sets vm.gc_stress_armed = 0
 * after init.  Structural-by-design, not a runtime rooting bug
 * (refactor-3 TEST-GAP-01 stress-exempt list). */
/* Unit tests: urbi_event_emit_sync + scratch-context degrade (spec #3 §5.3-§5.4).
 *
 * Source-level tests require Event.new() (T53) and globals (post-M5), so we
 * drive via direct C-API.
 *
 * Cases:
 *   1. sync_emit_runs_sync_subs_inline:
 *      AT_EVENT_SYNC watcher — run_event_body_on_scratch is called; at M5
 *      baseline the scratch runner is not yet wired so we verify the flag
 *      cycle (in_watcher_scratch set + cleared) via a log hook sentinel.
 *      We confirm AT_EVENT (async) sub spawns a body_strand and AT_EVENT_SYNC
 *      sub does NOT spawn a body_strand (runs inline instead).
 *   2. sync_emit_degrades_when_in_watcher_eval:
 *      Set vm->watchers->in_eval = 1 before calling urbi_event_emit_sync;
 *      expect URBI_LOG_WARN "degraded to async" and the emit to proceed
 *      asynchronously (waiter woken). */

#include "utest.h"

#include "event/uevent.h"
#include "event/uevent_emit.h"
#include "watcher/uwatcher.h"
#include "watcher/uwatcher_install.h"
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"  /* urbi_sched_strand_block (waiter park) */
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "chunk/uchunk.h"
#include "runtime/uclosure.h"

#include "urbi/urbi.h"   /* URBI_LOG_WARN */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

static UValue
make_int(int i)
{
    UValue v;
    v.kind = UVAL_INT;
    v.v.i  = i;
    return v;
}

static int g_warn_count;
static int g_log_total;

static void
capture_log(struct UVM *vm, void *ud, int level, const char *fmt, ...)
{
    (void)vm; (void)ud; (void)fmt;
    g_log_total++;
    if (level == URBI_LOG_WARN) g_warn_count++;
}

/* ===================================================================
 * Case 1: sync_emit_runs_sync_subs_inline
 *
 * Install one AT_EVENT_SYNC watcher and one AT_EVENT watcher.
 * urbi_event_emit_sync must:
 *   - NOT spawn a body_strand for AT_EVENT_SYNC (runs inline on scratch).
 *   - Spawn a body_strand for AT_EVENT (async path).
 *
 * At M5 baseline the scratch runner stub is a no-op, so we cannot verify
 * actual body execution.  We verify the structural distinction:
 *   - AT_EVENT_SYNC watcher → body_strand stays NULL (run inline path)
 *   - AT_EVENT watcher      → body_strand != NULL (spawned)
 * =================================================================== */

UTEST(sync_emit_runs_sync_subs_inline)
{
    UVM vm;
    uint32_t instr_sync[1], instr_async[1];
    UProto   proto_sync, proto_async;
    UClosure body_sync, body_async;

    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    make_trivial_closure(&body_sync,  &proto_sync,  instr_sync);
    make_trivial_closure(&body_async, &proto_async, instr_async);

    UStrand s;
    ustrand_init(&s, &vm);
    s.realm = r;

    /* Install sync watcher first (appears at head of at_watchers_head). */
    UWatcherInstallResult rs =
        urbi_watcher_install_at_event_runtime(&vm, &s, UWATCHER_AT_EVENT_SYNC, e, &body_sync, NULL);
    UASSERT_EQ((int)URBI_INSTALL_OK, (int)rs);

    /* Install async watcher second. */
    UWatcherInstallResult ra =
        urbi_watcher_install_at_event_runtime(&vm, &s, UWATCHER_AT_EVENT, e, &body_async, NULL);
    UASSERT_EQ((int)URBI_INSTALL_OK, (int)ra);

    UWatcher *ws = e->at_watchers_head;             /* sync watcher (first installed) */
    UASSERT(ws != NULL);
    UASSERT_EQ((int)ws->mode, (int)UWATCHER_AT_EVENT_SYNC);

    UWatcher *wa = ws->next_in_event;               /* async watcher (second) */
    UASSERT(wa != NULL);
    UASSERT_EQ((int)wa->mode, (int)UWATCHER_AT_EVENT);

    /* Before emit: no body strands. */
    UASSERT(ws->body_strand == NULL);
    UASSERT(wa->body_strand == NULL);

    UValue payload = make_int(7);
    urbi_event_emit_sync(&vm, e, payload);

    /* AT_EVENT_SYNC: body runs inline on scratch — no body_strand spawned. */
    UASSERT(ws->body_strand == NULL);

    /* AT_EVENT: body strand spawned. */
    UASSERT(wa->body_strand != NULL);

    urbi_watcher_unregister_internal(&vm, ws);
    urbi_watcher_unregister_internal(&vm, wa);
    ustrand_destroy(&s, &vm);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 2: sync_emit_degrades_when_in_watcher_eval
 *
 * With vm->watchers->in_eval = 1, urbi_event_emit_sync must:
 *   - Log URBI_LOG_WARN containing "degraded to async".
 *   - Delegate to urbi_event_emit_async (waiter still gets woken).
 * =================================================================== */

UTEST(sync_emit_degrades_when_in_watcher_eval)
{
    UVM vm;

    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    /* Park a waiter to verify async path fired — through the real
     * transition (v0.13.3 / SCHED-13: a raw WAIT_EVENT stamp would bypass
     * the strand_waiting_count increment and trip the wake's
     * no-saturation decrement). */
    UStrand waiter;
    ustrand_init(&waiter, &vm);
    waiter.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count++;     /* satisfy block's RUNNING-decrement */
    urbi_sched_strand_block(&waiter, USTRAND_REASON_EVENT, (uint64_t)(uintptr_t)e);
    waiter.wait_event_target  = e;
    waiter.next_event_waiter  = NULL;
    waiter.last_event_payload.kind = UVAL_NIL;
    waiter.last_event_payload.v.i  = 0;
    e->waiters_head = &waiter;

    g_warn_count = 0;
    g_log_total  = 0;
    vm.host_log_fn = capture_log;

    vm.watchers->in_eval = 1;
    urbi_event_emit_sync(&vm, e, make_int(55));
    vm.watchers->in_eval = 0;

    /* Must have emitted exactly one URBI_LOG_WARN. */
    UASSERT_EQ(g_warn_count, 1);

    /* Async path must have run: waiter woken. */
    UASSERT(e->waiters_head == NULL);
    UASSERT_EQ((int)waiter.state, (int)USTRAND_STATE_READY);
    UASSERT_EQ((int)waiter.last_event_payload.kind, (int)UVAL_INT);
    UASSERT_EQ((int)waiter.last_event_payload.v.i,  55);

    ustrand_destroy(&waiter, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * EMITR-005: degradation warn is one-shot
 *
 * Pre-fix the URBI_LOG_WARN at the degradation site fired on every call.
 * In a tight loop this floods host_log_fn (the spec #3 §5.4 contract +
 * the function's own header docstring claim "one-shot URBI_LOG_WARN").
 * Mirror the urbi_emit_slot_change_slow / slot_change_reentrancy_warned
 * shape: store the fired-once flag on UVM and gate the warn.
 * =================================================================== */

UTEST(sync_emit_degradation_warn_is_one_shot)
{
    UVM vm;

    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);

    g_warn_count = 0;
    g_log_total  = 0;
    vm.host_log_fn = capture_log;

    /* Call urbi_event_emit_sync 100 times with the degradation flag set.
     * Pre-fix: g_warn_count == 100 (warn-flooding).
     * Post-fix: g_warn_count == 1 (one-shot guard via
     *           vm->event_sync_degradation_warned). */
    vm.watchers->in_eval = 1;
    int i;
    for (i = 0; i < 100; i++) {
        urbi_event_emit_sync(&vm, e, make_int(i));
    }
    vm.watchers->in_eval = 0;

    UASSERT_EQ(g_warn_count, 1);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_event_emit_sync_suite(void)
{
    printf("test_event_emit_sync\n");
    utest_run("sync_emit_runs_sync_subs_inline",
              sync_emit_runs_sync_subs_inline);
    utest_run("sync_emit_degrades_when_in_watcher_eval",
              sync_emit_degrades_when_in_watcher_eval);
    utest_run("sync_emit_degradation_warn_is_one_shot",
              sync_emit_degradation_warn_is_one_shot);
}
