/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi_run_closure_on_scratch[_with_payload] — shared scratch-frame closure runner.
 *
 * Spec ref: #2 §6.4 + §7.3 phase 3 (no-payload variant);
 *           #3 §5.3            (payload variant).
 * Mirrors urbi_vm_run's transient-strand pattern (src/uvm.c:2112) but scoped to
 * single-closure evaluation with bounded dispatch budget and no-yield contract.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * All allocation goes through vm->alloc_fn.
 *
 * Used by:
 *   - install_watcher_runtime (install path cond eval, uwatcher_install.c)
 *   - invoke_condition_closure                  (eval path, uwatcher_eval.c)
 *   - invoke_body_inline                        (AT_SYNC body, uwatcher_eval.c)
 *   - invoke_onleave_inline                     (falling-edge onleave, uwatcher_eval.c)
 *   - run_watcher_onleave                       (drain onleave, uwatcher_drain.c)
 *   - run_event_body_on_scratch                 (event sync-emit body, uevent_emit.c)
 *
 * **Limitation:** strand.module is left NULL.  The dispatch loop dereferences
 * s->module in OP_CLOSURE (nested function literal) and in some type-error
 * formatters.  Cond closures with nested function literals are unsupported
 * at v0.5.1; typical conds (x > 5, obj.slot != nil, 1 + 1) don't trip this.
 * Plumbing module through requires a UProto.module back-pointer or wrapping
 * UClosure with the owning module reference — out of scope for this patch. */

#include "watcher/uwatcher.h"
#include "vm/uvm.h"
#include "runtime/uclosure.h"   /* UClosure full definition (M4: embeds UCell) */
#include "sched/ustrand.h"
#include "runtime/ucleanup.h"
#include "realm/urealm.h"
#include "urbi/urbi.h"
#include "runtime/umacros.h"
#include "chunk/uchunk.h"
#include "object/uchunk_instance.h"
#include "runtime/uframe.h"
#include <stddef.h>

/* === run_on_scratch_core (file-static) ===
 *
 * Shared implementation for both no-payload and payload variants.
 * If `initial_r0` is non-NULL, writes `*initial_r0` to strand.R[0] after
 * arm but before dispatch.  All other behaviour is identical to the
 * documented contract on urbi_run_closure_on_scratch.
 *
 * `out_fatal` (optional, may be NULL): when the body dies with a latched
 * fatal_status, receives that status (UEXEC_THROW / UEXEC_TAG_STOP /
 * UEXEC_CANCEL); UEXEC_OK for every other outcome (clean return, budget
 * exhaustion, yield, vm->last_error halt, setup OOM).  refactor-3 VM-07:
 * lets operator-overload fallbacks distinguish a genuine user throw from
 * the other abnormal exits that *out_threw conflates. */
