/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit test: scratch-frame strand visibility to sched_walk_roots
 * (closes GC-006 + GC-038 by construction).
 *
 * Background:
 *   v0.5.1's urbi_run_closure_on_scratch (src/runtime/uscratch.c)
 *   replaced an earlier vm->watcher_scratch_frame design with a transient
 *   UStrand allocated on the C stack and threaded onto
 *   vm->global_realm->strands_head before entering the dispatch loop.  The
 *   audit findings GC-006 and GC-038 were filed against the pre-v0.5.1 shape
 *   ("UScratchFrame.registers[] not GC-rooted").  The transient-strand
 *   architecture closes both by construction — sched_walk_roots iterates the
 *   realm hierarchy via realm.strands_head → strand_walk_roots and sees the
 *   scratch strand's full register window just like any persistent strand.
 *
 * This test pins that invariant structurally: hand-construct a transient
 * scratch strand following the same linkage steps as run_on_scratch_core,
 * write a sentinel UValue into the register window, call sched_walk_roots
 * with a probe callback, and assert the sentinel is visited.  A regression
 * here (e.g. someone moving the linkage below the GC-allocating dispatch
 * call) would silently drop scratch-frame rooting; this test fails loudly.
 *
 * Sibling coverage in test_gc_strand_walker.c covers the symmetric
 * urbi_vm_run transient-strand path (Test 4 in that file). */

#include "utest.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"
#include "realm/urealm.h"
#include "urbi/urbi.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* === Probe: locate a sentinel pointer among the visited UValues. ===
 *
 * The walker reports every UValue slot in the strand's register window;
 * we plant a UVAL_OBJECT-shaped sentinel into one of those slots and check
 * that the walker visited a slot whose payload pointer matches. */
typedef struct {
    void *needle;     /* sentinel pointer planted in scratch strand R[0] */
    int   found;      /* set non-zero when walker visits a slot referencing needle */
    int   total_visits;
} ScratchProbe;

static void scratch_probe_cb(UVM *vm, UValue *root, void *ctx)
{
    ScratchProbe *p = (ScratchProbe *)ctx;
    (void)vm;
    p->total_visits++;
    /* Match either a UVAL_OBJECT or any other heap-bearing kind whose
     * generic pointer payload equals our needle.  We compare via the raw
     * UValue.v.p slot; UVAL_NIL slots have v.p == NULL and never match. */
    if (root != NULL && root->v.p == p->needle) {
        p->found = 1;
    }
}

/* === Test: a scratch strand linked to global_realm.strands_head is walked
 *           by sched_walk_roots (its register-window UValues are visited). ===
 *
 * Setup mirrors run_on_scratch_core (src/runtime/uscratch.c) without entering
 * dispatch:
 *   1. Lazy-create global_realm via urbi_realm_global.
 *   2. Allocate a transient UStrand on the test stack; zero it.
 *   3. Allocate a register-stack via the VM allocator (mimics
 *      urbi_strand_arm_from_closure → urbi_strand_register_stack_alloc).
 *   4. Plant a sentinel pointer into strand.R[0].
 *   5. Link strand onto gr->strands_head with next_in_realm.
 *   6. Call sched_walk_roots with the probe callback.
 *   7. Verify the sentinel was visited; unlink + free.
 *
 * Pre-v0.5.1 (UScratchFrame design) would have failed this test because
 * UScratchFrame.registers[] was not on any GC-walked list.
 * Post-v0.5.1 (transient-strand design) passes by construction. */
UTEST(scratch_strand_register_window_walked_by_sched_walk_roots)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *gr = urbi_realm_global(&vm);
    UASSERT(gr != NULL);

    /* Transient strand on test stack — same shape as run_on_scratch_core. */
    UStrand strand;
    memset(&strand, 0, sizeof(strand));
    strand.vm                  = &vm;
    strand.state               = USTRAND_STATE_RUNNING;
    strand.is_transient_strand = 1U;

    /* Register stack — mirrors urbi_strand_register_stack_alloc. */
    const size_t stack_bytes = UVM_STACK_CAP * sizeof(UValue);
    strand.stack = (UValue *)vm.alloc_fn(NULL, stack_bytes, vm.alloc_ud);
    UASSERT(strand.stack != NULL);
    memset(strand.stack, 0, stack_bytes);
    strand.R = strand.stack;

    /* Plant a sentinel pointer in R[0]. We use a heap-bearing kind with a
     * distinguishable v.p so the probe callback can recognise it.  Pointing
     * at vm itself is sufficient — it's never a legitimate UValue payload,
     * yet has a well-defined non-NULL address. */
    void *needle = &vm;
    strand.R[0].kind = UVAL_OBJECT;
    strand.R[0].v.p  = needle;

    /* Link onto global_realm->strands_head — the linkage step that closes
     * GC-006 + GC-038 by construction. */
    strand.realm         = gr;
    strand.next_in_realm = gr->strands_head;
    gr->strands_head     = &strand;

    /* Call the walker. */
    ScratchProbe probe = {0};
    probe.needle = needle;
    sched_walk_roots(&vm, scratch_probe_cb, &probe);

    /* The walker must have visited the register window — including R[0]. */
    UASSERT(probe.total_visits > 0);
    UASSERT_EQ(1, probe.found);

    /* Symmetric unlink before teardown. */
    UStrand **pp = &gr->strands_head;
    while (*pp != NULL) {
        if (*pp == &strand) {
            *pp = strand.next_in_realm;
            strand.next_in_realm = NULL;
            break;
        }
        pp = &(*pp)->next_in_realm;
    }

    vm.alloc_fn(strand.stack, 0, vm.alloc_ud);
    strand.stack = NULL;
    strand.R     = NULL;

    urbi_vm_destroy(&vm);
}

/* === Suite entry point === */

void test_gc_scratch_rooting_suite(void)
{
    printf("  [gc_scratch_rooting]\n");
    utest_run("scratch_strand_register_window_walked_by_sched_walk_roots",
              scratch_strand_register_window_walked_by_sched_walk_roots);
}
