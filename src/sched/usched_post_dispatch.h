/* SPDX-License-Identifier: BSD-3-Clause */
/* src/sched/usched_post_dispatch.h — post-dispatch fix-up helper.
 *
 * urbi_sched_post_dispatch consolidates the four bookkeeping steps that must run
 * after every dispatch-loop iteration, regardless of which driver initiated
 * the dispatch.  Previously these steps lived exclusively in urbi_step
 * (scheduler audit F3); any alternative driver had to replicate them in order
 * or silently lose forward progress, leak memory, or corrupt the runnable count.
 *
 * The four fix-up steps (see F3 for the full analysis):
 *
 *   1. Runnable-count DEAD decrement (single-writer
 *      scheme).  A strand that left dispatch DEAD was RUNNING and therefore
 *      counted; it leaves the counted set here.  WAITING strands were
 *      decremented by urbi_sched_strand_block, READY (yield) strands were
 *      re-enqueued count-neutrally, SUSPENDED strands were decremented by
 *      urbi_strand_suspend — none of them is touched here.  (The
 *      pre-refactor step 1 was a WAITING *re-increment* pairing with a
 *      decrement in urbi_sched_dequeue_ready_head; that pair produced the B10
 *      phantom-count leak and is gone.)
 *
 *   2. Eager DEAD-strand reap.  Heap-allocated strands accumulate on
 *      realm->strands_head until urbi_realm_destroy unless reaped eagerly
 *      here.  Each carries a register stack (UVM_STACK_CAP * sizeof(UValue),
 *      ~32 KB at default); without eager reap the leak climbs into the multi-MB
 *      range at moderate event rates and wedges on constrained targets
 *      (v0.7.x ESP32-EYE eye_demo wedge, ~200 body completions).
 *
 *   3. Sleep-queue wake.  Walks vm->sleep_q_head and calls urbi_sched_strand_unblock
 *      for every strand whose wake_us <= now.  Must run per-iteration to bound
 *      wakeup latency to one dispatch cycle.
 *
 *   4. Periodic pump.  Fires every()-body re-spawn for any periodic whose
 *      next_fire_us has elapsed.  Running this inside the loop (rather than
 *      only pre-loop) allows a body strand that just completed to re-arm and
 *      have its next fire become a READY strand within the same urbi_step call.
 *
 * Callers:
 *   - urbi_step (src/vm/ustep.c): primary scheduler driver.
 *   - urbi_vm_run (src/vm/uvm_run.c): synchronous-eval driver; previously
 *     skipped steps 2-4 because the transient-strand model rejects OP_FORK_*,
 *     but calling the helper aligns it with the full fix-up sequence.
 *   - Any future alternative driver (multi-VM urbi_step_all, debugger-step,
 *     RT scheduler) should call this helper rather than reimplementing the
 *     steps individually.
 *
 * Note on urbi_repl_serve_step (src/repl/urepl.c): that function is a pure
 * data-plane sweep (accept/read/dispatch-jobs/write/disconnect) and performs
 * no strand dispatch; it does not call urbi_sched_post_dispatch.  The VM-thread
 * urbi_step loop drives all actual bytecode execution and runs the fix-ups
 * via this helper.
 *
 * Freestanding-safe: all includes are <stdbool.h> and <stdint.h> plus
 * project-internal headers that are themselves freestanding. */

#ifndef USCHED_POST_DISPATCH_H
#define USCHED_POST_DISPATCH_H

#include "sched/ustrand.h"
#include "vm/uvm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* urbi_sched_post_dispatch: run the four fix-up operations that close out a
 * dispatch-loop iteration.  Called by urbi_step internally after each strand
 * dispatch; also called by alternative drivers that run urbi_vm_dispatch_loop_until_yield
 * directly (e.g. urbi_vm_run).
 *
 * Preconditions:
 *   - s was the strand just dispatched (vm->cur_strand must be NULL on entry;
 *     the driver is responsible for clearing it before calling this helper).
 *   - s->fatal_status has already been checked by the driver; this helper does
 *     NOT recheck it (the FATAL path must return before reaching this call).
 *   - vm->host_time_us must be non-NULL when sleep-queue entries are present
 *     (step 3); this is guaranteed for any VM that registered a time hook.
 *
 * Post-conditions:
 *   - If s was DEAD and !s->is_transient_strand, vm->strand_runnable_count is
 *     decremented (step 1, SCHED-01): the strand was RUNNING (counted) and is
 *     no longer runnable.  WAITING/SUSPENDED strands were already uncounted
 *     by their parking transition; transient strands never participate in
 *     the count.
 *   - If s was DEAD and !s->is_transient_strand on entry, s is freed via
 *     urbi_strand_destroy (step 2); the caller MUST NOT dereference s after this
 *     call returns.  Transient strands (urbi_vm_run stack-local strands) are
 *     excluded — their lifetime is bounded by the urbi_vm_run call frame.
 *   - Any sleep-queue strand whose wake_us <= now is made READY (step 3).
 *   - Any periodic whose next_fire_us <= now spawns a body strand (step 4).
 *
 * See scheduler audit F3 for the full analysis of the duplication problem this
 * helper resolves. */
void urbi_sched_post_dispatch(UVM *vm, UStrand *s);

#ifdef __cplusplus
}
#endif

#endif /* USCHED_POST_DISPATCH_H */
