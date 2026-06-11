/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_vm_create_oom.c — refactor-3 VM-09: urbi_vm_create must
 * reclaim a partially-initialized VM on init failure.
 *
 * urbi_vm_init latches OOM (oom_seen) and keeps initializing, so it can
 * return URBI_ERR_OOM with subsystems already live (event ring, watcher
 * pool, ...).  urbi_vm_create must run urbi_vm_destroy before releasing
 * the storage, or every subsystem allocated before the failing point
 * leaks.  The sweep below fails the allocator at every prefix N and
 * checks the net outstanding-block balance returns to zero each time. */

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* Counting allocator: realloc-style (ptr, nbytes, ud) per UVMAllocFn.
 * Fails every allocation after `allocs_left` successes; tracks the net
 * number of outstanding blocks.  Growth of an existing block (ptr != NULL,
 * nbytes > 0) does not change the balance — it is the same block. */
typedef struct {
    int allocs_left;   /* successful allocations remaining before hard OOM */
    int outstanding;   /* net live blocks handed out by this allocator */
} OomCtl;

static void *oom_alloc(void *ptr, size_t nbytes, void *ud)
{
    OomCtl *c = (OomCtl *)ud;
    if (nbytes == 0) {
        if (ptr != NULL) {
            free(ptr);
            c->outstanding--;
        }
        return NULL;
    }
    if (c->allocs_left == 0) return NULL;   /* OOM: old block (if any) survives */
    c->allocs_left--;
    {
        void *p = realloc(ptr, nbytes);
        if (p != NULL && ptr == NULL) c->outstanding++;
        return p;
    }
}

UTEST(vm_create_oom_path_leaks_nothing) {
    /* Sweep the failure point across init: for each N, creation either
     * fails (balance must be zero — nothing may leak) or succeeds
     * (destroy it; balance must still be zero, sweep complete). */
    int n;
    for (n = 1; n < 60; n++) {
        OomCtl ctl;
        struct UVM *vm;
        ctl.allocs_left = n;
        ctl.outstanding = 0;
        vm = urbi_vm_create(oom_alloc, &ctl);
        if (vm != NULL) {
            urbi_vm_free(vm);
            UASSERT_EQ(ctl.outstanding, 0);
            return;  /* reached full success; sweep complete */
        }
        if (ctl.outstanding != 0) {
            printf("  (leak at N=%d: %d outstanding block(s))\n",
                   n, ctl.outstanding);
        }
        UASSERT_EQ(ctl.outstanding, 0);
    }
    UASSERT(0 && "urbi_vm_create never succeeded within the sweep");
}

void test_vm_create_oom_suite(void) {
    utest_run("vm_create_oom_path_leaks_nothing", vm_create_oom_path_leaks_nothing);
}
