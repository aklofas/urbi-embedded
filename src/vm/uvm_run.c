/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi_vm_run.c — urbi_vm_run adapter: transient-strand setup for non-strand entry points.
 * Extracted from uvm.c during v0.5.4-decompose (VM #6). */

#include "vm/uvm.h"
#include "vm/uvm_internal.h"
#include "runtime/umacros.h"         /* urbi_zero */
#include "runtime/uclosure.h"        /* UClosure, UUpvalCell */
#include "sched/ustrand.h"           /* UStrand, ustrand_destroy, urbi_strand_arm_init, USTRAND_IS_WAITING */
#include "sched/usched_cooperative.h" /* sched_strand_init */
#include "realm/urealm.h"            /* URealm, urbi_realm_global */
#include "object/umodule_instance.h" /* urbi_module_instance_create */
#include "module/umodule.h"
#include "runtime/ucleanup.h"
#include "runtime/uframe.h"
#include <stddef.h>
#include <stdint.h>

/* --- urbi_vm_run: thin adapter that wraps dispatch_loop_until_yield.
   Preserves the M2 public API contract:
   - Resets error state at entry.
   - Frees the previous run's return closure.
   - Returns UVM_OK with *out set on success, or the error code on failure.
   - Keeps vm->last_return_closure alive for the caller to inspect. */

UVMError urbi_vm_run(UVM *vm, URealm *realm, const UModule *module, UValue *out) {
    /* Reset error state at entry so callers who run multiple modules
       don't see stale last_error from a prior failure. */
    vm->last_error = UVM_OK;
    vm->last_errmsg[0] = '\0';

    /* M6 Phase 3: previously this site freed vm->last_return_closure at
     * the start of each new run.  With the closure migration introduced in
     * this phase (run-end closures move to vm->stdlib_closures rather than
     * being freed), the closure stays alive until vm_destroy.  Just clear
     * the pointer — the closure is already owned by the stdlib_closures
     * sweep. */
    vm->last_return_closure = NULL;

    /* Initialize out to Nil; overwritten on OP_RET success. */
    UValue nil = {0};  /* kind = UVAL_NIL, payload zeroed */
    *out = nil;

    /* Empty module: no instructions to dispatch; return Nil. */
    if (module->instr_count == 0) {
        return UVM_OK;
    }

    /* Create a transient strand for this run.
       We zero-init manually rather than calling ustrand_init to avoid
       pre-allocating the cleanup stack (which unwind_walk wires at T9;
       the M2 baseline dispatcher never uses it).  This preserves the
       M2 contract that the first allocation failure returns OOM for the
       register stack, not the cleanup stack. */
    UStrand strand;
    urbi_zero(&strand, sizeof(strand));
    strand.vm                   = vm;
    strand.state                = USTRAND_STATE_DORMANT;
    strand.is_transient_strand = 1U;  /* T33: discriminator for OP_FORK_* guards */

    /* Allocate the per-strand register stack first (preserves M2 OOM contract:
     * first allocation failure → UVM_OOM with diagnostic before cleanup init).
     * CHSTR-022: delegates alloc+zero to urbi_strand_arm_init; the manual
     * error path is preserved here because urbi_vm_run needs to set last_error
     * before returning (urbi_strand_arm_init returns -1 without diagnostics). */
    if (urbi_strand_arm_init(&strand) != 0) {
        vm->last_error = UVM_OOM;
        vm_format_oom(vm, UVM_STACK_CAP * sizeof(UValue));
        ustrand_destroy(&strand, vm);
        return UVM_OOM;
    }

    /* T10: initialise the cleanup stack so OP_TRY_BEGIN can push entries.
     * Failure leaves cleanup_base=NULL; OP_TRY_BEGIN detects and halts safely. */
    (void)strand_cleanup_stack_init(&strand, vm, URBI_CLEANUP_MAX);

    /* API-004 (Wave 5): route this transient onto realm->strands_head — the
     * caller-supplied realm if given, else the VM's global Realm (lazy-created
     * on first use).  Failure here is non-fatal — the strand stays realm=NULL
     * and the GC walker simply skips it.  The strand is unlinked again before
     * the matching ustrand_destroy below.  Per pre-M4 GC strand-walker spec §5.1.
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

    /* Wire frame-0 from module. */
    strand.R          = strand.stack;
    strand.pc         = module->instructions;
    strand.pc_base    = module->instructions;
    strand.cur_consts = module->constants;
    strand.module     = module;
    strand.root_proto = module->root_proto;  /* v0.8.1 Phase 1: fast-path alias */
    /* v0.8.0: bump module refcount for the strand binding.  Decrement
     * fires in ustrand_destroy at the end of this function (single matched
     * pair for the transient path).  The const-cast is necessary because
     * the v0.5.8 cppcheck sweep narrowed the parameter; Task 9 reverts
     * the const annotation. */
    umodule_refcount_inc((UModule *)module, vm);
    /* M4 follow-up / T72 fix: always create a fresh UModuleInstance for each
     * urbi_vm_run call.  urbi_get_or_create_module_instance is unsuitable here
     * because the REPL stack-allocates UModule and reuses the same stack
     * address across calls; the cache lookup would return a stale instance
     * with old (freed) ic_names.  Forcing fresh creation ensures ic->name is
     * populated from the current module's ic_names table.
     *
     * urbi_run_chunk pre-creates an instance via get_or_create before calling
     * urbi_vm_run; that cached instance is shadowed by this fresh one (prepended to
     * vm->module_instances_head) but both are functionally correct — only this
     * strand's module_instance is used for IC dispatch during this run. */
    strand.module_instance = urbi_module_instance_create(vm, (UModule *)module);
    strand.frame_count = 0;
    strand.open_upvals = NULL;
    strand.closure_list = NULL;
    strand.closed_cells = NULL;
    strand.out_slot   = out;  /* OP_RET at top-frame writes *out_slot */
    strand.state      = USTRAND_STATE_RUNNING;
    /* Arm the per-strand instruction budget via sched_strand_init so the
     * safepoint budget check does not immediately yield.  The transient
     * strand is zero-initialised above, leaving instruction_budget_remaining=0;
     * without this the first safepoint (OP_CALL, backward JMP, or non-top
     * OP_RET) exits before reaching watcher_eval_dirty.  urbi_vm_run re-enters on
     * yield so forward-progress is correct, but watcher_eval_dirty never fires
     * (the exit happens before the hook).  sched_strand_init was previously
     * skipped for transients; calling it here also zero-initialises the
     * scheduler list pointers (already zero from the volatile loop above,
     * so this is idempotent for all fields other than the budget). */
    sched_strand_init(&strand, NULL);

    /* Run to completion: loop until strand is DEAD or a fatal error sets last_error.
       OP_YIELD or per-strand budget exhaustion leaves state READY — treat as
       "continue" for the M2 API contract (urbi_vm_run must block until completion).
       M6 Phase 7: vm->cur_strand must be set during dispatch so native methods
       (Exception.raise, urbi_throw callers, c_event_waituntil, etc.) can
       reach the running strand.  Pre-Phase-7 only ustep.c set this field;
       urbi_vm_run is the synchronous one-shot path and was a gap. */
    for (;;) {
        vm->cur_strand = &strand;
        (void)dispatch_loop_until_yield(&strand, /* step_budget */ UINT64_MAX);
        vm->cur_strand = NULL;
        if (strand.state == USTRAND_STATE_DEAD) break;
        if (vm->last_error != UVM_OK) break;
        if (strand.state == USTRAND_STATE_READY) {
            /* OP_YIELD (between separator children) or per-strand budget.
               Remove from ready queue (sched_strand_yield enqueued it),
               reset to RUNNING, and re-enter dispatch.
               T24 / VM-011: route through sched_dequeue_ready_head so the
               runnable-count post-condition (and ready-queue link cleanup)
               lives at the sched boundary, not at the driver site.  Future
               drivers that pop the ready head get the invariant for free. */
            if (vm->ready_head == &strand) {
                sched_dequeue_ready_head(vm);
            }
            strand.state = USTRAND_STATE_RUNNING;
            continue;
        }
        if (USTRAND_IS_WAITING(&strand)) {
            /* M2 baseline has no blocking opcodes; WAITING here is a bug. */
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm, "strand blocked unexpectedly in urbi_vm_run");
            break;
        }
        /* RUNNING with step_budget exhausted (UINT64_MAX → shouldn't happen). */
        break;
    }

    /* M6 Phase 3: migrate every run-allocated closure onto vm->stdlib_closures
     * instead of freeing them at run end.  The earlier shape eagerly freed the
     * strand's closure_list at uvm_run exit, but a run can leak references to
     * those closures into long-lived storage (realm-global slots stored via
     * SETSLOT, watcher condition / body / onleave fields, etc.) that the
     * subsequent run's GC walker would dereference, hitting use-after-free.
     *
     * Phase 3 surfaced this latent issue (it was always there, but pre-Phase 3
     * the realm-global-store-then-GC-walk pattern stayed under the GC trigger
     * threshold; Phase 3's nine extra Object-proto slots add enough allocation
     * pressure to cross the threshold and trigger the walk on the next run).
     *
     * The fix here: closures created by OP_CLOSURE share lifetime with the VM
     * (vm_destroy reclaims them via the stdlib_closures sweep already in
     * uvm_init.c).  This wastes memory on truly transient closures (the
     * common case for a single-shot run) but eliminates the dangling-pointer
     * class.  v1.x GC-managed UClosure (regime 3 promotion in
     * gc_incremental.c) replaces this with proper sweep-time reclamation.
     *
     * vm->last_return_closure is still set so urbi_vm_run callers can
     * inspect the result; the underlying memory is no longer freed at the
     * next call (it's already on stdlib_closures), so the field is now
     * effectively informational. */
    {
        UClosure *out_cl = (out->kind == (uint8_t)UVAL_CLOSURE)
                           ? (UClosure *)out->v.p : NULL;
        vm->last_return_closure = out_cl;

        UClosure *cl = strand.closure_list;
        strand.closure_list = NULL;  /* null before ustrand_destroy to avoid double-free */
        /* O(n) per-closure migration with no tail-walk: pop the head off
         * strand.closure_list and prepend onto vm->stdlib_closures.  The
         * order of closures within stdlib_closures is irrelevant — the
         * list is only used for the destroy-time sweep. */
        while (cl != NULL) {
            UClosure *next = cl->next_alloc;
            cl->next_alloc = vm->stdlib_closures;
            vm->stdlib_closures = cl;
            cl = next;
        }
    }

    /* M6 Phase 3: migrate heapified upvals to vm->stdlib_upvalues so they
     * outlive their owning closures (which are also migrated to
     * vm->stdlib_closures above).  Symmetric with the closure migration —
     * both are reclaimed at urbi_vm_destroy.  Pre-Phase 3 these were freed
     * eagerly, but a closure that survives the run holds references to
     * its upvals; freeing the upvals dangles `closure->upvals[i]`. */
    {
        UUpvalCell *cell = strand.closed_cells;
        strand.closed_cells = NULL;  /* null before ustrand_destroy to avoid double-free */
        while (cell != NULL) {
            UUpvalCell *next = cell->next;
            cell->next = vm->stdlib_upvalues;
            vm->stdlib_upvalues = cell;
            cell = next;
        }
    }

    /* Free any open upvalue cells still on the strand. */
    vm_free_open_upvalues(vm, &strand);
    strand.open_upvals = NULL;  /* null before ustrand_destroy to avoid double-free */

    /* CHSTR-044: free register stack via triplet helper. */
    urbi_strand_register_stack_free(&strand, vm);

    /* T33: unlink the transient from global_realm->strands_head before
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

    UVMError rc = vm->last_error;
    ustrand_destroy(&strand, vm);
    return rc;
}
