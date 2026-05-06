/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi_run_closure_on_scratch — shared scratch-frame closure runner.
 *
 * Spec ref: #2 §6.4 + §7.3 phase 3.  Mirrors uvm_run's transient-strand
 * pattern (src/uvm.c:2112) but scoped to single-closure cond evaluation
 * with bounded dispatch budget and no-yield contract.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * All allocation goes through vm->alloc_fn.
 *
 * Used by:
 *   - run_closure_on_scratch_frame_with_result (install path, uwatcher_install.c)
 *   - invoke_condition_closure                  (eval path, uwatcher_eval.c)
 *   - invoke_body_inline                        (AT_SYNC body, uwatcher_eval.c)
 *   - invoke_onleave_inline                     (falling-edge onleave, uwatcher_eval.c)
 *   - run_watcher_onleave                       (drain onleave, uwatcher_drain.c)
 *
 * Out of scope (follow-up): event sync-emit body (uevent_emit.c).  Can wire
 * to this same helper as a small replacement at its call site.
 *
 * **Limitation:** strand.module is left NULL.  The dispatch loop dereferences
 * s->module in OP_CLOSURE (nested function literal) and in some type-error
 * formatters.  Cond closures with nested function literals are unsupported
 * at v0.5.1; typical conds (x > 5, obj.slot != nil, 1 + 1) don't trip this.
 * Plumbing module through requires a UProto.module back-pointer or wrapping
 * UClosure with the owning module reference — out of scope for this patch. */

#include "watcher/uwatcher.h"
#include "uvm.h"
#include "uclosure.h"   /* UClosure full definition (M4: embeds UCell) */
#include "ustrand.h"
#include "ucleanup.h"
#include "realm/urealm.h"
#include "urbi/urbi.h"
#include "umacros.h"