static int
run_on_scratch_core(struct UVM       *vm,
                    struct UClosure  *closure,
                    const UValue     *initial_r0,
                    UValue           *out_result,
                    int              *out_threw,
                    UExecStatus      *out_fatal)
{
    UValue nil = {0};   /* kind = UVAL_NIL, payload zeroed */
    UProtoInstanceArr *scratch_arr = NULL; /* heap buf for synthetic module_instance */

    URBI_ASSERT_NOT_ISR(vm);
    URBI_INTERNAL_ASSERT(out_result != NULL);
    URBI_INTERNAL_ASSERT(out_threw  != NULL);

    *out_result = nil;
    *out_threw  = 0;
    if (out_fatal != NULL) *out_fatal = UEXEC_OK;

    /* Reset last_error at entry so a stale error from a prior VM operation
     * doesn't get misread as a cond throw.  Mirrors urbi_vm_run's entry pattern. */
    vm->last_error = UVM_OK;
    vm->last_errmsg[0] = '\0';

    /* NULL closure: graceful nil — matches the prior stub contract for
     * watchers installed without a condition. */
    if (closure == NULL) return 0;

    /* Allocate a transient strand on the C stack.  Mirrors urbi_vm_run. */
    UStrand strand;
    urbi_zero(&strand, sizeof(strand));
    strand.vm                   = vm;
    strand.state                = USTRAND_STATE_DORMANT;
    strand.is_transient_strand = 1U;  /* guards reject OP_FORK_DETACH/JOIN */

    /* Arm from the closure: allocates register stack, wires pc / pc_base /
     * cur_consts / frame_count from closure->proto.  Returns -1 on OOM. */
    if (urbi_strand_arm_from_closure(&strand, closure) != 0) {
        if (vm->host_log_fn) {
            vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                "scratch-frame arm: register-stack OOM");
        }
        return -1;
    }

    /* Payload init: write to R[0] after the register stack exists but before
     * dispatch.  AT_EVENT_SYNC subscribers receive the emit payload as their
     * first argument here.  strand.R is guaranteed non-NULL by a successful
     * urbi_strand_arm_from_closure return, so no defensive NULL check needed. */
    if (initial_r0 != NULL) {
        strand.R[0] = *initial_r0;
    }

    /* strand.module is intentionally left NULL — see the function docstring
     * for the OP_CLOSURE limitation.  Typical cond closures don't trigger it. */

    /* Synthetic module_instance: OP_GETSLOT at frame_count==0 reads
     * s->module_instance->proto_instances->entries[0].ic_table.  The scratch
     * strand runs closure directly at frame_count==0 (not the root chunk), so
     * entries[0] must expose closure's own IC table.  Allocate a minimal
     * UProtoInstanceArr (one entry) and a stack-local UChunkInstance shell.
     * Freed in teardown below; GC does not chase strand.module_instance.
     *
     * IC pointer-sharing: only the bare UChunkInstance + UProtoInstanceArr
     * structs are allocated — no trailing IC byte array.  entries[0].ic_table
     * is a borrowed read-only pointer into closure->proto_inst's existing IC
     * table.  This is safe because (1) the scratch frame holds no slot-write
     * barriers that would mutate ICs, (2) closure->proto_inst already has
     * its ICs populated by the time the scratch helper runs, and (3) the
     * scratch frame's lifetime is strictly contained within the closure's.
     * M6 will formalize this via a UClosure.owning_mi field; today the
     * scratch helper carries the pointer-share invariant in code.
     *
     * WATCH-007 (v0.5.7): on alloc failure, signal *out_threw = 1 and skip
     * dispatch.  Pre-fix the OOM path silently left strand.module_instance
     * = NULL, then OP_GETSLOT (frame_count==0 path) dereferenced NULL — a
     * segfault that masqueraded as a soft cond throw. */
    UChunkInstance scratch_mi;
    int scratch_arr_alloc_failed = 0;
    urbi_zero(&scratch_mi, sizeof(scratch_mi));
    if (closure->proto_inst != NULL) {
        size_t arr_bytes = sizeof(UProtoInstanceArr) + sizeof(UProtoInstance);
        scratch_arr = (UProtoInstanceArr *)vm->alloc_fn(NULL, arr_bytes, vm->alloc_ud);
        if (scratch_arr != NULL) {
            urbi_zero(scratch_arr, arr_bytes);
            scratch_arr->n = 1;
            scratch_arr->entries[0].proto    = closure->proto;
            scratch_arr->entries[0].ic_table = closure->proto_inst->ic_table;
            scratch_mi.vm               = vm;
            scratch_mi.proto_instances  = scratch_arr;
            strand.module_instance      = &scratch_mi;
        } else {
            scratch_arr_alloc_failed = 1;
            if (vm->host_log_fn) {
                vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                    "scratch-frame: synthetic module_instance alloc failed");
            }
        }
    }

    /* Initialise the cleanup stack so OP_TRY_BEGIN can push entries.
     * Failure leaves cleanup_base=NULL; OP_TRY_BEGIN detects and halts safely. */
    (void)strand_cleanup_stack_init(&strand, vm, URBI_CLEANUP_MAX);

    /* Thread onto a realm's strands_head so the GC walker sees the strand's
     * register window (mirrors urbi_vm_run's transient-strand dance).
     *
     * GC-006 + GC-038 (audit findings closed by construction):
     * Before this linkage step, no GC slice can fire — every prior allocation
     * uses vm->alloc_fn directly (register-stack alloc, scratch_arr, cleanup
     * stack), none of which trigger GC.  Once linked, sched_walk_roots
     * (src/sched/usched_cooperative.c) iterates vm->realms_head →
     * realm.strands_head → strand_walk_roots and visits the full register
     * window via the existing s->stack scan — so any UValue placed in
     * strand.R[k] (including the payload write at line 94 below) is rooted
     * for the duration of dispatch.  Unlinking happens below in the teardown
     * block, after dispatch_loop_until_yield returns and *out_result has been
     * captured into the caller's local.
     *
     * v0.9.0-repl: use the closure's owning realm so that OP_LOAD_REALM_GLOBAL
     * resolves globals from the realm where the closure was compiled.  Fall
     * back to global_realm when the chain is absent (native closures, stubs). */
    {
        URealm *scratch_realm = NULL;
        /* closure != NULL is guaranteed by the early-return guard above. */
        if (closure->proto != NULL
                && closure->proto->owning_module_instance != NULL
                && closure->proto->owning_module_instance->module != NULL) {
            scratch_realm = closure->proto->owning_module_instance->module->owning_realm;
        }
        if (scratch_realm == NULL) {
            scratch_realm = urbi_realm_global(vm);
        }
        if (scratch_realm != NULL) {
            strand.realm         = scratch_realm;
            strand.next_in_realm = scratch_realm->strands_head;
            scratch_realm->strands_head = &strand;
        }
    }

    /* out_slot wires OP_RET's top-frame return value into our local. */
    UValue out_local = {0};
    strand.out_slot  = &out_local;
    strand.state     = USTRAND_STATE_RUNNING;

    /* WATCH-007: skip dispatch when scratch_arr alloc failed; OP_GETSLOT at
     * frame_count==0 would deref strand.module_instance == NULL. */
    if (scratch_arr_alloc_failed) {
        *out_threw = 1;
    } else {
        /* Run with bounded budget.  Cond closures must not yield (spec §6.4),
         * but we cap dispatch ops as a defensive measure.
         *
         * refactor-3 VM-10 + SCHED-10: the nested dispatch must identify the
         * scratch strand as current (slot faults / natives throw on
         * vm->cur_strand — pre-fix the throw landed on the OUTER strand, e.g.
         * the loader strand executing OP_AT_INSTALL, or was lost) and must
         * not clobber the embedder's urbi_step budget
         * (dispatch_loop_until_yield overwrites vm->step_budget_remaining at
         * entry).  Save both, point cur_strand at the scratch strand for the
         * duration of the dispatch, restore after. */
        UStrand *saved_cur    = vm->cur_strand;
        uint64_t saved_budget = vm->step_budget_remaining;
        vm->cur_strand = &strand;
        (void)dispatch_loop_until_yield(&strand, URBI_SCRATCH_BUDGET_OPS);
        vm->cur_strand = saved_cur;
        vm->step_budget_remaining = saved_budget;

        /* Detect unhandled throw / abnormal exit. */
        if (vm->last_error != UVM_OK) {
            *out_threw = 1;
            vm->last_error = UVM_OK;
            vm->last_errmsg[0] = '\0';
        } else if (strand.fatal_status != UEXEC_OK) {
            /* refactor-3 VM-10: with cur_strand pointing at the scratch
             * strand, typed throws (slot faults via slot_throw_or_fatal,
             * urbi_raise_typed natives) now land here and unwind to a DEAD
             * strand with fatal_status latched — vm->last_error stays UVM_OK
             * on that path.  Report a throw so install/eval fail-soft
             * instead of misreading the death as a clean OP_RET (which
             * would deliver nil as the cond/body result). */
            *out_threw = 1;
            if (out_fatal != NULL) *out_fatal = strand.fatal_status;
            if (strand.fatal_status == UEXEC_THROW) {
                /* refactor-3 VM-07: surface the thrown value so operator-
                 * overload callers can re-deposit the user's exception at
                 * the call site instead of replacing it with a numeric
                 * TypeError.  The unrooted window opened at strand DEATH,
                 * not at the realm-unlink below: strand_walk_roots skips
                 * DEAD strands, so fatal_value is already unrooted by the
                 * time this branch runs.  Between this copy and the caller's
                 * re-deposit
                 * into a rooted location only frees happen (register-stack
                 * free, scratch_arr free, ustrand_destroy — no allocation,
                 * hence no GC slice), so the value stays live.
                 * For TAG_STOP / CANCEL *out_result stays nil: those are
                 * control transfers, not user exceptions, and callers keep
                 * their legacy fail-soft handling for them. */
                *out_result = strand.fatal_value;
            }
        } else if (strand.state == USTRAND_STATE_DEAD) {
            /* Clean OP_RET — capture the return value. */
            *out_result = out_local;
        } else {
            /* RUNNING with budget exhausted, READY (yield), WAITING (block), or
             * SUSPENDED (gate-suspended member strand, v0.13.3 SCHED-08).
             * The latter three violate the §6.4 no-yield contract; treat as
             * cond-throw so install/eval can fail-soft.  Diagnostic neutralized
             * because the same core also handles AT_SYNC bodies, onleave handlers,
             * and event sync-emit bodies — not just cond closures. */
            *out_threw = 1;
            if (vm->host_log_fn) {
                vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                    "scratch-frame body exceeded budget or yielded");
            }
        }
    }

    /* Unlink from global_realm before tearing down (symmetric with insert
     * above; mirrors urbi_vm_run's pre-ustrand_destroy unlink). */
    if (strand.realm != NULL && strand.realm->strands_head != NULL) {
        UStrand **pp = &strand.realm->strands_head;
        while (*pp != NULL) {
            if (*pp == &strand) {
                *pp = strand.next_in_realm;
                strand.next_in_realm = NULL;
                break;
            }
            pp = &(*pp)->next_in_realm;
        }
        strand.realm = NULL;
    }

    /* v0.8.4 Option B Step C-2/C-3: UClosure and UUpvalCell are GC-managed.
     * The pre-C-2 free loops here were double-free hazards (C-2 migrated
     * vm_alloc_closure to urbi_gc_alloc but missed this scratch teardown path).
     * Just clear the open_upvals head pointer — the GC sweep reclaims the cells
     * when they become unreachable from any root.  closure_list + closed_cells
     * fields were deleted at Step C-3. */
    strand.open_upvals = NULL;

    /* Free the register stack. */
    if (strand.stack != NULL) {
        vm->alloc_fn(strand.stack, 0, vm->alloc_ud);
        strand.stack = NULL;
    }

    /* Free the synthetic module_instance buffer (paired with the alloc above). */
    if (scratch_arr != NULL) {
        strand.module_instance = NULL;
        vm->alloc_fn(scratch_arr, 0, vm->alloc_ud);
        scratch_arr = NULL;
    }

    ustrand_destroy(&strand, vm);
    return 0;
}

