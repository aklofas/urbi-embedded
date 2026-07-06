/* SPDX-License-Identifier: BSD-3-Clause */
/* uscratch.h — scratch-frame closure runner.
 * Runs a UClosure synchronously on a transient stack-allocated strand with a
 * bounded dispatch budget; used by watchers, events, operator overloads, and
 * the List.sort comparator path. */

#ifndef USCRATCH_H
#define USCRATCH_H

#include <stdint.h>

#include "chunk/uchunk.h"  /* UValue, UClosure typedef */
#include "urbi/types.h"    /* UExecStatus */

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct UVM;

/* Default 4096 dispatch ops — generous for typical conds (`x > 5`,
 * `obj.slot != nil`).  Override at compile time for footprint targets:
 *   -DURBI_SCRATCH_BUDGET_OPS=512
 * Conds that exhaust the budget log a warn, set out_threw=1, and return 0
 * (caller treats as cond-throw → install fails or eval skips fire). */
#ifndef URBI_SCRATCH_BUDGET_OPS
#  define URBI_SCRATCH_BUDGET_OPS 4096
#endif

/* === urbi_run_closure_on_scratch (spec #2 §6.4 + §7.3 phase 3) ===
 *
 * Run `closure` to OP_RET on a transient scratch-frame strand and capture
 * the return value.  Used by:
 *   - urbi_watcher_install_watcher_runtime (install-time cond eval, uwatcher_install.c)
 *   - urbi_watcher_invoke_condition_closure (eval-time cond)
 *
 * The transient strand is allocated on the C stack (mirroring urbi_vm_run's
 * pattern), threaded onto vm->global_realm->strands_head for the duration
 * of the call so the GC walker visits its register window, then unlinked
 * and torn down before return.  Bounded by URBI_SCRATCH_BUDGET_OPS dispatch
 * ops; cond closures must not OP_YIELD or block (spec §6.4 no-yield contract
 * — yield/block trips a debug-mode assertion and degrades to nil + warn).
 *
 * `closure` may be NULL: returns 0 immediately with *out_result=UVAL_NIL,
 * *out_threw=0 (matches the prior stub contract for watchers installed
 * without a condition).
 *
 * Return value: 0 on clean OP_RET, NULL closure, budget exhaustion, or
 * cond throw; -1 only on register-stack OOM (transient setup fail).
 * *out_result holds the OP_RET value (UVAL_NIL on OOM, NULL closure,
 * cond throw, or budget exhaustion).  *out_threw is set to 1 on unhandled
 * THROW / TAG_STOP unwind or budget exhaustion, 0 otherwise.  Callers must
 * pass non-NULL out pointers. */
int urbi_run_closure_on_scratch(struct UVM      *vm,
                                struct UClosure *closure,
                                UValue          *out_result,
                                int             *out_threw);

/* === urbi_run_closure_on_scratch_with_payload (spec #3 §5.3) ===
 *
 * Same as urbi_run_closure_on_scratch but writes `payload` into the
 * closure's R[0] before dispatch.  Used by event sync-emit subscribers
 * (uevent_emit.c) — AT_EVENT_SYNC bodies receive the emit payload as
 * their first argument.
 *
 * NULL closure handled identically to the no-payload variant
 * (returns 0, *out_result = nil, *out_threw = 0).
 *
 * Same return-value semantics: 0 on clean OP_RET / NULL closure /
 * budget exhaustion / throw; -1 only on register-stack OOM. */
int urbi_run_closure_on_scratch_with_payload(struct UVM      *vm,
                                             struct UClosure *closure,
                                             UValue           payload,
                                             UValue          *out_result,
                                             int             *out_threw);

/* === urbi_run_closure_on_scratch_ex ===
 *
 * Superset of the two variants above: `initial_r0` is optional (NULL → no
 * payload; non-NULL → written to R[0] before dispatch) and `out_fatal`
 * (optional, may be NULL) reports the body's latched fatal_status when
 * *out_threw is set — UEXEC_THROW for a genuine user throw, UEXEC_TAG_STOP /
 * UEXEC_CANCEL for control transfers, UEXEC_OK for the other abnormal exits
 * (budget exhaustion, yield, vm->last_error halt, setup OOM).
 *
 * On UEXEC_THROW, *out_result additionally carries the thrown value (nil
 * for every other *out_threw case).  The value is NOT GC-rooted on return:
 * the caller must move it into a rooted location (e.g. a strand register or
 * s->unwind_value) before any allocation can run a GC slice.  Used by the
 * operator-overload fallbacks (src/vm/uvm_op_overload.c) to propagate user
 * exceptions out of overload bodies instead of swallowing them into MISS. */
int urbi_run_closure_on_scratch_ex(struct UVM      *vm,
                                   struct UClosure *closure,
                                   const UValue    *initial_r0,
                                   UValue          *out_result,
                                   int             *out_threw,
                                   UExecStatus     *out_fatal);

/* === urbi_run_closure_on_scratch_args (v0.13.5) ===
 *
 * Multi-argument superset of _ex: deposits `args[0..nargs-1]` into the
 * closure's R[0..nargs-1] before dispatch, so a comparator `function(a, b)`
 * receives both parameters.  Introduced for List.sort(comparator), which
 * calls a user closure once per comparison on the shared scratch frame.
 * `args` may be NULL when nargs == 0.  nargs must not exceed the closure's
 * usable register window (the fixed UVM_STACK_CAP guarantees R[0..1] for the
 * 2-arg comparator case).  Same *out_result / *out_threw / *out_fatal
 * semantics as _ex, including the "thrown value is not GC-rooted on return"
 * contract — the caller must move it into a rooted slot before the next
 * allocation.  INTERNAL (Tier-4 internal-leak); not public ABI surface. */
int urbi_run_closure_on_scratch_args(struct UVM      *vm,
                                     struct UClosure *closure,
                                     const UValue    *args,
                                     uint8_t          nargs,
                                     UValue          *out_result,
                                     int             *out_threw,
                                     UExecStatus     *out_fatal);

#ifdef __cplusplus
}
#endif

#endif /* USCRATCH_H */
