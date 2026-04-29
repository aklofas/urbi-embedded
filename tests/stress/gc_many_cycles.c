/* SPDX-License-Identifier: BSD-3-Clause */
/* Stress test: GC many full-collection cycles without crash.
 *
 * Calls urbi_gc_force_full() 100 times in a row with no live cells.
 * Verifies that:
 *   (a) No crash or assertion failure occurs.
 *   (b) The GC ends in GC_PHASE_IDLE after each forced full cycle.
 *
 * Row 10 acceptance #4 (stress tests pass; GC phase returns to IDLE).
 * T28. */

#include "urbi/gc.h"
#include "uvm.h"
#include <stdio.h>
#include <stdlib.h>

#define CYCLE_COUNT 100

int main(void)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    for (int i = 0; i < CYCLE_COUNT; i++) {
        urbi_gc_force_full(&vm);

        uint8_t phase = urbi_gc_phase(&vm);
        if (phase != GC_PHASE_IDLE) {
            fprintf(stderr,
                    "FAIL: gc_phase != GC_PHASE_IDLE after force_full"
                    " (cycle %d, phase=%u)\n",
                    i, (unsigned)phase);
            uvm_destroy(&vm);
            return 1;
        }
    }

    printf("gc_many_cycles: %d forced full cycles completed PASS\n", CYCLE_COUNT);
    uvm_destroy(&vm);
    return 0;
}
