/* SPDX-License-Identifier: BSD-3-Clause */
/* URBI_DEBUG-only invariant check: urbi_watcher_check_invariants.
 * spec #1 §7.2.  Task T29.
 *
 * Cases (all guarded by #if URBI_DEBUG — no-ops in release builds):
 *   1. watcher_invariant_holds_during_body_run:
 *      Install + spawn body strand → call urbi_watcher_check_invariants
 *      while body_strand is live.  Must not abort.
 *   2. watcher_invariant_holds_after_body_completes:
 *      After running dispatcher to quiescent (body_strand → NULL),
 *      call urbi_watcher_check_invariants again.  Must not abort. */

#include "utest.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "module/umodule.h"
#include "runtime/uclosure.h"
#include "runtime/uframe.h"
#include "watcher/uwatcher.h"
#include "urbi/urbi.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers
 * =================================================================== */

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

static UWatcher *
install_body_watcher(struct UVM *vm, struct URealm *realm, UClosure *body_cl)
{
    UWatcher *w = urbi_watcher_install_internal(
        vm, UWATCHER_AT,
        realm->tag,   /* owning_tag == realm->tag → no extra ambient attach */
        NULL,         /* condition */
        body_cl,      /* body */
        NULL,         /* onleave */
        NULL, 0U);
    if (w)
        w->realm = realm;
    return w;
}

/* Drive the VM until no runnable strands remain or limit reached. */
#define GC_INV_MAX_ITERS 1000

static int
run_until_no_runnable(struct UVM *vm)
{
    int i;
    for (i = 0; i < GC_INV_MAX_ITERS; i++) {
        UStepResult sr = urbi_step(vm, 64, NULL);
        if (sr == URBI_STEP_FATAL) return -1;
        if (vm->strand_runnable_count == 0) return 1;
    }
    return 0;
}

/* ===================================================================
 * Test cases (URBI_DEBUG only)
 * =================================================================== */

#ifdef URBI_DEBUG

/* 1. watcher_invariant_holds_during_body_run
 *
 * With a body strand live (body_strand != NULL, back-pointer set, strand on
 * realm->strands_head), urbi_watcher_check_invariants must complete without
 * aborting. */
UTEST(watcher_invariant_holds_during_body_run)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;

    uvm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_ret_closure(&body_cl, &proto, instr);

    UWatcher *w = install_body_watcher(&vm, r, &body_cl);
    UASSERT(w != NULL);
    UASSERT(w->body_strand == NULL);

    vm.in_watcher_eval = 1;
    do_spawn_body_coroutine(&vm, w, NULL);
    vm.in_watcher_eval = 0;

    /* Body strand must be live with back-pointer set. */
    UASSERT(w->body_strand != NULL);
    UASSERT(w->body_strand->watcher_body_owner == w);

    /* Invariant check must pass without aborting. */
    urbi_watcher_check_invariants(&vm);

    /* Clean up — run dispatcher first so the body strand completes cleanly. */
    run_until_no_runnable(&vm);

    urbi_watcher_unregister_internal(&vm, w);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 2. watcher_invariant_holds_after_body_completes
 *
 * After the body strand has run to completion (body_strand → NULL),
 * urbi_watcher_check_invariants must also pass (no live body_strand to walk). */
UTEST(watcher_invariant_holds_after_body_completes)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;

    uvm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_ret_closure(&body_cl, &proto, instr);

    UWatcher *w = install_body_watcher(&vm, r, &body_cl);
    UASSERT(w != NULL);

    vm.in_watcher_eval = 1;
    do_spawn_body_coroutine(&vm, w, NULL);
    vm.in_watcher_eval = 0;

    UASSERT(w->body_strand != NULL);

    /* Run until body finishes — urbi_watcher_body_completed clears body_strand. */
    int rc = run_until_no_runnable(&vm);
    UASSERT_EQ(rc, 1);
    UASSERT(w->body_strand == NULL);

    /* Invariant check on idle watcher (no body_strand) must pass. */
    urbi_watcher_check_invariants(&vm);

    urbi_watcher_unregister_internal(&vm, w);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

#endif /* URBI_DEBUG */

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_watcher_gc_invariants_suite(void)
{
    printf("test_watcher_gc_invariants\n");
#ifdef URBI_DEBUG
    utest_run("watcher_invariant_holds_during_body_run",
              watcher_invariant_holds_during_body_run);
    utest_run("watcher_invariant_holds_after_body_completes",
              watcher_invariant_holds_after_body_completes);
#endif
}