int
urbi_run_closure_on_scratch(struct UVM      *vm,
                            struct UClosure *closure,
                            UValue          *out_result,
                            int             *out_threw)
{
    UValue nil = {0};   /* kind = UVAL_NIL, payload zeroed */
    UProtoInstanceArr *scratch_arr = NULL; /* heap buf for synthetic module_instance */

    URBI_ASSERT_NOT_ISR(vm);
    URBI_INTERNAL_ASSERT(out_result != NULL);
    URBI_INTERNAL_ASSERT(out_threw  != NULL);

    *out_result = nil;
    *out_threw  = 0;

    /* Reset last_error at entry so a stale error from a prior VM operation
     * doesn't get misread as a cond throw.  Mirrors uvm_run's entry pattern. */
    vm->last_error = UVM_OK;
    vm->last_errmsg[0] = '\0';

    /* NULL closure: graceful nil — matches the prior stub contract for
     * watchers installed without a condition. */
    if (closure == NULL) return 0;

    /* Allocate a transient strand on the C stack.  Mirrors uvm_run. */
    UStrand strand;
    {
        volatile unsigned char *p = (volatile unsigned char *)&strand;
        size_t i;
        for (i = 0; i < sizeof(strand); i++) p[i] = 0;
    }
    strand.vm                   = vm;
    strand.state                = USTRAND_STATE_DORMANT;
    strand.is_uvm_run_transient = 1u;  /* guards reject OP_FORK_DETACH/JOIN */

    /* Arm from the closure: allocates register stack, wires pc / pc_base /
     * cur_consts / frame_count from closure->proto.  Returns -1 on OOM. */
    if (urbi_strand_arm_from_closure(&strand, closure) != 0) {
        if (vm->host_log_fn) {
            vm->host_log_fn(vm, URBI_LOG_WARN,
                "urbi_run_closure_on_scratch: register-stack OOM");
        }
        return -1;
    }

    /* strand.module is intentionally left NULL — see the function docstring
     * for the OP_CLOSURE limitation.  Typical cond closures don't trigger it. */

    /* Synthetic module_instance: OP_GETSLOT at frame_count==0 reads
     * s->module_instance->proto_instances->entries[0].ic_table.  The scratch
     * strand runs closure directly at frame_count==0 (not the root chunk), so
     * entries[0] must expose closure's own IC table.  Allocate a minimal
     * UProtoInstanceArr (one entry) and a stack-local UModuleInstance shell.
     * Freed in teardown below; GC does not chase strand.module_instance. */
    UModuleInstance scratch_mi;
    {
        volatile unsigned char *p = (volatile unsigned char *)&scratch_mi;
        size_t i;
        for (i = 0; i < sizeof(scratch_mi); i++) p[i] = 0;
    }
    if (closure->proto_inst != NULL) {
        size_t arr_bytes = sizeof(UProtoInstanceArr) + sizeof(UProtoInstance);
        scratch_arr = (UProtoInstanceArr *)vm->alloc_fn(NULL, arr_bytes, vm->alloc_ud);
        if (scratch_arr != NULL) {
            {
                volatile unsigned char *p = (volatile unsigned char *)scratch_arr;
                size_t i;
                for (i = 0; i < arr_bytes; i++) p[i] = 0;
            }
            scratch_arr->n = 1;
            scratch_arr->entries[0].proto    = closure->proto;
            scratch_arr->entries[0].ic_table = closure->proto_inst->ic_table;
            scratch_mi.vm               = vm;
            scratch_mi.proto_instances  = scratch_arr;
            strand.module_instance      = &scratch_mi;
        }
    }

    /* Initialise the cleanup stack so OP_TRY_BEGIN can push entries.
     * Failure leaves cleanup_base=NULL; OP_TRY_BEGIN detects and halts safely. */
    (void)strand_cleanup_stack_init(&strand, vm, URBI_CLEANUP_MAX);

    /* Thread onto global_realm->strands_head so the GC walker sees the
     * strand's register window (mirrors uvm_run T33 dance). */
    {
        URealm *gr = urbi_realm_global(vm);
        if (gr != NULL) {
            strand.realm         = gr;
            strand.next_in_realm = gr->strands_head;
            gr->strands_head     = &strand;
        }
    }

    /* out_slot wires OP_RET's top-frame return value into our local. */
    UValue out_local = {0};
    strand.out_slot  = &out_local;
    strand.state     = USTRAND_STATE_RUNNING;

    /* Run with bounded budget.  Cond closures must not yield (spec §6.4),
     * but we cap dispatch ops as a defensive measure. */
    (void)dispatch_loop_until_yield(&strand, URBI_SCRATCH_BUDGET_OPS);

    /* Detect unhandled throw / abnormal exit. */
    if (vm->last_error != UVM_OK) {
        *out_threw = 1;
        vm->last_error = UVM_OK;
        vm->last_errmsg[0] = '\0';
    } else if (strand.state == USTRAND_STATE_DEAD) {
        /* Clean OP_RET — capture the return value. */
        *out_result = out_local;
    } else {
        /* RUNNING with budget exhausted, READY (yield), or WAITING (block).
         * The latter two violate the §6.4 no-yield contract; treat as
         * cond-throw so install/eval can fail-soft. */
        *out_threw = 1;
        if (vm->host_log_fn) {
            vm->host_log_fn(vm, URBI_LOG_WARN,
                "urbi_run_closure_on_scratch: cond exceeded budget or yielded");
        }
    }

    /* Unlink from global_realm before tearing down (symmetric with insert
     * above; mirrors uvm_run's pre-ustrand_destroy unlink). */
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

    /* Teardown sequence: adapted from uvm_run's tail block (unlink reordered to before free) (src/uvm.c:2251-2305).
     * closure_list and closed_cells are nulled before ustrand_destroy to
     * avoid double-free on the same list if ustrand_destroy were to walk them
     * (it doesn't at v1.0, but belt-and-suspenders matches uvm_run). */
    {
        UClosure *cl = strand.closure_list;
        strand.closure_list = NULL;
        while (cl != NULL) {
            UClosure *next = cl->next_alloc;
            vm->alloc_fn(cl, 0, vm->alloc_ud);
            cl = next;
        }
    }
    {
        UUpvalCell *cell = strand.closed_cells;
        strand.closed_cells = NULL;
        while (cell != NULL) {
            UUpvalCell *next = cell->next;
            vm->alloc_fn(cell, 0, vm->alloc_ud);
            cell = next;
        }
    }

    /* Free any open upvalue cells still on the strand (inlined from the
     * static vm_free_open_upvalues helper in uvm.c). */
    {
        UUpvalCell *cell = strand.open_upvals;
        while (cell != NULL) {
            UUpvalCell *next = cell->next;
            vm->alloc_fn(cell, 0, vm->alloc_ud);
            cell = next;
        }
        strand.open_upvals = NULL;
    }

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
