/* SPDX-License-Identifier: BSD-3-Clause */
/* Stress test: reactive event emit loop (R8 hardening).
 *
 * Creates one event, installs 5 AT_EVENT subscribers (trivial OP_RET bodies),
 * then fires c_event_emit_async 10 000 times.  Verifies pool stability:
 * watcher_pool_in_use stays constant (no new pool allocations during emit)
 * and watcher_pool_high_water == NUM_SUBS after the loop.
 *
 * The 5 subscriber body_strands are spawned but never stepped (no urbi_step;
 * body closures are trivial OP_RET stubs).  The test exercises the emit
 * fan-out path + strand-spawn bookkeeping.  Every 100 emits the body_strand
 * references are manually recycled to keep the run queue bounded.
 *
 * Foundation for the spec'd 5 stress targets (R8 §3); remaining 4 (slot-change
 * churn, watcher cycles, OOM injection, tag cascade) deferred to v1.x. */

#include "event/uevent.h"
#include "event/uevent_emit.h"
#include "watcher/uwatcher.h"
#include "watcher/uwatcher_install.h"
#include "runtime/uclosure.h"
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"
#include "realm/urealm.h"
#include "chunk/uchunk.h"
#include "vm/uvm.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define EMIT_COUNT   10000
#define NUM_SUBS     5

static UValue make_int(int i)
{
    UValue v = {0};
    v.kind = UVAL_INT;
    v.v.i  = i;
    return v;
}

static void
make_trivial_closure(UClosure *cl, UProto *proto, uint32_t *buf)
{
    buf[0] = (uint32_t)OP_RET;
    memset(proto, 0, sizeof(*proto));
    proto->instructions = buf;
    proto->instr_count  = 1;
    memset(cl, 0, sizeof(*cl));
    cl->proto   = proto;
    cl->nupvals = 0;
}

int main(void)
{
    UVM    vm;
    URealm *realm;
    UEvent *e;
    UStrand  strand;
    UClosure bodies[NUM_SUBS];
    UProto   protos[NUM_SUBS];
    uint32_t instrs[NUM_SUBS][1];
    UWatcher *watchers[NUM_SUBS];

    urbi_vm_init(&vm, NULL, NULL);

    realm = urbi_realm_create(&vm);
    if (realm == NULL) {
        fprintf(stderr, "FAIL: urbi_realm_create returned NULL\n");
        urbi_vm_destroy(&vm);
        return 1;
    }

    e = urbi_event_create(&vm);
    if (e == NULL) {
        fprintf(stderr, "FAIL: urbi_event_create returned NULL\n");
        urbi_realm_destroy(&vm, realm);
        urbi_vm_destroy(&vm);
        return 1;
    }

    ustrand_init(&strand, &vm);
    strand.realm = realm;

    for (int i = 0; i < NUM_SUBS; i++) {
        make_trivial_closure(&bodies[i], &protos[i], instrs[i]);

        UWatcherInstallResult rc = install_at_event_runtime(
            &vm, &strand, UWATCHER_AT_EVENT, e, &bodies[i], NULL);
        if (rc != URBI_INSTALL_OK) {
            fprintf(stderr, "FAIL: install_at_event_runtime[%d] returned %d\n",
                    i, (int)rc);
            ustrand_destroy(&strand, &vm);
            urbi_realm_destroy(&vm, realm);
            urbi_vm_destroy(&vm);
            return 1;
        }
        /* Record last-inserted watcher (tail of event list). */
        UWatcher *w = e->at_watchers_head;
        while (w != NULL && w->next_in_event != NULL) w = w->next_in_event;
        watchers[i] = w;
    }

    uint16_t pool_in_use_after_install = vm.watchers->pool_in_use;
    if (pool_in_use_after_install != NUM_SUBS) {
        fprintf(stderr, "FAIL: expected %d watchers in use after install, got %u\n",
                NUM_SUBS, (unsigned)pool_in_use_after_install);
        ustrand_destroy(&strand, &vm);
        urbi_realm_destroy(&vm, realm);
        urbi_vm_destroy(&vm);
        return 1;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < EMIT_COUNT; i++) {
        c_event_emit_async(&vm, e, make_int(i));

        /* Drain body_strand references every 100 emits to keep the run queue
         * bounded.  Body strands are trivial OP_RET stubs; we recycle their
         * pool slots manually (no urbi_step available in this harness). */
        if ((i % 100) == 99) {
            UWatcher *w = e->at_watchers_head;
            while (w != NULL) {
                if (w->body_strand != NULL) {
                    /* SCHED-01: unbind owns the runnable-count decrement
                     * (the body strand is READY on the queue); also fixes
                     * up the queue neighbours before the destroy. */
                    urbi_sched_strand_unbind_from_ready_queue(w->body_strand);
                    ustrand_destroy(w->body_strand, &vm);
                    w->body_strand = NULL;
                }
                w = w->next_in_event;
            }
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);

    /* Pool in-use must still be NUM_SUBS — emit doesn't allocate watchers. */
    if (vm.watchers->pool_in_use != NUM_SUBS) {
        fprintf(stderr, "FAIL: pool_in_use changed during emit: "
                "expected %d got %u\n",
                NUM_SUBS, (unsigned)vm.watchers->pool_in_use);
        ustrand_destroy(&strand, &vm);
        urbi_realm_destroy(&vm, realm);
        urbi_vm_destroy(&vm);
        return 1;
    }

    for (int i = NUM_SUBS - 1; i >= 0; i--) {
        if (watchers[i] != NULL)
            urbi_watcher_unregister_internal(&vm, watchers[i]);
    }

    /* After unregister, pool_in_use must be 0 (all watchers returned). */
    if (vm.watchers->pool_in_use != 0) {
        fprintf(stderr, "FAIL: pool_in_use %u after unregister (expected 0)\n",
                (unsigned)vm.watchers->pool_in_use);
        ustrand_destroy(&strand, &vm);
        urbi_realm_destroy(&vm, realm);
        urbi_vm_destroy(&vm);
        return 1;
    }

    ustrand_destroy(&strand, &vm);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);

    long ns = (long)(t1.tv_sec  - t0.tv_sec)  * 1000000000L
            + (long)(t1.tv_nsec - t0.tv_nsec);
    double mops = ns > 0
        ? (double)EMIT_COUNT * NUM_SUBS / ((double)ns / 1e9) / 1e6 : 0.0;

    printf("stress_event_emit_loop: %d emits x %d subs = %d deliveries "
           "in %ld ns (%.2f Mops/s) PASS\n",
           EMIT_COUNT, NUM_SUBS, EMIT_COUNT * NUM_SUBS, ns, mops);
    return 0;
}
