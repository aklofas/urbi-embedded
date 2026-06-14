/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: urbi_vm_liveness() + QUIESCENT semantics (refactor-3 SCHED-13,
 * SCHED-06, VM-12; v0.13.3-scheduler-liveness Task 2).
 *
 * Owner decision (2026-06-11, option a): armed watchers (all modes) and
 * SUSPENDED/WAITING strands do NOT block QUIESCENT — they are external-input
 * work, re-armed by host slot writes, injected events, or tag unblock /
 * unfreeze.  urbi_step's verdict ladder: FATAL -> RUNNING (runnable strands,
 * pending ISR-ring events, pending host calls, pending dirty/onleave work)
 * -> WAKE_AT (sleepers/periodics) -> QUIESCENT.
 *
 * Three findings pinned here:
 *   SCHED-06 — install_at_event_runtime skipped the active_count bump while
 *              urbi_watcher_unregister_internal decrements unconditionally
 *              (uint32 underflow -> quiescent-never).
 *   SCHED-13 — three divergent quiescence formulas (sched_quiescent,
 *              urbi_step's post-loop returns, urbi_vm_has_live_work) folded
 *              into one urbi_vm_liveness().
 *   VM-12   — SUSPENDED strands were invisible to every liveness query.
 */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include <stddef.h>
#include <string.h>

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"
#include "runtime/ucleanup.h"
#include "tag/utag.h"
#include "watcher/uwatcher.h"

#define UTEST(name) static void name(void)

/* ===================================================================
 * Case 1: SCHED-06 — event-watcher install/unregister active_count
 * symmetry.
 *
 * Pre-fix: install_at_event_runtime skipped the active_count bump
 * ("the count tracks cond-watcher pressure") while every teardown path
 * (urbi_watcher_unregister_internal, the pool-destroy drains) decrements
 * -> uint32 wrap -> a VM that ever hosted an event watcher never reports
 * quiescent again.  Post-fix: the count covers ALL armed watchers; the
 * decrements assert > 0 instead of saturating (masking forbidden).
 * =================================================================== */
UTEST(active_count_event_watcher_symmetric)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    int rc = utest_e2e_compile_and_run(&vm,
        "var e = Event.new(); at (e?) { 1 };",
        NULL);
    UASSERT_EQ(URBI_OK, rc);
    if (rc == URBI_OK) {
        /* SCHED-06 red: pre-fix this reads 0 — the install skipped the
         * bump.  Post-fix the armed event watcher is counted. */
        UASSERT_EQ(1U, vm.watchers->active_count);
    }

    /* Teardown unregisters via the realm-tag stop cascade + pool drain;
     * pre-fix (with the no-saturation asserts in place) the unconditional
     * decrement of a never-incremented count aborts here. */
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 2: owner decision (a) — a VM whose only live work is an armed
 * cond watcher reports QUIESCENT.
 *
 * Pre-fix: urbi_step's post-loop check returned RUNNING whenever
 * watchers->active_count > 0, so a host driving an idle-but-armed VM
 * busy-spun forever.  Post-fix: armed reactive surface does not block
 * QUIESCENT; the host re-steps after providing input (slot write /
 * event injection).
 * =================================================================== */
UTEST(quiescent_with_armed_cond_watcher)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *gr = urbi_realm_global(&vm);
    UASSERT(gr != NULL);
    if (gr == NULL) { urbi_vm_destroy(&vm); return; }

    /* Pre-install Realm.flag = false via the C API so the script write
     * below cannot dirty an already-observed cell (the watcher installs
     * after; install-time cond trace only reads). */
    UValue f = {0};
    f.kind = (uint8_t)UVAL_BOOL;
    f.v.i  = 0;
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, gr, "flag", 4, f));

    int rc = utest_e2e_compile_and_run(&vm,
        "at (Realm.flag) Realm.hit = 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ(1U, vm.watchers->active_count);   /* armed cond watcher */

    /* Drain any loader-strand residue, then observe the settled verdict. */
    UASSERT_EQ(1, utest_e2e_run_to_no_runnable(&vm));

    UStepResult sr = urbi_step(&vm, 1000, NULL);
    /* SCHED-13 red: pre-fix this is URBI_STEP_RUNNING (host busy-spin). */
    UASSERT_EQ((int)URBI_STEP_QUIESCENT, (int)sr);

    /* The armed watcher still shows up on the inclusive host query. */
    uint32_t watchers = 0;
    UASSERT(urbi_vm_has_live_work(&vm, NULL, &watchers, NULL));
    UASSERT_EQ(1U, watchers);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 3: VM-12 — strand_suspended_count is maintained; the inclusive
 * host query reports it; urbi_step still says QUIESCENT (the strand is
 * host-resumable, not runnable).
 * =================================================================== */
UTEST(suspended_strand_counted_and_quiescent)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    if (r == NULL) { urbi_vm_destroy(&vm); return; }

    UStrand *s = urbi_strand_create(&vm, r, NULL);
    UASSERT(s != NULL);
    if (s == NULL) { urbi_vm_destroy(&vm); return; }

    /* Member-strand setup mirrors tests/unit/test_tag_state.c: link the
     * strand onto a tag via a synthetic TAG_SCOPE cleanup entry (what
     * OP_PUSH_TAG does at runtime). */
    UTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.type_tag = (uint8_t)UTYPE_TAG;
    {
        UCleanupEntry *e = strand_cleanup_push(s);
        UASSERT(e != NULL);
        e->kind          = (uint8_t)UCLEANUP_TAG_SCOPE;
        e->owning_tag    = &tag;
        e->catch_pattern = NULL;
        e->strand_back   = s;
        e->next_member   = tag.member_strands_head;
        tag.member_strands_head = e;
    }

    urbi_strand_start(&vm, s);   /* DORMANT -> READY */
    UASSERT_EQ(1U, vm.strand_runnable_count);
    UASSERT_EQ(0U, vm.strand_suspended_count);

    /* Cross-strand suspend: READY -> SUSPENDED (leaves the counted set). */
    UASSERT_EQ(URBI_OK, urbi_tag_block(&vm, &tag, utest_e2e_make_nil()));
    UASSERT_EQ((unsigned)USTRAND_SUSPENDED, (unsigned)USTRAND_GET_STATE(s));
    UASSERT_EQ(0U, vm.strand_runnable_count);
    /* VM-12 red: pre-fix the counter is never written (always 0). */
    UASSERT_EQ(1U, vm.strand_suspended_count);

    /* Inclusive host query: a SUSPENDED strand is live work (armed). */
    UASSERT(urbi_vm_has_live_work(&vm, NULL, NULL, NULL));

    /* But it does not block QUIESCENT — host-resumable, not runnable. */
    UStepResult sr = urbi_step(&vm, 1000, NULL);
    UASSERT_EQ((int)URBI_STEP_QUIESCENT, (int)sr);

    /* Resume: SUSPENDED -> READY; counter returns to 0.  (Do not step
     * again: the bare strand has no armed register stack.) */
    UASSERT_EQ(URBI_OK, urbi_tag_unblock(&vm, &tag));
    UASSERT_EQ((unsigned)USTRAND_READY, (unsigned)USTRAND_GET_STATE(s));
    UASSERT_EQ(0U, vm.strand_suspended_count);
    UASSERT_EQ(1U, vm.strand_runnable_count);

    urbi_realm_destroy(&vm, r);   /* frees s; unlinks the tag entry */
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry point.
 * =================================================================== */

void test_vm_liveness_suite(void);

void
test_vm_liveness_suite(void)
{
    utest_run("urbi_vm_liveness: event-watcher install/unregister keeps "
              "active_count symmetric (SCHED-06)",
              active_count_event_watcher_symmetric);
    utest_run("urbi_vm_liveness: armed cond watcher does not block QUIESCENT "
              "(SCHED-13, owner decision a)",
              quiescent_with_armed_cond_watcher);
    utest_run("urbi_vm_liveness: SUSPENDED strand counted + reported but "
              "QUIESCENT (VM-12)",
              suspended_strand_counted_and_quiescent);
}
