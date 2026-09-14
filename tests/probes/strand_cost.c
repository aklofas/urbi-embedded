/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/probes/strand_cost.c — what an idle strand costs.
 *
 * The design target is under 1 KB for a strand parked on a sleep, which is
 * what makes "one strand per behaviour" affordable on a microcontroller: a
 * robot script with fifty concurrent behaviours should cost tens of
 * kilobytes, not megabytes.  A hundred sleepers under 100 KB is the same
 * statement, measured where the per-strand cost is large enough to read
 * clearly over the boot heap.
 *
 * The strands are made with __detach_strand rather than the `detach`
 * wrapper: the wrapper takes a lazy argument, and a lazy-taking function
 * called twice in one frame miscompiles today (a frontend defect filed for
 * Phase 4).  The primitive is what this probe is about anyway. */

#include "probe.h"

#define SLEEPERS      100u
#define SLEEPERS_CAP  (100u * 1024u)

int main(void)
{
    ProbeAlloc a = { 0, 0, 0 };
    UVM *vm = urbi_open(probe_alloc, &a, NULL);
    if (!vm) { fprintf(stderr, "strand_cost: urbi_open failed\n"); return 1; }

    /* Define the maker first, so its own chunk is not counted with the
     * strands it creates. */
    if (probe_run(vm, "var mk = function(n) { var i = 0;"
                      " while (i < n) { __detach_strand(function() { sleep(1000s) });"
                      " i = i + 1 } }") != URBI_OK) { urbi_close(vm); return 1; }
    urbi_gc_collect(vm);
    size_t before = a.live;

    if (probe_run(vm, "mk(100)") != URBI_OK) { urbi_close(vm); return 1; }
    urbi_gc_collect(vm);

    /* The VM must actually be holding a hundred parked strands: a probe
     * that measured nothing would otherwise pass. */
    if (!urbi_has_live_work(vm)) {
        fprintf(stderr, "strand_cost: nothing parked — the sleepers did not survive\n");
        urbi_close(vm);
        return 1;
    }

    size_t cost = a.live - before;
    printf("100 sleeping strands: %lu bytes (%lu each)\n",
           (unsigned long)cost, (unsigned long)(cost / SLEEPERS));
    printf("  cap:                %u bytes\n", SLEEPERS_CAP);

    int ok = cost < SLEEPERS_CAP;
    if (!ok)
        fprintf(stderr, "strand_cost: %lu bytes exceeds the %u byte cap\n",
                (unsigned long)cost, SLEEPERS_CAP);

    urbi_close(vm);
    if (a.live != 0) {
        fprintf(stderr, "strand_cost: %lu bytes still out on loan after close\n",
                (unsigned long)a.live);
        ok = 0;
    }
    return probe_verdict("strand_cost", ok);
}
