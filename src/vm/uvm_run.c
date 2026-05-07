/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_run.c — uvm_run adapter: transient-strand setup for non-strand entry points.
 * Extracted from uvm.c during v0.5.4-decompose (VM #6). */

#include "vm/uvm.h"
#include "vm/uvm_internal.h"
#include "runtime/umacros.h"         /* urbi_zero */
#include "runtime/uclosure.h"        /* UClosure, UUpvalCell */
#include "sched/ustrand.h"           /* UStrand, ustrand_destroy, USTRAND_IS_WAITING */
#include "sched/usched_cooperative.h" /* sched_strand_init */
#include "realm/urealm.h"            /* URealm, urbi_realm_global */
#include "object/umodule_instance.h" /* urbi_module_instance_create */

/* --- uvm_run: thin adapter that wraps dispatch_loop_until_yield.
   Preserves the M2 public API contract:
   - Resets error state at entry.
   - Frees the previous run's return closure.
   - Returns UVM_OK with *out set on success, or the error code on failure.
   - Keeps vm->last_return_closure alive for the caller to inspect. */

UVMError uvm_run(UVM *vm, const UModule *module, UValue *out) {
    /* Reset error state at entry so callers who run multiple modules
       don't see stale last_error from a prior failure. */
    vm->last_error = UVM_OK;
    vm->last_errmsg[0] = '\0';

    /* Pre-GC: free the closure returned by the previous uvm_run (if any).
     * The caller had one run's lifetime to inspect it. */
    if (vm->last_return_closure != NULL) {
        UClosure *prev = vm->last_return_closure;
        uint8_t nup = prev->nupvals;
        size_t extra = (nup > 1u) ? (size_t)(nup - 1u) * sizeof(UUpvalCell *) : 0u;
        (void)extra;
        vm->alloc_fn(prev, 0, vm->alloc_ud);
        vm->last_return_closure = NULL;
    }

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
    strand.is_transient_strand = 1u;  /* T33: discriminator for OP_FORK_* guards */

    /* Allocate the per-strand register stack first (preserves M2 OOM contract:
     * first allocation failure → UVM_OOM with diagnostic before cleanup init). */
    const size_t stack_bytes = UVM_STACK_CAP * sizeof(UValue);
    strand.stack = (UValue *)vm->alloc_fn(NULL, stack_bytes, vm->alloc_ud);
    if (strand.stack == NULL) {
        vm->last_error = UVM_OOM;
        vm_format_oom(vm, stack_bytes);
        ustrand_destroy(&strand, vm);
        return UVM_OOM;
    }
    urbi_zero(strand.stack, stack_bytes);

    /* T10: initialise the cleanup stack so OP_TRY_BEGIN can push entries.
     * Failure leaves cleanup_base=NULL; OP_TRY_BEGIN detects and halts safely. */
    (void)strand_cleanup_stack_init(&strand, vm, URBI_CLEANUP_MAX);

    /* T33: route this transient onto vm->global_realm->strands_head so the
     * GC realm-hierarchy walker (T32) visits its register window.  Lazy-create
     * the global realm on first use; failure here is non-fatal — the strand
     * stays realm=NULL and the GC walker simply skips it (M3 baseline behavior).
     * The strand stays a stack-local UStrand and is unlinked again before the
     * matching ustrand_destroy below.  Per pre-M4 GC strand-walker spec §5.1.
     * entry_closure stays NULL — that is the discriminator the OP_FORK_DETACH
     * / OP_FORK_JOIN guards now use to reject forks from a uvm_run transient. */
    {
        URealm *gr = urbi_realm_global(vm);
        if (gr != NULL) {
            strand.realm         = gr;
            strand.next_in_realm = gr->strands_head;
            gr->strands_head     = &strand;
        }
    }

    /* Wire frame-0 from module. */
    strand.R          = strand.stack;
    strand.pc         = module->instructions;
    strand.pc_base    = module->instructions;
    strand.cur_consts = module->constants;
    strand.module     = module;
    /* M4 follow-up / T72 fix: always create a fresh UModuleInstance for each
     * uvm_run call.  urbi_get_or_create_module_instance is unsuitable here
     * because the REPL stack-allocates UModule and reuses the same stack
     * address across calls; the cache lookup would return a stale instance
     * with old (freed) ic_names.  Forcing fresh creation ensures ic->name is
     * populated from the current module's ic_names table.
     *
     * urbi_run_chunk pre-creates an instance via get_or_create before calling
     * uvm_run; that cached instance is shadowed by this fresh one (prepended to
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
     * OP_RET) exits before reaching watcher_eval_dirty.  uvm_run re-enters on
     * yield so forward-progress is correct, but watcher_eval_dirty never fires
     * (the exit happens before the hook).  sched_strand_init was previously
     * skipped for transients; calling it here also zero-initialises the
     * scheduler list pointers (already zero from the volatile loop above,
     * so this is idempotent for all fields other than the budget). */
    sched_strand_init(&strand, NULL);

    /* Run to completion: loop until strand is DEAD or a fatal error sets last_error.
       OP_YIELD or per-strand budget exhaustion leaves state READY — treat as
       "continue" for the M2 API contract (uvm_run must block until completion). */
    for (;;) {
        (void)dispatch_loop_until_yield(&strand, /* step_budget */ UINT64_MAX);
        if (strand.state == USTRAND_STATE_DEAD) break;
        if (vm->last_error != UVM_OK) break;
        if (strand.state == USTRAND_STATE_READY) {
            /* OP_YIELD (between separator children) or per-strand budget.
               Remove from ready queue (sched_strand_yield enqueued it),
               reset to RUNNING, and re-enter dispatch. */
            if (vm->ready_head == &strand) {
                vm->ready_head = strand.ready_next;
                if (vm->ready_head != NULL)
                    vm->ready_head->ready_prev = NULL;
                else
                    vm->ready_tail = NULL;
                if (vm->strand_runnable_count > 0)
                    vm->strand_runnable_count--;
                strand.ready_next = NULL;
                strand.ready_prev = NULL;
            }
            strand.state = USTRAND_STATE_RUNNING;
            continue;
        }
        if (USTRAND_IS_WAITING(&strand)) {
            /* M2 baseline has no blocking opcodes; WAITING here is a bug. */
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm, "strand blocked unexpectedly in uvm_run");
            break;
        }
        /* RUNNING with step_budget exhausted (UINT64_MAX → shouldn't happen). */
        break;
    }

    /* Pre-GC: free every closure allocated this run, except the one returned
     * to the caller via *out.  That closure is kept alive in
     * vm->last_return_closure until the next uvm_run() or uvm_destroy(). */
    {
        UClosure *out_cl = (out->kind == (uint8_t)UVAL_CLOSURE)
                           ? (UClosure *)out->v.p : NULL;
        vm->last_return_closure = out_cl;

        UClosure *cl = strand.closure_list;
        strand.closure_list = NULL;  /* null before ustrand_destroy to avoid double-free */
        while (cl != NULL) {
            UClosure *next = cl->next_alloc;
            if (cl != out_cl) {
                vm->alloc_fn(cl, 0, vm->alloc_ud);
            }
            cl = next;
        }
    }

    /* Pre-GC: free every heapified upvalue cell allocated this run. */
    {
        UUpvalCell *cell = strand.closed_cells;
        strand.closed_cells = NULL;  /* null before ustrand_destroy to avoid double-free */
        while (cell != NULL) {
            UUpvalCell *next = cell->next;
            vm->alloc_fn(cell, 0, vm->alloc_ud);
            cell = next;
        }
    }

    /* Free any open upvalue cells still on the strand. */
    vm_free_open_upvalues(vm, &strand);
    strand.open_upvals = NULL;  /* null before ustrand_destroy to avoid double-free */

    /* Free the register stack. */
    vm->alloc_fn(strand.stack, 0, vm->alloc_ud);
    strand.stack = NULL;

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
