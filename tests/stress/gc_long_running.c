/* SPDX-License-Identifier: BSD-3-Clause */
/* Stress test: GC long-running alloc-drop loop.
 *
 * Allocates 10000 cells in a loop, triggering incremental GC slices every
 * 1000 iterations.  After the loop, forces a full collection and verifies
 * that gc_live_bytes is negligible relative to gc_total_allocated — i.e.,
 * that the GC reclaims dead cells and the heap does not grow without bound.
 *
 * Row 10 acceptance #4 (stress tests pass with bounded heap).
 * T28.  Lives in tests/stress/ — NOT wired into `make test` (slow path);
 * invoked by `make test-stress` and `make releasetest`. */

#include "ugc_capi.h"
#include "uvm.h"
#include <stdio.h>
#include <stdlib.h>

#define LOOP_COUNT        10000
#define SLICE_INTERVAL    1000

int main(void)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    for (int i = 0; i < LOOP_COUNT; i++) {
        /* Allocate a small cell and immediately drop the reference.
         * The GC is responsible for collecting unreachable cells. */
        UCell *c = urbi_gc_alloc(&vm, sizeof(UCell) + 32u, UTYPE_OBJECT);
        if (c == NULL) {
            fprintf(stderr, "FAIL: urbi_gc_alloc returned NULL at i=%d\n", i);
            uvm_destroy(&vm);
            return 1;
        }
        (void)c;  /* drop reference — cell is now unreachable */

        if (i % SLICE_INTERVAL == 0) {
            urbi_gc_slice(&vm, URBI_GC_SLICE_BUDGET);
        }
    }

    /* Force a full collection so all dead cells are swept. */
    urbi_gc_force_full(&vm);

    size_t live  = urbi_gc_live_bytes(&vm);
    size_t total = urbi_gc_bytes_allocated(&vm);

    /* After a full collection with no live references, live_bytes should be
     * zero (all cells unreachable after force_full). */
    int pass = (total == 0 || live == 0);

    printf("gc_long_running: total_allocated=%zu live_bytes=%zu %s\n",
           total, live, pass ? "PASS" : "FAIL");

    if (!pass) {
        fprintf(stderr, "FAIL: live_bytes (%zu) not zero after force_full"
                " — heap not fully reclaimed\n", live);
    }

    uvm_destroy(&vm);
    return pass ? 0 : 1;
}
