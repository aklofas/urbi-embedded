/* SPDX-License-Identifier: BSD-3-Clause */
/* Integration tests: dispatcher wires urbi_watcher_body_completed into the
 * strand-DEAD path (spec #1 §6.1) and drain defers for body-alive watchers
 * (spec #1 §6.3).  Task T28.
 *
 * Cases:
 *   1. watcher_body_completion_clears_back_pointer_before_destroy:
 *      Spawn a watcher body strand, run urbi_step to quiescent, verify
 *      w->body_strand == NULL — i.e. the dispatcher called
 *      urbi_watcher_body_completed before the strand was freed.
 *
 *   2. unregister_while_body_alive_defers_drain:
 *      Spawn body strand, push watcher to onleave queue while body is READY
 *      (body_strand != NULL), drain — drain must skip (defer) the entry.
 *      Run urbi_step to quiescent so body completes (body_strand → NULL).
 *      Drain again — queue must be empty afterwards. */

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

/* Build a trivial closure backed by a single OP_RET instruction.
 * Storage for proto, closure, and instruction word is caller-provided. */
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

/* Install a minimal AT watcher with a real body closure in realm.
 * owning_tag is set to realm->tag so the ambient-attach step in
 * do_spawn_body_coroutine is skipped (the realm tag is already attached
 * by urbi_strand_create). */
static UWatcher *
install_body_watcher(struct UVM *vm, struct URealm *realm, UClosure *body_cl)
{
    UWatcher *w = urbi_watcher_install_internal(
        vm, UWATCHER_AT,
        realm->tag,   /* owning_tag == realm->tag */
        NULL,         /* condition: NULL (no condition closure) */
        body_cl,      /* body */
        NULL,         /* onleave */
        NULL, 0U);    /* read_set: empty */
    if (w)
        w->realm = realm;
    return w;
}

/* Check whether w is currently in vm->pending_onleave_head chain.
 * Threading pointer while on pending queue is next_active. */
static int
pending_queue_contains(struct UVM *vm, struct UWatcher *w)
{
    struct UWatcher *cur = vm->pending_onleave_head;
    while (cur != NULL) {
        if (cur == w) return 1;
        cur = cur->next_active;
    }
    return 0;
}

/* Drive the VM via urbi_step until no strands are runnable, or until a
 * fatal error or step limit.  Returns 1 when strand_runnable_count reaches
 * zero, 0 on timeout, -1 on fatal.
 *
 * We cannot wait for URBI_STEP_QUIESCENT because installed watchers keep
 * watcher_active_count > 0 which prevents quiescence even when all body
 * strands have completed.  Checking strand_runnable_count == 0 is sufficient
 * for these tests: once the body strand (the only runnable strand) has run to
 * DEAD, strand_runnable_count drops to 0 and urbi_step stops dispatching. */
#define LIFECYCLE_MAX_ITERS 1000

static int
run_until_no_runnable(struct UVM *vm)
{
    int i;
    for (i = 0; i < LIFECYCLE_MAX_ITERS; i++) {
        UStepResult sr = urbi_step(vm, 64, NULL);
        if (sr == URBI_STEP_FATAL)   return -1;
        if (sr == URBI_STEP_WAKE_AT) return 1; /* no sleepers here */
        /* URBI_STEP_QUIESCENT or URBI_STEP_RUNNING with no runnable strands. */
        if (vm->strand_runnable_count == 0) return 1;
    }
    return 0; /* timeout */
}

/* ===================================================================
 * Case 1: watcher_body_completion_clears_back_pointer_before_destroy
 *
 * After the body strand runs to completion through the real dispatcher,
 * w->body_strand must be NULL — confirming that the exit_strand path in
 * dispatch_loop_until_yield called urbi_watcher_body_completed.
 * =================================================================== */
