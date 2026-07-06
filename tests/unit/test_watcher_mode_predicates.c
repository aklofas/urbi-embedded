/* SPDX-License-Identifier: BSD-3-Clause */
/* test_watcher_mode_predicates.c — TDD for refactor-3 SCHED-16 / SCHED-18.
 *
 * URBI_GC_STRESS disarm: these tests hold GC cells (UEvent, UClosure) in bare
 * C locals and synthetic strands outside the realm graph.  GC stress would
 * sweep them between paired allocations; each test clears gc_stress_armed
 * after init (refactor-3 TEST-GAP-01 stress-exempt pattern).
 *
 * Four tests:
 *   1. whenever_event_unregister_unlinks_event_chain (SCHED-16, adaptation 4)
 *      urbi_watcher_unregister_internal on a WHENEVER_EVENT watcher must
 *      unlink it from event->at_watchers_head.  Pre-fix it did not
 *      (WHENEVER_EVENT was missing from the inline mode list) → dangling
 *      pointer on the next emit.  Red under ASan on the subsequent emit.
 *
 *   2. pool_destroy_whenever_event_no_dangling (carried fix)
 *      uwatcher_pool_destroy's slab walk must unlink WHENEVER_EVENT watchers
 *      from event->at_watchers_head before freeing the slab.  Pre-fix the
 *      watcher slot was left on the chain → the slab free leaves a dangling
 *      pointer; the next emit dereferences freed memory → ASan UAF.
 *
 *   3. sched18_mid_emit_watcher_not_fired (SCHED-18)
 *      A subscriber appended to at_watchers_head by a sync body during an
 *      in-progress urbi_event_emit_sync must NOT fire for that emission.  The
 *      tail-pin (last = tail at emit entry) stops the walk.  Pre-fix the
 *      next-capture was per-iteration, not pinned at entry; a sync body
 *      that appended w3 caused w3 to fire in the same emission.
 *
 *   4. mode_predicates_classify_all_modes
 *      UWATCHER_IS_EVENT_MODE covers exactly {5, 6, 7}; IS_COND_MODE covers
 *      exactly {1, 2, 3, 4}.  Tests both macros against every defined mode. */

#include "utest.h"

#include "event/uevent.h"
#include "event/uevent_emit.h"
#include "event/uevent_subscribe.h"
#include "watcher/uwatcher.h"
#include "watcher/uwatcher_install.h"
#include "sched/ustrand.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "chunk/uchunk.h"
#include "runtime/uclosure.h"
#include "runtime/umacros.h"

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
make_int_val(int i)
{
    UValue v;
    v.kind = UVAL_INT;
    v.v.i  = i;
    return v;
}

/* ===================================================================
 * Test 1: whenever_event_unregister_unlinks_event_chain
 *
 * SCHED-16 (adaptation 4): urbi_watcher_unregister_internal must unlink
 * a WHENEVER_EVENT watcher from event->at_watchers_head.
 * Pre-fix: the inline mode list in the unlink branch omitted
 * UWATCHER_WHENEVER_EVENT — the watcher was left on the chain while the
 * pool slot was returned to the freelist.  A subsequent emit walks freed
 * (or reused) pool memory → UAF under ASan.
 * =================================================================== */

