/* SPDX-License-Identifier: BSD-3-Clause */
/* src/runtime/uperf.c — perf-counter reset (v0.11.1). */
#include "runtime/uperf.h"
#include "vm/uvm.h"

#if URBI_PERF_COUNTERS
void urbi_perf_reset(struct UVM *vm)
{
    uint32_t next_epoch;
    if (!vm) return;
    next_epoch = vm->perf.epoch + 1u;   /* preserve epoch monotonicity across resets */
    {
        UPerfCounters zero = {0};
        vm->perf = zero;
    }
    vm->perf.epoch = next_epoch;
}
#endif
