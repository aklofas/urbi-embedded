/* SPDX-License-Identifier: BSD-3-Clause */
/* Stress test: GC per-slice pause time instrumentation.
 *
 * Allocates cells to push the GC through several incremental cycles, timing
 * each urbi_gc_slice() call with clock_gettime(CLOCK_MONOTONIC).  Prints a
 * one-line summary of max observed slice duration.
 *
 * At M3, the acceptance criterion is "runs without crash and produces a
 * summary".  The sub-millisecond pause target (Row 10 §3.7) is formally
 * verified by T46 (make test-gc-pause).
 *
 * Row 10 acceptance #4 + #8 (stress tests run; T46 owns the < 1 ms gate).
 * T28. */

#include "urbi/gc.h"
#include "vm/uvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define ALLOC_COUNT    5000
#define SLICE_INTERVAL  100

/* T46 will tighten this to 1000000 (1 ms) per row 10 §3.7 sub-ms target.
 * At M3 the assertion is a no-op (GC_PAUSE_ASSERT_NS=0); flip the constant at T46. */
#ifndef GC_PAUSE_ASSERT_NS
#define GC_PAUSE_ASSERT_NS  0
#endif

static long
elapsed_ns(struct timespec *start, struct timespec *end)
{
    return (long)(end->tv_sec  - start->tv_sec)  * 1000000000L
         + (long)(end->tv_nsec - start->tv_nsec);
}

int main(void)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    long max_ns   = 0;
    long total_ns = 0;
    int  slices   = 0;

    for (int i = 0; i < ALLOC_COUNT; i++) {
        UCell *c = urbi_gc_alloc(&vm, sizeof(UCell) + 64U, UTYPE_OBJECT);
        if (c == NULL) {
            fprintf(stderr, "FAIL: urbi_gc_alloc returned NULL at i=%d\n", i);
            urbi_vm_destroy(&vm);
            return 1;
        }
        (void)c;

        if (i % SLICE_INTERVAL == 0) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            urbi_gc_slice(&vm, URBI_GC_SLICE_BUDGET);
            clock_gettime(CLOCK_MONOTONIC, &t1);

            long ns = elapsed_ns(&t0, &t1);
            if (ns > max_ns) max_ns = ns;
            total_ns += ns;
            slices++;
        }
    }

    /* Final forced collection — also timed. */
    {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        urbi_gc_force_full(&vm);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ns = elapsed_ns(&t0, &t1);
        printf("gc_pause_time: force_full took %ld ns\n", ns);
    }

    long avg_ns = slices > 0 ? total_ns / slices : 0;
    printf("gc_pause_time: %d slices, max=%ld ns, avg=%ld ns PASS\n",
           slices, max_ns, avg_ns);
    printf("  (sub-ms verification: T46 / make test-gc-pause)\n");

#if GC_PAUSE_ASSERT_NS > 0
    if (max_ns > GC_PAUSE_ASSERT_NS) {
        printf("FAIL — max slice %ld ns exceeds GC_PAUSE_ASSERT_NS=%d\n",
               max_ns, GC_PAUSE_ASSERT_NS);
        urbi_vm_destroy(&vm);
        return 1;
    }
#endif

    urbi_vm_destroy(&vm);
    return 0;
}