UTEST(whenever_event_unregister_unlinks_event_chain)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body;

    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    if (!r) { urbi_vm_destroy(&vm); return; }

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);
    if (!e) { urbi_realm_destroy(&vm, r); urbi_vm_destroy(&vm); return; }

    make_trivial_closure(&body, &proto, instr);

    /* Wire a strand so urbi_watcher_install_at_event_runtime can resolve owning_tag/realm. */
    UStrand s;
    ustrand_init(&s, &vm);
    s.realm = r;

    /* Install a WHENEVER_EVENT watcher — lands on e->at_watchers_head. */
    UWatcherInstallResult ir =
        urbi_watcher_install_at_event_runtime(&vm, &s, UWATCHER_WHENEVER_EVENT, e, &body, NULL);
    UASSERT_EQ((int)UWATCHER_INSTALL_OK, (int)ir);

    UWatcher *w = e->at_watchers_head;
    UASSERT(w != NULL);
    UASSERT_EQ((int)UWATCHER_WHENEVER_EVENT, (int)w->mode);

    /* Unregister DIRECTLY (not via the tag-stop drain path which has its own
     * complete list).  Pre-fix: the :288 mode branch in uwatcher.c omits
     * WHENEVER_EVENT — the watcher is NOT unlinked from at_watchers_head. */
    urbi_watcher_unregister_internal(&vm, w);

    /* The watcher must have been unlinked from the event chain.
     * Pre-fix: at_watchers_head still == w (dangling pool slot). */
    UASSERT(e->at_watchers_head == NULL);

    /* Only emit if the chain was properly cleaned (guard against crashing the
     * runner pre-fix; under ASan the unguarded emit would be the primary UAF
     * indicator).  Post-fix: head == NULL → emit is safe. */
    if (e->at_watchers_head == NULL) {
        urbi_event_emit_async(&vm, e, make_int_val(1));
    }

    ustrand_destroy(&s, &vm);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 2: pool_destroy_whenever_event_no_dangling  (carried fix)
 *
 * uwatcher_pool_destroy's slab walk must unlink WHENEVER_EVENT watchers
 * from event->at_watchers_head before freeing the slab.
 * Pre-fix: the slab walk's inline mode list omitted WHENEVER_EVENT.
 * Post-destroy at_watchers_head must be NULL; an emit after the free must
 * not dereference freed memory (ASan heap-use-after-free if not fixed).
 * =================================================================== */

UTEST(pool_destroy_whenever_event_no_dangling)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body;

    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    if (!r) { urbi_vm_destroy(&vm); return; }

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);
    if (!e) { urbi_realm_destroy(&vm, r); urbi_vm_destroy(&vm); return; }

    make_trivial_closure(&body, &proto, instr);

    UStrand s;
    ustrand_init(&s, &vm);
    /* Deliberately leave s.realm = NULL: tag-less watcher.
     * uwatcher_pool_destroy's slab walk is specifically for tag-less event
     * watchers — tagged ones are torn down by the tag-stop cascade before
     * pool_destroy runs.  A non-NULL realm would link the watcher into
     * r->tag->member_watchers_head, which pool_destroy does not unlink;
     * utag_destroy would then assert member_watchers_head == NULL and crash.
     * The realm r is still alive (urbi_vm_destroy cleans it up). */

    /* Install WHENEVER_EVENT watcher — NOT on active_watchers_head (only cond
     * watchers thread there), so drain_watcher_list won't touch it. */
    UWatcherInstallResult ir =
        urbi_watcher_install_at_event_runtime(&vm, &s, UWATCHER_WHENEVER_EVENT, e, &body, NULL);
    UASSERT_EQ((int)UWATCHER_INSTALL_OK, (int)ir);

    UASSERT(e->at_watchers_head != NULL);
    UASSERT_EQ(1u, (unsigned)vm.watchers->active_count);

    /* Directly call pool_destroy (simulates the VM teardown path).
     * Pre-fix: slab walk skips WHENEVER_EVENT → watcher stays on
     * e->at_watchers_head while the slab is freed → dangling pointer. */
    uwatcher_pool_destroy(&vm);

    /* Slab freed.  The watcher must have been unlinked from the event chain
     * before the slab was freed (post-fix).  Pre-fix: still != NULL. */
    UASSERT(e->at_watchers_head == NULL);

    /* active_count must be decremented to 0. Pre-fix: still == 1. */
    UASSERT_EQ(0u, (unsigned)vm.watchers->active_count);

    /* Emit the event, guarded so the runner doesn't crash pre-fix.
     * Post-fix: head == NULL → safe to emit (no dereference).
     * Under ASan without the guard, the emit would show heap-use-after-free
     * because the slab freed by uwatcher_pool_destroy still contains
     * the dangling at_watchers_head pointer (pre-fix). */
    if (e->at_watchers_head == NULL) {
        urbi_event_emit_async(&vm, e, make_int_val(2));
    }

    /* urbi_vm_destroy skips pool_destroy (pool_base == NULL from above). */
    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 3: sched18_mid_emit_watcher_not_fired  (SCHED-18)
 *
 * Legacy contract: urbi_event_emit_sync fires PRE-REGISTERED subscribers only.
 * A subscriber installed mid-emit by a sync body (into at_watchers_head)
 * must NOT receive the in-flight emission; it fires on the NEXT emit.
 *
 * Setup:
 *   w1 (AT_EVENT_SYNC) → w2 (AT_EVENT_SYNC) installed before emit.
 *   after_sync_body hook fires after w1's body → installs w3 (AT_EVENT).
 *   w3 is appended to at_watchers_head (now: w1→w2→w3).
 *
 * With SCHED-18 tail-pin (last = w2 captured before any body runs):
 *   Walk stops at w2; w3 NOT fired → w3->body_strand == NULL  ✓
 *   Second emit (w3 now pre-registered): w3->body_strand != NULL  ✓
 *
 * Pre-fix (no tail-pin, per-iteration next snapshot):
 *   After w1's body installs w3: w2->next_in_event = w3.
 *   w2 iteration captures next = w3.  w3 is walked and spawned → FAIL.
 * =================================================================== */

