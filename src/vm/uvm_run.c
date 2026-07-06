/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi_vm_run.c — urbi_vm_run adapter: transient-strand setup for non-strand entry points.
 * Extracted from uvm.c during v0.5.4-decompose (VM #6). */

#include "vm/uvm.h"
#include "vm/uvm_internal.h"
#include "runtime/umacros.h"         /* urbi_zero */
#include "runtime/uclosure.h"        /* UClosure, UUpvalCell */
#include "sched/ustrand.h"           /* UStrand, ustrand_destroy, urbi_strand_arm_init, USTRAND_IS_WAITING */
#include "sched/usched_cooperative.h" /* urbi_sched_strand_init */
#include "sched/usched_post_dispatch.h" /* urbi_sched_post_dispatch (scheduler F3) */
#include "realm/urealm.h"            /* URealm, urbi_realm_global */
#include "object/uchunk_instance.h" /* urbi_chunk_instance_create */
#include "chunk/uchunk.h"
#include "runtime/ucleanup.h"
#include "runtime/uframe.h"
#include "value/uvalue.h"     /* uvalue_format: uncaught-throw errmsg */
#include <stddef.h>
#include <stdint.h>
#if __STDC_HOSTED__
#  include <stdio.h>   /* snprintf: uncaught-throw errmsg formatting */
#endif

/* --- urbi_vm_run: thin adapter that wraps urbi_vm_dispatch_loop_until_yield.
   Preserves the public API contract:
   - Resets error state at entry.
   - Frees the previous run's return closure.
   - Returns UVM_OK with *out set on success, or the error code on failure.
   - Keeps vm->last_return_closure alive for the caller to inspect. */

int urbi_vm_run(UVM *vm, URealm *realm, const UProto *root, UValue *out) {
    /* Reset error state at entry so callers who run multiple modules
       don't see stale last_error from a prior failure. */
    vm->last_error = UVM_OK;
    vm->last_errmsg[0] = '\0';

    /* Clear last_return_closure; the GC keeps it alive via the root walker
     * (vm_misc_walk_roots) until this assignment drops the last reference. */
    vm->last_return_closure = NULL;

    /* Initialize out to Nil; overwritten on OP_RET success. */
    UValue nil = {0};  /* kind = UVAL_NIL, payload zeroed */
    *out = nil;

    /* Empty root (or failed emit): no instructions to dispatch; return Nil. */
    if (root == NULL || root->instr_count == 0) {
        return UVM_OK;
    }

    /* Create a transient strand for this run.
       We zero-init manually rather than calling ustrand_init to avoid
       pre-allocating the cleanup stack (which the unwind walker requires;
       the transient-strand path never uses it).  This preserves the
       contract that the first allocation failure returns OOM for the
       register stack, not the cleanup stack. */
    UStrand strand;
    urbi_zero(&strand, sizeof(strand));
    strand.vm                   = vm;
    strand.state                = USTRAND_STATE_DORMANT;
    strand.is_transient_strand = 1U;  /* discriminator for OP_FORK_* guards */

    /* Allocate the per-strand register stack first (preserves OOM contract:
     * first allocation failure → UVM_OOM with diagnostic before cleanup init).
     * CHSTR-022: delegates alloc+zero to urbi_strand_arm_init; the manual
     * error path is preserved here because urbi_vm_run needs to set last_error
     * before returning (urbi_strand_arm_init returns -1 without diagnostics). */
    if (urbi_strand_arm_init(&strand) != 0) {
        vm->last_error = UVM_OOM;
        urbi_vm_format_oom(vm, UVM_STACK_CAP * sizeof(UValue));
        ustrand_destroy(&strand, vm);
        return UVM_OOM;
    }

    /* Initialise the cleanup stack so OP_TRY_BEGIN can push entries.
     * Failure leaves cleanup_base=NULL; OP_TRY_BEGIN detects and halts safely. */
    (void)urbi_sched_strand_cleanup_stack_init(&strand, vm, URBI_CLEANUP_MAX);

    /* API-004 (Wave 5): route this transient onto realm->strands_head — the
     * caller-supplied realm if given, else the VM's global Realm (lazy-created
     * on first use).  Failure here is non-fatal — the strand stays realm=NULL
     * and the GC walker simply skips it.  The strand is unlinked again before
     * the matching ustrand_destroy below.  Per GC strand-walker spec §5.1.
     * entry_closure stays NULL — that is the discriminator the OP_FORK_DETACH
     * / OP_FORK_JOIN guards now use to reject forks from a urbi_vm_run transient.
     *
     * Pre-Wave-5: this branch unconditionally bound the transient to the global
     * Realm regardless of any realm argument the caller might have intended;
     * the realm argument was added at Wave 5 to thread urbi_run_chunk's
     * caller-supplied Realm through here.  NULL → global preserves the
     * implicit pre-Wave-5 behavior for callers that don't care which Realm
     * they run in. */
    {
        URealm *target_realm = realm;
        if (target_realm == NULL) target_realm = urbi_realm_global(vm);
        if (target_realm != NULL) {
            strand.realm         = target_realm;
            strand.next_in_realm = target_realm->strands_head;
            target_realm->strands_head = &strand;
        }
    }

    /* Wire frame-0 from root.
     * v0.9.2: root IS the root UProto — no intermediate module. */
    strand.root_proto = (UProto *)root;
    strand.R          = strand.stack;
    strand.pc         = strand.root_proto->instructions;
    strand.pc_base    = strand.root_proto->instructions;
    strand.cur_consts = strand.root_proto->constants;
    /* v0.8.1 Phase 2 (Variant B fusion): strand-bind bump goes to root_proto.
     * Decrement fires in ustrand_destroy at the end of this function.
     * v0.10.1: typed-handle acquire for diagnostics (F3 transient site). */
    urbi_proto_strand_ref_acquire(strand.root_proto, URBI_PROTO_REF_OWNER_TRANSIENT);
    /* Always create a fresh UChunkInstance for each
     * urbi_vm_run call.  urbi_get_or_create_chunk_instance is unsuitable here
     * because urbi_repl_eval heap-allocates UProto per line and reuses the same
     * address across calls; the cache lookup would return a stale instance
     * with old (freed) ic_names.  Forcing fresh creation ensures ic->name is
     * populated from the current root's ic_names table.
     *
     * urbi_run_chunk pre-creates an instance via get_or_create before calling
     * urbi_vm_run; that cached instance is shadowed by this fresh one (prepended to
     * vm->module_instances_head) but both are functionally correct. */
    strand.module_instance = urbi_chunk_instance_create(vm, (UProto *)root);
    if (strand.module_instance == NULL) {
        vm->last_error = UVM_OOM;
        /* Unlink stack-local transient from realm before ustrand_destroy,
         * mirroring src/sched/ustrand.c:699-700 and the normal-exit path below. */
        if (strand.realm != NULL && strand.realm->strands_head != NULL) {
            UStrand **pp = &strand.realm->strands_head;
            while (*pp != NULL) {
                if (*pp == &strand) { *pp = strand.next_in_realm; break; }
                pp = &(*pp)->next_in_realm;
            }
            strand.realm = NULL;
        }
        ustrand_destroy(&strand, vm);
        return UVM_OOM;
    }
    strand.frame_count = 0;
    strand.open_upvals = NULL;
    strand.out_slot   = out;  /* OP_RET at top-frame writes *out_slot */
    strand.state      = USTRAND_STATE_RUNNING;
    /* Arm the per-strand safepoint budget via urbi_sched_strand_init so the
     * safepoint budget check does not immediately yield.  The transient
     * strand is zero-initialised above, leaving safepoint_budget_remaining=0;
     * without this the first safepoint (OP_CALL, backward JMP, or non-top
     * OP_RET) exits before reaching urbi_vm_watcher_eval_dirty.  urbi_vm_run re-enters on
     * yield so forward-progress is correct, but urbi_vm_watcher_eval_dirty never fires
     * (the exit happens before the hook).  urbi_sched_strand_init was previously
     * skipped for transients; calling it here also zero-initialises the
     * scheduler list pointers (already zero from the volatile loop above,
     * so this is idempotent for all fields other than the budget). */
    urbi_sched_strand_init(&strand, NULL);

    /* Run to completion: loop until strand is DEAD or a fatal error sets last_error.
       OP_YIELD or per-strand budget exhaustion leaves state READY — treat as
       "continue" contract (urbi_vm_run must block until completion).
       vm->cur_strand must be set during dispatch so native methods
       (Exception.raise, urbi_throw callers, urbi_event_waituntil, etc.) can
       reach the running strand.  Pre-Phase-7 only ustep.c set this field;
       urbi_vm_run is the synchronous one-shot path and was a gap. */
    for (;;) {
        vm->cur_strand = &strand;
        (void)urbi_vm_dispatch_loop_until_yield(&strand, /* step_budget */ UINT64_MAX);
        vm->cur_strand = NULL;

        /* Post-dispatch fix-ups — scheduler F3.
         *
         * urbi_sched_post_dispatch runs the four bookkeeping steps after each
         * dispatch-loop iteration.  For the transient strand (is_transient_strand=1):
         *   - Step 1 (runnable-count DEAD decrement, SCHED-01): skipped —
         *     transient strands never participate in strand_runnable_count
         *     (urbi_sched_runnable_inc/dec both skip them).
         *   - Step 2 (eager DEAD-strand reap): skipped — the strand is stack-local;
         *     lifetime bounded by this function; freed by ustrand_destroy at exit.
         *   - Step 3 (sleep-queue wake): runs — keeps sleep-blocked strands on the
         *     same VM from starving while urbi_vm_run holds the call frame.
         *   - Step 4 (periodic pump): runs — allows every()-body strands that
         *     fire during a synchronous eval to re-arm within the same call.
         *
         * Note: urbi_vm_run is the synchronous-eval path and is typically short;
         * running sleep-wake + periodic pump here is a convergence improvement but
         * does not change the fundamental semantics (the driver is still single-
         * strand). */
        urbi_sched_post_dispatch(vm, &strand);
        /* strand is still valid here (step 2 was skipped for transient). */

        if (strand.state == USTRAND_STATE_DEAD) break;
        if (vm->last_error != UVM_OK) break;
        if (strand.state == USTRAND_STATE_READY) {
            /* OP_YIELD (between separator children) or per-strand budget.
               Remove from ready queue (urbi_sched_strand_yield enqueued it),
               reset to RUNNING, and re-enter dispatch.
               VM-011: route through urbi_sched_dequeue_ready_head so the
               ready-queue link cleanup lives at the sched boundary, not at
               the driver site.  SCHED-01: the dequeue is count-neutral
               (and transients never participate in the count anyway), so
               this is purely queue-link hygiene.  Future drivers that pop
               the ready head get the link invariant for free. */
            if (vm->ready_head == &strand) {
                urbi_sched_dequeue_ready_head(vm);
            }
            strand.state = USTRAND_STATE_RUNNING;
            /* VM-04/SCHED-11 (v0.13.1-E): re-arm per-slice safepoint
             * budget so a budget-exhausted transient strand can reach the GC
             * check on the next dispatch iteration (mirrors ustep.c). */
            strand.safepoint_budget_remaining = (uint16_t)URBI_STRAND_BUDGET_MAX;
            continue;
        }
        if (USTRAND_IS_WAITING(&strand)) {
            /* urbi_vm_run is synchronous; WAITING here is a bug. */
            vm->last_error = UVM_TYPE_ERROR;
            urbi_vm_format_type_error_msg(vm, "strand blocked unexpectedly in urbi_vm_run");
            break;
        }
        if (USTRAND_IS_SUSPENDED(&strand)) {
            /* VM-03: a native suspended the transient strand
             * (t.block()/t.freeze() from inside the tag's own scope).
             * urbi_vm_run is the synchronous one-shot path — no later
             * urbi_step loop exists to resume the strand, so parking
             * would silently truncate the body at the blocking call.
             * Error loudly, mirroring the WAITING arm. */
            vm->last_error = UVM_TYPE_ERROR;
            urbi_vm_format_type_error_msg(vm, "strand suspended unexpectedly in urbi_vm_run");
            break;
        }
        /* RUNNING: safepoint-budget arm in uvm.c (B11/SCHED-03 transient guard)
         * exited without calling urbi_sched_strand_yield — the strand was not enqueued
         * so no dequeue is needed.  Re-arm the safepoint budget and continue,
         * mirroring the READY re-arm above. */
        strand.safepoint_budget_remaining = (uint16_t)URBI_STRAND_BUDGET_MAX;
    }

    /* v0.8.4 Step C-3: closure_list / closed_cells fields deleted.
     * UClosure + UUpvalCell are GC-managed; reachable cells survive via the
     * root paths (entry_closure / call-frame closures / realm globals /
     * watcher fields); unreachable cells are swept.  Record last_return_closure
     * for callers that inspect the result across calls (GC root via
     * vm_misc_walk_roots). */
    {
        UClosure *out_cl = (out->kind == (uint8_t)UVAL_CLOSURE)
                           ? (UClosure *)out->v.p : NULL;
        vm->last_return_closure = out_cl;
    }

    /* open_upvals: cleared inline (release_strand_resource_chain also clears
     * it in ustrand_destroy; belt-and-suspenders). */
    strand.open_upvals = NULL;

    /* CHSTR-044: free register stack via triplet helper. */
    urbi_strand_register_stack_free(&strand, vm);

    /* Unlink the transient from global_realm->strands_head before
     * ustrand_destroy.  The stack-local UStrand is about to leave scope; if
     * we leave it threaded, urealm_teardown_all → urbi_realm_destroy would
     * walk strands_head and call urbi_strand_destroy on a stack address.
     * Symmetric with the head-insert just before frame-0 wiring. */
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

    int rc = vm->last_error;
    if (rc == UVM_OK) {
        /* Check whether the strand died with an uncaught script throw.
         * HALT-path fatals (type errors, OOM) set vm->last_error != UVM_OK
         * and skip this arm; script-level throw leaves last_error == UVM_OK
         * but marks strand->fatal_status = UEXEC_THROW. */
        UStrandUnwind fstat;
        UValue fval;
        if (urbi_strand_is_fatal(vm, &strand, &fstat, &fval)) {
            rc = (fstat == URBI_UNWIND_THROW) ? URBI_ERR_UNCAUGHT_THROW
                                              : URBI_ERR_STRAND_FATAL;
            if (vm->last_errmsg[0] == '\0') {
#if __STDC_HOSTED__
                char fmt[64];
                uvalue_format(&fval, fmt, sizeof fmt);
                (void)snprintf(vm->last_errmsg, UVM_ERRMSG_CAP,
                               "uncaught throw: %s", fmt);
#else
                urbi_strncpy_truncating(vm->last_errmsg, UVM_ERRMSG_CAP,
                                        "uncaught throw");
#endif
            }
            if (out != NULL) *out = fval;
        }
    }
    ustrand_destroy(&strand, vm);
    return rc;
}
