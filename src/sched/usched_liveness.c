/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi_vm_liveness — the ONE quiescence/liveness formula.
 *
 * Pre-fix the runtime carried three divergent quiescence definitions:
 * urbi_sched_quiescent AND'd five counters (including the armed watcher count and
 * a vestigial event_queue_count), urbi_step's post-loop returns used a
 * different subset, and urbi_vm_has_live_work read a third.  They are now
 * all thin views over this function.
 *
 * Owner decision 2026-06-11 (option a): `armed` (watchers of all modes +
 * SUSPENDED strands + WAITING strands) does NOT block QUIESCENT — it is
 * external-input work, reported but not runnable.  See the UVmLiveness
 * field docs in usched_cooperative.h.
 *
 * Freestanding-safe: no libc beyond <stdint.h>/<stddef.h>. */

#include "sched/usched_cooperative.h"
#include "vm/uvm.h"
#include "watcher/uwatcher_state.h"   /* UWatcherState — active/dirty counts */
#include "event/uevent_ring.h"        /* uevent_ring_has_pending */
#include "stdlib/temporal.h"          /* urbi_periodic_earliest_wake_us */
#include <stddef.h>
#include <stdint.h>

void
urbi_vm_liveness(const UVM *vm, UVmLiveness *out)
{
    out->runnable = vm->strand_runnable_count;

    /* Pending: internal work the next urbi_step performs without external
     * input.  ISR-ring pendingness is queried live (event_queue_count, the
     * old vestigial mirror, was deleted this task).  dirty_count / the
     * onleave queue make an idle step RUNNING so the host calls again —
     * the pre/post-loop reactive drain that consumes them with no strand
     * dispatch lands in the vm_reactive_drain pump. */
    out->pending = (uint32_t)((vm->event_ring != NULL
                               && uevent_ring_has_pending(vm->event_ring)) ? 1 : 0)
                 + vm->host_call_pending_count
                 + vm->watchers->dirty_count
                 + (uint32_t)(vm->pending_onleave_head != NULL ? 1 : 0);

    /* Armed: external-input work — reported (urbi_vm_has_live_work) but
     * excluded from QUIESCENT (owner decision 2026-06-11). */
    out->armed = vm->watchers->active_count
               + vm->strand_suspended_count
               + vm->strand_waiting_count;

    out->next_wake_us = UINT64_MAX;
    if (vm->wakeup_pending_count > 0)
        out->next_wake_us = urbi_sched_earliest_wake_us(vm);
    {
        uint64_t p = urbi_periodic_earliest_wake_us(vm);
        if (p < out->next_wake_us) out->next_wake_us = p;
    }
    out->timed = (out->next_wake_us != UINT64_MAX) ? 1U : 0U;
}