/* File-scope context for the SCHED-18 after_sync_body hook. */
static struct {
    UStrand  *inst_strand;
    UClosure *w3_body;
    UWatcher *w3;           /* NULL until hook fires; set to the installed slot */
    int       hook_count;
} g_sched18;

static void
sched18_hook(struct UVM *vm, struct UWatcher *w)
{
    if (g_sched18.hook_count++ == 0) {
        /* First call (after w1's body): install w3 mid-emit. */
        UWatcherInstallResult r =
            urbi_watcher_install_at_event_runtime(vm, g_sched18.inst_strand,
                                     UWATCHER_AT_EVENT,
                                     (struct UEvent *)w->event,
                                     g_sched18.w3_body, NULL);
        if (r == UWATCHER_INSTALL_OK) {
            /* w3 is the tail of at_watchers_head. */
            struct UWatcher *tail = ((struct UEvent *)w->event)->at_watchers_head;
            while (tail->next_in_event) tail = tail->next_in_event;
            g_sched18.w3 = tail;
        }
    }
}

UTEST(sched18_mid_emit_watcher_not_fired)
{
    UVM vm;
    uint32_t instr1[1], instr2[1], instr3[1];
    UProto   proto1, proto2, proto3;
    UClosure body1, body2, body3;

    urbi_vm_init(&vm, NULL, NULL);
    vm.gc_stress_armed = 0;   /* URBI_GC_STRESS disarmed — see file banner */

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    if (!r) { urbi_vm_destroy(&vm); return; }

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);
    if (!e) { urbi_realm_destroy(&vm, r); urbi_vm_destroy(&vm); return; }

    make_trivial_closure(&body1, &proto1, instr1);
    make_trivial_closure(&body2, &proto2, instr2);
    make_trivial_closure(&body3, &proto3, instr3);

    UStrand s;
    ustrand_init(&s, &vm);
    s.realm = r;

    /* Install w1 (AT_EVENT_SYNC) then w2 (AT_EVENT_SYNC). */
    UWatcherInstallResult r1 =
        urbi_watcher_install_at_event_runtime(&vm, &s, UWATCHER_AT_EVENT_SYNC, e, &body1, NULL);
    UASSERT_EQ((int)UWATCHER_INSTALL_OK, (int)r1);

    UWatcherInstallResult r2 =
        urbi_watcher_install_at_event_runtime(&vm, &s, UWATCHER_AT_EVENT_SYNC, e, &body2, NULL);
    UASSERT_EQ((int)UWATCHER_INSTALL_OK, (int)r2);

    /* Arm the after_sync_body hook: installs w3 (AT_EVENT async) after w1
     * runs, simulating a sync body calling an install inside the emit. */
    g_sched18.inst_strand = &s;
    g_sched18.w3_body     = &body3;
    g_sched18.w3          = NULL;
    g_sched18.hook_count  = 0;
    vm.test_hooks->after_sync_body = sched18_hook;

    /* === First emit === */
    urbi_event_emit_sync(&vm, e, make_int_val(10));

    /* Hook must have fired (at least once, for w1). */
    UASSERT(g_sched18.hook_count >= 1);

    /* w3 must have been installed by the hook. */
    UASSERT(g_sched18.w3 != NULL);

    /* SCHED-18 assertion: w3 must NOT have been spawned in the in-flight emit.
     * Pre-fix: w3->body_strand != NULL (w3 was walked in the same emit). */
    UASSERT(g_sched18.w3->body_strand == NULL);

    /* Disarm the hook so the second emit doesn't re-install w3. */
    vm.test_hooks->after_sync_body = NULL;
    g_sched18.hook_count = 0;

    /* === Second emit === w3 is now pre-registered (it's on at_watchers_head).
     * SCHED-18 assertion: w3 DOES fire on the next emit. */
    urbi_event_emit_sync(&vm, e, make_int_val(11));

    /* w3 is AT_EVENT (async): urbi_watcher_do_spawn_body_coroutine sets body_strand. */
    UASSERT(g_sched18.w3->body_strand != NULL);

    /* Cleanup. */
    vm.test_hooks->after_sync_body = NULL;
    while (e->at_watchers_head != NULL)
        urbi_watcher_unregister_internal(&vm, e->at_watchers_head);
    ustrand_destroy(&s, &vm);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Test 4: mode_predicates_classify_all_modes
 *
 * Verify UWATCHER_IS_EVENT_MODE and UWATCHER_IS_COND_MODE classify every
 * defined mode correctly.  Documents the invariant: modes 5-7 are event
 * (thread on event->at_watchers_head); modes 1-4 are cond (thread on
 * vm->active_watchers_head).  There are no "neither" modes.
 * =================================================================== */

