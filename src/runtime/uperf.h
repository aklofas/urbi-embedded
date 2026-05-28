/* SPDX-License-Identifier: BSD-3-Clause */
/* src/runtime/uperf.h — VM-domain performance counters (v0.11.1).
 *
 * Gated by URBI_PERF_COUNTERS: undefined => URBI_PERF_INC is (void)0 and the
 * UPerfCounters field is absent from struct UVM (zero hot-path cost, zero UVM
 * delta).  Counters are EXCLUDED from urbi_get_determinism_checksum — never
 * add them there.  Surfaced via Debug.profile(); internal (no public C API). */
#ifndef URBI_PERF_H
#define URBI_PERF_H

#include <stddef.h>
#include <stdint.h>

#ifndef URBI_PERF_COUNTERS
#  define URBI_PERF_COUNTERS 0
#endif

struct UVM;

#if URBI_PERF_COUNTERS

typedef struct {
    uint32_t epoch;          /* bumped on reset so tools can distinguish snapshots */
    size_t   opcodes;        /* opcodes retired */
    size_t   calls;          /* OP_CALL dispatched */
    size_t   returns;        /* OP_RET dispatched */
    size_t   slot_get;       /* OP_GETSLOT */
    size_t   slot_set;       /* OP_SETSLOT */
    size_t   ic_hit;         /* inline-cache hits */
    size_t   ic_miss;        /* inline-cache misses */
    size_t   native_calls;   /* host-fn invocations */
    size_t   ctx_switches;   /* strand dispatches */
    size_t   yields;         /* strand yields */
    size_t   blocks;         /* strand blocks */
    size_t   watcher_installs;
    size_t   watcher_fires;
    size_t   event_emits;
} UPerfCounters;

#define URBI_PERF_INC(vm, field)      ((void)((vm)->perf.field++))
#define URBI_PERF_ADD(vm, field, n)   ((void)((vm)->perf.field += (size_t)(n)))

void urbi_perf_reset(struct UVM *vm);   /* zero all counters, bump epoch */

#else  /* URBI_PERF_COUNTERS == 0 */

#define URBI_PERF_INC(vm, field)      ((void)0)
#define URBI_PERF_ADD(vm, field, n)   ((void)0)

#endif /* URBI_PERF_COUNTERS */

#endif /* URBI_PERF_H */