UTEST(watcher_body_completion_clears_back_pointer_before_destroy)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;

    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_ret_closure(&body_cl, &proto, instr);

    UWatcher *w = install_body_watcher(&vm, r, &body_cl);
    UASSERT(w != NULL);
    UASSERT(w->body_strand == NULL);

    /* Spawn body strand (simulates watcher_eval_dirty firing the watcher). */
    vm.in_watcher_eval = 1;
    do_spawn_body_coroutine(&vm, w, NULL);
    vm.in_watcher_eval = 0;

    /* Body strand must have been created and enqueued. */
    UASSERT(w->body_strand != NULL);

    /* Run scheduler until quiescent.  The body strand executes OP_RET,
     * exits via exit_strand:, and the new §6.1 hook calls
     * urbi_watcher_body_completed which clears both pointers. */
    int rc = run_until_no_runnable(&vm);
    UASSERT_EQ(rc, 1); /* reached quiescent */

    /* Key assertion: back-pointer cleared by the dispatcher. */
    UASSERT(w->body_strand == NULL);

    /* Cleanup. */
    urbi_watcher_unregister_internal(&vm, w);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 2: unregister_while_body_alive_defers_drain
 *
 * With the body strand alive (READY / RUNNING), drain must skip the
 * watcher and leave it on the queue.  Once the body completes the
 * next drain must process the entry (queue becomes empty).
 * =================================================================== */
UTEST(unregister_while_body_alive_defers_drain)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;

    urbi_vm_init(&vm, NULL, NULL);
    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_ret_closure(&body_cl, &proto, instr);

    UWatcher *w = install_body_watcher(&vm, r, &body_cl);
    UASSERT(w != NULL);

    /* Spawn body strand (body_strand != NULL, state READY). */
    vm.in_watcher_eval = 1;
    do_spawn_body_coroutine(&vm, w, NULL);
    vm.in_watcher_eval = 0;
    UASSERT(w->body_strand != NULL);

    /* Push watcher onto the onleave queue while body is still alive.
     * pending_onleave_queue_push sets URBI_WATCHER_PENDING_UNREGISTER,
     * removes w from active_watchers_head and from the tag member list,
     * and appends w to the pending_onleave_queue FIFO. */
    pending_onleave_queue_push(&vm, w);

    /* drain_pending_onleave_queue must defer w because body_strand != NULL. */
    drain_pending_onleave_queue(&vm);

    /* Watcher must still be on the queue (drain deferred). */
    UASSERT(pending_queue_contains(&vm, w));
    /* PENDING_UNREGISTER must be set (set by push, not cleared by defer). */
    UASSERT((w->flags & URBI_WATCHER_PENDING_UNREGISTER) != 0);

    /* Dispatch until quiescent: body strand executes OP_RET.
     * exit_strand calls urbi_watcher_body_completed which:
     *   - Sees PENDING_UNREGISTER → clears PENDING_REFIRE, returns.
     *   - Clears body_strand and watcher_body_owner.
     * drain is NOT automatically invoked by the dispatcher after exit_strand
     * (safepoints run before exit_strand, not after), so w remains on the
     * queue until the next explicit drain call. */
    int rc = run_until_no_runnable(&vm);
    UASSERT_EQ(rc, 1);

    /* Body is done; body_strand must be NULL. */
    UASSERT(w->body_strand == NULL);

    /* Queue must still have w on it (auto-safepoint drain at step boundaries
     * ran before body completed; no drain ran after body_strand was cleared
     * within the same step call). */
    UASSERT(pending_queue_contains(&vm, w));

    /* Explicit drain: w->body_strand == NULL now, so drain processes it.
     * urbi_watcher_unregister_internal frees w back to the pool.
     * Do NOT access w after this call. */
    drain_pending_onleave_queue(&vm);

    /* Queue must be empty. */
    UASSERT(vm.pending_onleave_head == NULL);
    UASSERT(vm.pending_onleave_tail == NULL);

    /* watcher_active_count must be 0 (unregister_internal decremented it). */
    UASSERT_EQ((int)vm.watcher_active_count, 0);

    /* Realm destroy: frees all realm-managed strands. */
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_watcher_lifecycle_suite(void)
{
    printf("test_watcher_lifecycle\n");
    utest_run("watcher_body_completion_clears_back_pointer_before_destroy",
              watcher_body_completion_clears_back_pointer_before_destroy);
    utest_run("unregister_while_body_alive_defers_drain",
              unregister_while_body_alive_defers_drain);
}