UTEST(mode_predicates_classify_all_modes)
{
    /* Cond modes (1-4): UWATCHER_IS_EVENT_MODE must be false. */
    UASSERT(!UWATCHER_IS_EVENT_MODE(UWATCHER_AT));
    UASSERT(!UWATCHER_IS_EVENT_MODE(UWATCHER_WHENEVER));
    UASSERT(!UWATCHER_IS_EVENT_MODE(UWATCHER_AT_SYNC));
    UASSERT(!UWATCHER_IS_EVENT_MODE(UWATCHER_WAITUNTIL));

    /* Event modes (5-7): UWATCHER_IS_EVENT_MODE must be true. */
    UASSERT( UWATCHER_IS_EVENT_MODE(UWATCHER_AT_EVENT));
    UASSERT( UWATCHER_IS_EVENT_MODE(UWATCHER_AT_EVENT_SYNC));
    UASSERT( UWATCHER_IS_EVENT_MODE(UWATCHER_WHENEVER_EVENT));

    /* IS_COND_MODE is the negation. */
    UASSERT( UWATCHER_IS_COND_MODE(UWATCHER_AT));
    UASSERT( UWATCHER_IS_COND_MODE(UWATCHER_WHENEVER));
    UASSERT( UWATCHER_IS_COND_MODE(UWATCHER_AT_SYNC));
    UASSERT( UWATCHER_IS_COND_MODE(UWATCHER_WAITUNTIL));
    UASSERT(!UWATCHER_IS_COND_MODE(UWATCHER_AT_EVENT));
    UASSERT(!UWATCHER_IS_COND_MODE(UWATCHER_AT_EVENT_SYNC));
    UASSERT(!UWATCHER_IS_COND_MODE(UWATCHER_WHENEVER_EVENT));

    /* Numeric constants must not have drifted. */
    UASSERT_EQ(1, (int)UWATCHER_AT);
    UASSERT_EQ(2, (int)UWATCHER_WHENEVER);
    UASSERT_EQ(3, (int)UWATCHER_AT_SYNC);
    UASSERT_EQ(4, (int)UWATCHER_WAITUNTIL);
    UASSERT_EQ(5, (int)UWATCHER_AT_EVENT);
    UASSERT_EQ(6, (int)UWATCHER_AT_EVENT_SYNC);
    UASSERT_EQ(7, (int)UWATCHER_WHENEVER_EVENT);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_watcher_mode_predicates_suite(void)
{
    printf("test_watcher_mode_predicates\n");
    utest_run("whenever_event_unregister_unlinks_event_chain",
              whenever_event_unregister_unlinks_event_chain);
    utest_run("pool_destroy_whenever_event_no_dangling",
              pool_destroy_whenever_event_no_dangling);
    utest_run("sched18_mid_emit_watcher_not_fired",
              sched18_mid_emit_watcher_not_fired);
    utest_run("mode_predicates_classify_all_modes",
              mode_predicates_classify_all_modes);
}
