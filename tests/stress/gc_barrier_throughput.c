/* SPDX-License-Identifier: BSD-3-Clause */
/* Stress test: GC write-barrier throughput.
 *
 * Calls urbi_gc_slot_write() 100 000 times using synthetic parent/child cells
 * allocated via urbi_gc_alloc (test-only pattern — same as test_ugc_barrier.c).
 * Times the loop with clock_gettime(CLOCK_MONOTONIC) and prints ops/sec.
 *
 * At M3, the acceptance criterion is "runs without crash and prints a summary".
 * The throughput number is diagnostic; no hard gate is set here.
 *
 * Row 10 acceptance #4 + #8.  T28. */

#include "urbi/gc.h"
#include "uvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define BARRIER_CALLS   100000
#define POOL_SIZE       64      /* reuse a small pool of cells to keep RSS bounded */

/* Build a UVAL_CLOSURE-tagged UValue from a UCell* (test-only, mirrors
 * test_ugc_barrier.c helper). */
static UValue
uvalue_from_cell(UCell *c)
{
    UValue v = {0};
    v.kind = UVAL_CLOSURE;
    v.v.p  = (void *)c;
    return v;
}

int main(void)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* Allocate a small pool of reusable cells. */
    UCell *pool[POOL_SIZE];
    for (int i = 0; i < POOL_SIZE; i++) {
        pool[i] = urbi_gc_alloc(&vm, sizeof(UCell) + 16u, UTYPE_OBJECT);
        if (pool[i] == NULL) {
            fprintf(stderr, "FAIL: urbi_gc_alloc returned NULL (pool i=%d)\n", i);
            uvm_destroy(&vm);
            return 1;
        }
    }

    /* Force parent cells black so the barrier hot-path fires on white children. */
    for (int i = 0; i < POOL_SIZE; i++) {
        pool[i]->gc_byte = (uint8_t)((pool[i]->gc_byte & ~UGC_COLOR_MASK)
                                     | UGC_COLOR_BLACK);
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < BARRIER_CALLS; i++) {
        UCell *parent = pool[i % POOL_SIZE];
        UCell *child  = pool[(i + 1) % POOL_SIZE];

        /* Reset child to white so the barrier condition fires. */
        child->gc_byte = (uint8_t)((child->gc_byte & ~UGC_COLOR_MASK)
                                   | vm.current_white);

        urbi_gc_slot_write(&vm, parent, (uint32_t)(i % 8), uvalue_from_cell(child));

        /* Reset child back to black so the next iteration's parent slot write
         * hits the barrier condition again (keep exercising the shade path). */
        child->gc_byte = (uint8_t)((child->gc_byte & ~UGC_COLOR_MASK)
                                   | UGC_COLOR_BLACK);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);

    long ns = (long)(t1.tv_sec  - t0.tv_sec)  * 1000000000L
            + (long)(t1.tv_nsec - t0.tv_nsec);
    double ops_per_sec = ns > 0
        ? (double)BARRIER_CALLS / ((double)ns / 1e9)
        : 0.0;

    printf("gc_barrier_throughput: %d calls in %ld ns (%.0f ops/sec) PASS\n",
           BARRIER_CALLS, ns, ops_per_sec);

    uvm_destroy(&vm);
    return 0;
}