/* urbi_run_closure_on_scratch (WATCH-011): synchronously run the closure
 * body on a scratch frame.
 *
 * NOTE: this function does NOT set vm->watchers->in_scratch despite the
 * name.  The flag is owned by callers that need re-entry guarding
 * (specifically c_event_emit_sync's run_event_body_on_scratch in
 * src/event/uevent_emit.c, which sets the flag around its call to this
 * helper).  Other callers — install_watcher_runtime, invoke_condition_closure,
 * invoke_body_inline, invoke_onleave_inline, run_watcher_onleave — rely on
 * caller-owned vm->watchers->in_eval / vm->watchers->in_install for re-entry
 * protection instead.  See WATCH-036 (uvm.h field comment on
 * in_watcher_scratch — pre-W2 name; current field is in_scratch) for the
 * asymmetry rationale. */
int
urbi_run_closure_on_scratch(struct UVM      *vm,
                            struct UClosure *closure,
                            UValue          *out_result,
                            int             *out_threw)
{
    return run_on_scratch_core(vm, closure, NULL, out_result, out_threw, NULL);
}

int
urbi_run_closure_on_scratch_with_payload(struct UVM      *vm,
                                         struct UClosure *closure,
                                         UValue           payload,
                                         UValue          *out_result,
                                         int             *out_threw)
{
    return run_on_scratch_core(vm, closure, &payload, out_result, out_threw,
                               NULL);
}

int
urbi_run_closure_on_scratch_ex(struct UVM      *vm,
                               struct UClosure *closure,
                               const UValue    *initial_r0,
                               UValue          *out_result,
                               int             *out_threw,
                               UExecStatus     *out_fatal)
{
    return run_on_scratch_core(vm, closure, initial_r0, out_result, out_threw,
                               out_fatal);
}
