/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode interpreter — dispatch loop (uvm_init.c + uvm_run.c hold lifecycle). */

#include "vm/uvm.h"
#include "runtime/umacros.h"
#include "urbi/urbi.h"    /* URBI_CALLBACK_WARN_US, URBI_WATCHDOG_WARN */
#include "runtime/uclosure.h"     /* UClosure full definition (M4: embeds UCell) */
#include "sched/ustrand.h"
#include "value/uvalue.h"
#include "runtime/uunwind.h"
#include "realm/urealm.h"            /* URealm — OP_LOAD_REALM_GLOBAL */
#include "urbi/gc.h" /* urbi_gc_slice + URBI_GC_SLICE_BUDGET */
#include "tag/utag.h"    /* UTag, utag_create/destroy (T30) */
#include "watcher/uwatcher.h"          /* UWatcher — watcher dispatch (T32) */
#include "watcher/uwatcher_install.h"  /* install_watcher_runtime, install_at_event_runtime (T41-T47) */
#include "event/uevent.h"                    /* UEvent — cast target for OP_AT_EVENT_INSTALL (T47) */
#include "event/uevent_emit.h"               /* c_event_emit_sync — tier-2 tag enter/leave hooks (T55) */
#include "event/uevent_native.h"             /* uvalue_from_event — OP_GETSLOT_CHANGE_EVENT (T61) */
#include "vm/uop_fork.h" /* op_fork_detach/join/wait + fork_wake_joiners (T38) */
#include "vm/uvm_arith.h"    /* arith_add/sub/mul/div/neg + helpers (T16) */
#include "vm/uvm_internal.h" /* diag / closure cross-TU decls (T15) */
#include "object/uic.h"         /* UIC + urbi_slot_get_slow / urbi_slot_set_slow (T22-T25) */
#include "object/uobject.h"     /* UObject — receivers for GETSLOT/SETSLOT (T22-T25) */
#include "changed/uchanged_node.h"          /* urbi_object_get_or_create_change_event (T60) */

/* --- Dispatch macros.
       Under GCC/Clang with computed-goto support (and without
       URBI_VM_FORCE_SWITCH), DISPATCH/CASE/NEXT expand to threaded
       dispatch. Otherwise they expand to switch/case/continue.
       Opcode bodies are written once; both paths use them. --- */

#if !defined(URBI_VM_FORCE_SWITCH) && (defined(__GNUC__) || defined(__clang__))
#  define UVM_USE_COMPUTED_GOTO 1
#else
#  define UVM_USE_COMPUTED_GOTO 0
#endif

#if UVM_USE_COMPUTED_GOTO
#  define DISPATCH()  goto *dispatch_table[uinstr_op(*s->pc)]
#  define CASE(op)    label_##op:
#  define NEXT()      do { s->pc++; DISPATCH(); } while (0)
#  define HALT()      goto halt_error
#else
#  define DISPATCH()  switch (uinstr_op(*s->pc))
#  define CASE(op)    case (op):
#  define NEXT()      do { s->pc++; goto dispatch; } while (0)
#  define HALT()      goto halt_error
#endif

/* Dispatch-time assertion for placeholder opcode stubs.
 * In hosted builds (tests, REPL) this triggers assert() so CI catches
 * stray row-7 opcodes emitted without an emit path.  In freestanding
 * builds it is a no-op — the stub immediately sets strand DEAD, which
 * is safe and produces a VM_TYPE_ERROR diagnostic. */
#if __STDC_HOSTED__
#  include <assert.h>
#  define URBI_DISPATCH_ASSERT(cond) assert(cond)
#else
#  define URBI_DISPATCH_ASSERT(cond) ((void)0)
#endif

/* --- ic_resolve_pi: IC-table proto-instance resolver (VM-008) ---
   Resolves the UProtoInstance* used by OP_GETSLOT and OP_SETSLOT.
   Three-way dispatch:
     frame_count == 0 + entry_closure with proto_inst  → entry_closure->proto_inst
     frame_count == 0 + no entry_closure / no proto_inst → entries[0] of module_instance
     frame_count >  0                                   → frames[top].closure->proto_inst
   Returns NULL when no IC table is reachable (megamorphic-bail; caller must HALT).
   Must inline into the dispatch loop — __attribute__((always_inline)) ensures the
   compiler never emits a call instruction on the hot path. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
static inline UProtoInstance *
ic_resolve_pi(UStrand *s)
{
    if (s->frame_count == 0) {
        if (s->entry_closure != NULL && s->entry_closure->proto_inst != NULL)
            return s->entry_closure->proto_inst;
        if (s->module_instance != NULL
                && s->module_instance->proto_instances != NULL)
            return &s->module_instance->proto_instances->entries[0];
        return NULL;
    }
    UClosure *cur_cl = s->frames[s->frame_count - 1].closure;
    return cur_cl ? cur_cl->proto_inst : NULL;
}

/* --- dispatch_loop_until_yield ---
   The core execution engine (T6).  Runs s's bytecode until one of:
   - strand reaches DEAD (top-level OP_RET or halt_error)
   - strand yields via OP_YIELD (state → READY)
   - step_budget_in opcodes are consumed (state remains RUNNING)
   Returns the number of opcodes consumed.

   The uvm_run() function below is a thin adapter that creates a transient
   UStrand, loops calling this function until DEAD, then tears down.
   All dispatch state lives on the strand; vm holds only VM-wide state. */

uint64_t
dispatch_loop_until_yield(UStrand *s, uint64_t step_budget_in)
{
    UVM *vm = s->vm;
    uint64_t steps_consumed = 0;

    s->state = USTRAND_STATE_RUNNING;
    vm->step_budget_remaining = step_budget_in;

#if UVM_USE_COMPUTED_GOTO
    /* Dispatch table keyed by opcode.  All opcodes populated; loader
       validates opcode is in [0, OP_MAX) before uvm_run is called. */
    static void *dispatch_table[OP_MAX] = {
        [OP_LOADK]      = &&label_OP_LOADK,
        [OP_MOVE]       = &&label_OP_MOVE,
        [OP_ADD]        = &&label_OP_ADD,
        [OP_SUB]        = &&label_OP_SUB,
        [OP_MUL]        = &&label_OP_MUL,
        [OP_DIV]        = &&label_OP_DIV,
        [OP_NEG]        = &&label_OP_NEG,
        [OP_RET]        = &&label_OP_RET,
        [OP_LOADNIL]    = &&label_OP_LOADNIL,
        [OP_LOADBOOL]   = &&label_OP_LOADBOOL,
        [OP_LOADVOID]   = &&label_OP_LOADVOID,
        [OP_GETUPVAL]   = &&label_OP_GETUPVAL,
        [OP_SETUPVAL]   = &&label_OP_SETUPVAL,
        [OP_CLOSURE]    = &&label_OP_CLOSURE,
        [OP_CLOSE]      = &&label_OP_CLOSE,
        [OP_CALL]       = &&label_OP_CALL,
        [OP_JMP]        = &&label_OP_JMP,
        [OP_TEST]       = &&label_OP_TEST,
        [OP_TESTSET]    = &&label_OP_TESTSET,
        [OP_EQ]         = &&label_OP_EQ,
        [OP_NEQ]        = &&label_OP_NEQ,
        [OP_LT]         = &&label_OP_LT,
        [OP_LE]         = &&label_OP_LE,
        [OP_YIELD]      = &&label_OP_YIELD,
        [OP_FORK_DETACH]= &&label_OP_FORK_DETACH,
        [OP_FORK_JOIN]  = &&label_OP_FORK_JOIN,
        [OP_JOIN_WAIT]  = &&label_OP_JOIN_WAIT,
        [OP_GETSLOT]    = &&label_OP_GETSLOT,
        [OP_SETSLOT]    = &&label_OP_SETSLOT,
        /* M3 row 7 control-transfer — T10 wires THROW/TRY_BEGIN/TRY_END/RESUME/LOAD_CATCH_VALUE
         * T11 wires PUSH_TAG/POP_TAG/PUSH_FRAME_GUARD; TAG_STOP stays stub until T31. */
        [OP_THROW]            = &&label_OP_THROW,
        [OP_TAG_STOP]         = &&label_row7_stub,
        [OP_TRY_BEGIN]        = &&label_OP_TRY_BEGIN,
        [OP_TRY_END]          = &&label_OP_TRY_END,
        [OP_PUSH_TAG]         = &&label_OP_PUSH_TAG,
        [OP_POP_TAG]          = &&label_OP_POP_TAG,
        [OP_PUSH_FRAME_GUARD] = &&label_OP_PUSH_FRAME_GUARD,
        [OP_RESUME]           = &&label_OP_RESUME,
        [OP_LOAD_CATCH_VALUE] = &&label_OP_LOAD_CATCH_VALUE,
        /* M4 reserve stub — not yet implemented. */
        [OP_INVOKE]                = &&label_m5_stub,
        /* M5 reactive runtime — T41 wires AT/WHENEVER install opcodes. */
        [OP_AT_INSTALL]            = &&label_OP_AT_INSTALL,
        [OP_AT_SYNC_INSTALL]       = &&label_OP_AT_SYNC_INSTALL,
        [OP_WHENEVER_INSTALL]      = &&label_OP_WHENEVER_INSTALL,
        [OP_WAITUNTIL_INSTALL]     = &&label_OP_WAITUNTIL_INSTALL,
        [OP_AT_EVENT_INSTALL]      = &&label_OP_AT_EVENT_INSTALL,
        [OP_AT_EVENT_SYNC_INSTALL] = &&label_OP_AT_EVENT_SYNC_INSTALL,
        [OP_GETSLOT_CHANGE_EVENT]  = &&label_OP_GETSLOT_CHANGE_EVENT,
        [OP_LOAD_REALM_GLOBAL]     = &&label_OP_LOAD_REALM_GLOBAL,
    };

    DISPATCH();
#else
dispatch:
    DISPATCH() {
#endif

        CASE(OP_LOADK) {
            s->R[uinstr_a(*s->pc)] = s->cur_consts[uinstr_bx(*s->pc)];
            NEXT();
        }

        CASE(OP_MOVE) {
            s->R[uinstr_a(*s->pc)] = s->R[uinstr_b(*s->pc)];
            NEXT();
        }

        CASE(OP_ADD) {
            UValue *a = &s->R[uinstr_a(*s->pc)];
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *cc = &s->R[uinstr_c(*s->pc)];
            UVMError rc = arith_add(a, b, cc);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm_format_type_error_binary(vm, s->module,
                    (size_t)(s->pc - s->pc_base),
                    OP_ADD, b->kind, cc->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_SUB) {
            UValue *a = &s->R[uinstr_a(*s->pc)];
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *cc = &s->R[uinstr_c(*s->pc)];
            UVMError rc = arith_sub(a, b, cc);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm_format_type_error_binary(vm, s->module,
                    (size_t)(s->pc - s->pc_base),
                    OP_SUB, b->kind, cc->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_MUL) {
            UValue *a = &s->R[uinstr_a(*s->pc)];
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *cc = &s->R[uinstr_c(*s->pc)];
            UVMError rc = arith_mul(a, b, cc);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm_format_type_error_binary(vm, s->module,
                    (size_t)(s->pc - s->pc_base),
                    OP_MUL, b->kind, cc->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_DIV) {
            UValue *a = &s->R[uinstr_a(*s->pc)];
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *cc = &s->R[uinstr_c(*s->pc)];
            UVMError rc = arith_div(a, b, cc);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm_format_type_error_binary(vm, s->module,
                    (size_t)(s->pc - s->pc_base),
                    OP_DIV, b->kind, cc->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_NEG) {
            UValue *a = &s->R[uinstr_a(*s->pc)];
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            UVMError rc = arith_neg(a, b);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm_format_type_error_unary(vm, s->module,
                    (size_t)(s->pc - s->pc_base),
                    OP_NEG, b->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_RET) {
            UValue retval = s->R[uinstr_a(*s->pc)];

            if (s->frame_count == 0) {
                /* Top-frame return — strand becomes DEAD.
                 * Adapter (uvm_run) extracts result via out_slot. */
                if (s->out_slot != NULL) {
                    *s->out_slot = retval;
                }
                s->state = USTRAND_STATE_DEAD;
                steps_consumed++;
                goto exit_strand;
            }

            /* Non-top-frame: hand off to walker.
             * M2's inline pop+deliver is now urbi_unwind()'s job (T8 bridging
             * stub; T9 replaces with the real 5-kind walker). */
            s->unwind_value   = retval;
            s->pending_unwind = UEXEC_RETURN;
            steps_consumed++;
            goto safepoint;
        }

        CASE(OP_LOADNIL) {
            s->R[uinstr_a(*s->pc)].kind = (uint8_t)UVAL_NIL;
            NEXT();
        }

        CASE(OP_LOADBOOL) {
            s->R[uinstr_a(*s->pc)].kind  = (uint8_t)UVAL_BOOL;
            s->R[uinstr_a(*s->pc)].v.i   = uinstr_b(*s->pc) != 0 ? 1 : 0;
            if (uinstr_c(*s->pc)) { s->pc++; }
            NEXT();
        }

        CASE(OP_LOADVOID) {
            s->R[uinstr_a(*s->pc)].kind = (uint8_t)UVAL_VOID;
            NEXT();
        }

        CASE(OP_GETUPVAL) {
            /* ABC: R[A] := upvalue[B] from the current frame's closure.
             * At frame_count == 0 (top-level strand including fork-spawned
             * children) fall back to s->entry_closure so that closures
             * created by emit_lazy_thunk can read their captured upvalues. */
            UClosure *cur_cl = (s->frame_count > 0)
                             ? s->frames[s->frame_count - 1].closure
                             : s->entry_closure;
            if (cur_cl == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "GETUPVAL: no closure in current frame");
                HALT();
            }
            {
                uint8_t b = uinstr_b(*s->pc);
                UUpvalCell *uvc = cur_cl->upvals[b];
                s->R[uinstr_a(*s->pc)] = uvc->on_heap ? uvc->u.value
                                                      : *uvc->u.stack_ptr;
            }
            NEXT();
        }

        CASE(OP_SETUPVAL) {
            /* ABC: upvalue[B] := R[A] for the current frame's closure.
             * At frame_count == 0 fall back to s->entry_closure (same
             * rationale as OP_GETUPVAL). */
            UClosure *cur_cl = (s->frame_count > 0)
                             ? s->frames[s->frame_count - 1].closure
                             : s->entry_closure;
            if (cur_cl == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "SETUPVAL: no closure in current frame");
                HALT();
            }
            {
                uint8_t a = uinstr_a(*s->pc);
                uint8_t b = uinstr_b(*s->pc);
                UUpvalCell *uvc = cur_cl->upvals[b];
                /* GC barrier (M4): UClosure now embeds UCell at offset 0,
                 * so urbi_gc_upvalue_write may safely cast UClosure* → UCell*
                 * for the color check.  Hook fires before the actual store. */
                urbi_gc_upvalue_write(vm, cur_cl, b, s->R[a]);
                if (uvc->on_heap) {
                    uvc->u.value = s->R[a];
                } else {
                    *uvc->u.stack_ptr = s->R[a];
                }
            }
            NEXT();
        }

        CASE(OP_CLOSURE) {
            /* ABx: R[A] := new closure from module->nested[Bx].
             * Reads nupvals pseudo-instructions (OP_MOVE-encoded) immediately
             * after, each specifying (B=in_stack, C=src_idx). */
            uint8_t  a  = uinstr_a(*s->pc);
            uint16_t bx = uinstr_bx(*s->pc);
            if ((size_t)bx >= s->module->nested_count) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "CLOSURE: proto index out of range");
                HALT();
            }
            UProto *child_proto = s->module->nested[bx];
            UClosure *cl = vm_alloc_closure(vm, child_proto, &s->closure_list);
            if (cl == NULL) {
                vm->last_error = UVM_OOM;
                vm_format_oom(vm, sizeof(UClosure));
                HALT();
            }
            /* M4 follow-up: bind proto_inst so the new closure can dispatch
             * OP_GETSLOT/OP_SETSLOT against the per-VM IC table.  entries[0]
             * is the root chunk; entries[bx + 1] is the matching nested proto. */
            if (s->module_instance != NULL
                && s->module_instance->proto_instances != NULL
                && (size_t)bx + 1u < (size_t)s->module_instance->proto_instances->n) {
                cl->proto_inst = &s->module_instance->proto_instances->entries[bx + 1u];
            }
            /* If no module_instance is bound (defensive — uvm_run wires it for
             * every normal execution path), proto_inst stays NULL and
             * OP_GETSLOT/SETSLOT will diagnose cleanly. */

            /* Read nupvals pseudo-instructions. */
            {
                int i;
                for (i = 0; i < (int)child_proto->nupvals; i++) {
                    s->pc++;
                    uint8_t in_stack = uinstr_b(*s->pc);
                    uint8_t src_idx  = uinstr_c(*s->pc);
                    if (in_stack) {
                        UUpvalCell *uvc = vm_open_upvalue(vm, s, &s->R[src_idx]);
                        if (uvc == NULL) {
                            vm->alloc_fn(cl, 0, vm->alloc_ud);
                            vm->last_error = UVM_OOM;
                            vm_format_oom(vm, sizeof(UUpvalCell));
                            HALT();
                        }
                        cl->upvals[i] = uvc;
                    } else {
                        /* Re-capture: copy parent closure's upvalue pointer.
                         * Fall back to entry_closure at frame_count == 0 for
                         * fork-spawned child strands (same as GETUPVAL). */
                        UClosure *par_cl = (s->frame_count > 0)
                                         ? s->frames[s->frame_count - 1].closure
                                         : s->entry_closure;
                        if (par_cl == NULL || src_idx >= par_cl->nupvals) {
                            vm->alloc_fn(cl, 0, vm->alloc_ud);
                            vm->last_error = UVM_TYPE_ERROR;
                            vm_format_type_error_msg(vm, "CLOSURE: upvalue re-capture out of range");
                            HALT();
                        }
                        cl->upvals[i] = par_cl->upvals[src_idx];
                    }
                }
            }
            s->R[a].kind  = (uint8_t)UVAL_CLOSURE;
            s->R[a].v.p   = cl;
            NEXT();
        }

        CASE(OP_CLOSE) {
            /* ABC: heapify all open upvalue cells at R >= R[A]. */
            vm_close_upvalues(s, &s->R[uinstr_a(*s->pc)], &s->closed_cells);
            NEXT();
        }

        CASE(OP_CALL) {
            /* ABC: R[A](R[A+1]..R[A+B-1]); B-1 = nargs, C unused at T14.
             * After the call, the result overwrites R[A]. */
            uint8_t a = uinstr_a(*s->pc);
            uint8_t b = uinstr_b(*s->pc);
            int nargs = (int)b - 1;

            if (s->R[a].kind != (uint8_t)UVAL_CLOSURE) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "CALL: callee is not a closure");
                HALT();
            }
            UClosure *callee = (UClosure *)s->R[a].v.p;
            if (nargs != (int)callee->proto->nparams) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "CALL: wrong argument count");
                HALT();
            }
            if (s->frame_count >= UVM_MAX_FRAMES) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "CALL: call stack overflow");
                HALT();
            }

            /* Check stack space: callee's frame starts at R[a+1]. */
            if ((s->R + a + 1 + callee->proto->max_reg + 1) > (s->stack + UVM_STACK_CAP)) {
                vm->last_error = UVM_OOM;
                vm_format_oom(vm, (size_t)(callee->proto->max_reg + 1) * sizeof(UValue));
                HALT();
            }

            /* Push a new frame record.  This frame record stores what to restore
             * on OP_RET, plus the callee's closure (for GETUPVAL during the call). */
            UCallFrame *new_frame = &s->frames[s->frame_count++];
            new_frame->closure         = callee;
            new_frame->proto           = callee->proto;
            new_frame->pc              = s->pc;    /* points AT OP_CALL in caller */
            new_frame->base            = s->R;     /* caller's register base */
            new_frame->result_dest_reg = (int)a;  /* where to write result */

            /* Switch to callee frame. Args R[a+1..a+nargs] become R[0..nargs-1]. */
            s->R        = &s->R[a + 1];
            s->pc       = callee->proto->instructions;
            s->pc_base  = s->pc;
            s->cur_consts = callee->proto->constants ? callee->proto->constants
                                                     : s->module->constants;

            /* Zero registers beyond nparams up to max_reg. */
            {
                int si;
                for (si = nargs; si <= (int)callee->proto->max_reg; si++) {
                    UValue z = {0};
                    s->R[si] = z;
                }
            }

            /* Safepoint at call-frame-push. */
            steps_consumed++;
            goto safepoint;
        }

        CASE(OP_JMP) {
            /* ABx: pc += signed(Bx) - 32768.  Offset is applied after the
             * normal pc++ in NEXT, so we pre-adjust by (offset - 1). */
            int offset = (int)uinstr_bx(*s->pc) - 32768;
            s->pc += offset;
            /* Safepoint at backward branch (prevents infinite loop starvation). */
            if (offset < 0) {
                steps_consumed++;
                goto safepoint;
            }
            NEXT();
        }

        CASE(OP_TEST) {
            /* ABC: if (truthy(R[A]) == C) pc++ (skip next instr) */
            const UValue *a = &s->R[uinstr_a(*s->pc)];
            bool truthy = uvalue_truthy(a);
            if ((int)truthy == (int)uinstr_c(*s->pc)) { s->pc++; }
            NEXT();
        }

        CASE(OP_TESTSET) {
            /* ABC: if (truthy(R[B]) == C) pc++ else R[A] := R[B] */
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            bool truthy = uvalue_truthy(b);
            if ((int)truthy == (int)uinstr_c(*s->pc)) {
                s->pc++;
            } else {
                s->R[uinstr_a(*s->pc)] = *b;
            }
            NEXT();
        }

        CASE(OP_EQ) {
            /* ABC: if ((R[B]==R[C]) != A) pc++ */
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *c = &s->R[uinstr_c(*s->pc)];
            bool eq = uvalue_equal(b, c);
            if ((int)eq != (int)uinstr_a(*s->pc)) { s->pc++; }
            NEXT();
        }

        CASE(OP_NEQ) {
            /* ABC: if ((R[B]!=R[C]) != A) pc++ */
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *c = &s->R[uinstr_c(*s->pc)];
            bool neq = !uvalue_equal(b, c);
            if ((int)neq != (int)uinstr_a(*s->pc)) { s->pc++; }
            NEXT();
        }

        CASE(OP_LT) {
            /* ABC: if ((R[B]<R[C]) != A) pc++ — numeric only at M2 */
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *c = &s->R[uinstr_c(*s->pc)];
            bool lt = false;
            if (uvalue_lt(b, c, &lt) != UVAL_CMP_OK) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_binary(vm, s->module,
                    (size_t)(s->pc - s->pc_base), OP_LT, b->kind, c->kind);
                HALT();
            }
            if ((int)lt != (int)uinstr_a(*s->pc)) { s->pc++; }
            NEXT();
        }

        CASE(OP_LE) {
            /* ABC: if ((R[B]<=R[C]) != A) pc++ — numeric only at M2 */
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *c = &s->R[uinstr_c(*s->pc)];
            bool le = false;
            if (uvalue_le(b, c, &le) != UVAL_CMP_OK) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_binary(vm, s->module,
                    (size_t)(s->pc - s->pc_base), OP_LE, b->kind, c->kind);
                HALT();
            }
            if ((int)le != (int)uinstr_a(*s->pc)) { s->pc++; }
            NEXT();
        }

        CASE(OP_YIELD) {
            /* Cooperative yield: advance past this opcode, transition to READY,
               and return to the scheduler.  The uvm_run adapter re-enters
               dispatch_loop_until_yield until strand is DEAD. */
            s->pc++;
            s->state = USTRAND_STATE_READY;
            sched_strand_yield(s);
            steps_consumed++;
            goto exit_strand;
        }

        CASE(OP_FORK_DETACH) {
            /* `,` separator: spawn child closure as detached strand.
             * A = closure_reg.  Parent continues; child runs concurrently.
             * See src/uop_fork.c for M3 closure-spawn vs. spec §7.1 rationale.
             * Rejected from uvm_run's stack-local transient because that
             * adapter only dispatches its own strand and would leak any
             * spawned children.  T33 routes the transient onto
             * vm->global_realm->strands_head for GC-walker visibility, so
             * realm == NULL no longer discriminates; the dedicated flag
             * is_transient_strand does. */
            if (s->is_transient_strand) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "OP_FORK_DETACH: `,` requires urbi_step driver (uvm_run transient strand)");
                HALT();
            }
            int rc = op_fork_detach(s, vm, *s->pc);
            if (rc != 0) goto exit_strand;
            NEXT();
        }

        CASE(OP_FORK_JOIN) {
            /* `&` separator LHS: spawn child closure, store handle in R[B].
             * A = closure_reg, B = child_handle_reg.
             * Same uvm_run-transient guard as OP_FORK_DETACH; see note above. */
            if (s->is_transient_strand) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "OP_FORK_JOIN: `&` requires urbi_step driver (uvm_run transient strand)");
                HALT();
            }
            int rc = op_fork_join(s, vm, *s->pc);
            if (rc != 0) goto exit_strand;
            NEXT();
        }

        CASE(OP_JOIN_WAIT) {
            /* `&` separator join-point: block until child handle in R[A] is DEAD.
             * A = child_handle_reg. */
            int rc = op_join_wait(s, vm, *s->pc);
            if (rc < 0) goto exit_strand;   /* OOM or error */
            if (rc > 0) {
                /* Blocked — parent threaded onto child->joiners_head. */
                steps_consumed++;
                goto exit_strand;
            }
            /* rc == 0: child already DEAD, continue. */
            NEXT();
        }

        CASE(OP_GETSLOT) {
            /* OP_GETSLOT ABC: R[A] := R[B].slot[ic_index].
             *   A = dst_reg, B = recv_reg, C = ic_index.
             *
             * Fast path: linear scan of the IC entries for a (recv->shape,
             * vm->topology_gen) match.  On hit, copy *slots[k] into R[A]
             * (or, if FLAG_OGET set, dispatch the getter — currently raises
             * a diagnostic; full getter dispatch lands when the frame-push
             * wrapper API matures, see TODO below).
             *
             * Slow path: urbi_slot_get_slow walks the prototype chain,
             * fills exactly one IC entry at ic->replace_cursor, and either
             * copies the slot value to *out (no OGET) or signals OGET-flag
             * present (caller would dispatch). */
            uint32_t i = *s->pc;
            uint8_t  dst_reg  = uinstr_a(i);
            uint8_t  recv_reg = uinstr_b(i);
            uint8_t  ic_index = uinstr_c(i);

            /* Resolve IC table (ic_resolve_pi, VM-008). */
            UProtoInstance *pi = ic_resolve_pi(s);
            if (pi == NULL || pi->ic_table == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "GETSLOT: no IC table bound");
                HALT();
            }
            UIC *ic = &pi->ic_table[ic_index];

            if (s->R[recv_reg].kind != (uint8_t)UVAL_OBJECT) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "GETSLOT: receiver is not an Object");
                HALT();
            }
            UObject *recv = (UObject *)s->R[recv_reg].v.p;

            /* Trace probe (spec #2 §7.3 phase 2+3): when watcher install is
             * tracing reads, record the receiver's GC cell.
             * UNLIKELY: this branch is taken only during install-time cond eval
             * — never on the normal hot path.  Zero overhead when bit is clear. */
            if (UNLIKELY(vm->in_watcher_install)) {
                UCell *cell = (UCell *)recv;
                bool already_present = false;
                size_t _ti;
                for (_ti = 0; _ti < (size_t)vm->trace_read_set_count; _ti++) {
                    if (vm->trace_read_set[_ti] == cell) {
                        already_present = true;
                        break;
                    }
                }
                if (!already_present) {
                    if ((size_t)vm->trace_read_set_count < (size_t)URBI_WATCHER_READSET_MAX) {
                        vm->trace_read_set[vm->trace_read_set_count++] = cell;
                    } else {
                        vm->trace_overflow = 1;
                    }
                }
            }

            /* Fast path: linear scan over ic->n entries. */
            for (uint8_t k = 0; k < ic->n; k++) {
                if (ic->recv_shapes[k]  == recv->shape
                 && ic->topology_gen[k] == vm->topology_gen) {
                    if (ic->flags[k] & URBI_SLOT_FLAG_OGET) {
                        /* TODO(T39+): wire URBI_VM_DISPATCH_GETTER once the
                         * frame-push wrapper for receiver+0-arg invocation
                         * is defined.  Until then, getters are rejected at
                         * dispatch with a clean diagnostic; corpus revival
                         * fixtures (T42) exercise non-getter slot reads.
                         * The IC entry itself was filled correctly by the
                         * slow path on first install, so the diagnostic is
                         * a runtime-only restriction, not a missing IC. */
                        vm->last_error = UVM_TYPE_ERROR;
                        vm_format_type_error_msg(vm, "GETSLOT: getter dispatch not yet implemented");
                        HALT();
                    }
                    s->R[dst_reg] = *ic->slots[k];
                    NEXT();
                }
            }

            /* Slow path. */
            UValue v;
            int rc = urbi_slot_get_slow(vm, recv, ic, &v);
            if (rc != 0) {
                vm->last_error = UVM_TYPE_ERROR;
                {
                    UDiagWriter _w;
                    diag_init(&_w, vm->last_errmsg, UVM_ERRMSG_CAP);
                    diag_write_cstr(&_w, "TypeError: GETSLOT: slot '");
                    if (ic->name != NULL)
                        diag_write_cstr(&_w, (const char *)ic->name);
                    diag_write_cstr(&_w, "' not found");
                }
                HALT();
            }
            /* Inspect the just-filled IC entry to decide if a getter is
             * pending.  Same TODO as above — diagnose for now. */
            uint8_t fresh_k = (uint8_t)((ic->replace_cursor + URBI_IC_ENTRIES_PER_SITE - 1u)
                                        % URBI_IC_ENTRIES_PER_SITE);
            if (ic->n > 0u && (ic->flags[fresh_k] & URBI_SLOT_FLAG_OGET)) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "GETSLOT: getter dispatch not yet implemented");
                HALT();
            }
            s->R[dst_reg] = v;
            NEXT();
        }

        CASE(OP_SETSLOT) {
            /* OP_SETSLOT ABC: R[B].slot[ic_index] := R[A].
             *   A = src_reg, B = recv_reg, C = ic_index.
             *
             * Fast path: scan IC entries; on shape+topology match, dispatch
             * setter (FLAG_OSET — currently diagnoses), reject CONSTANT, or
             * write in place if FLAG_LOCAL.  A proto-chain hit (no LOCAL,
             * no OSET) breaks out of the fast path so the slow path can do
             * COW.
             *
             * Slow path: urbi_slot_set_slow walks the prototype chain and
             * either installs a fresh local slot on recv (miss / COW) or
             * fills the IC and writes through (local hit / setter pending). */
            uint32_t i = *s->pc;
            uint8_t  src_reg  = uinstr_a(i);
            uint8_t  recv_reg = uinstr_b(i);
            uint8_t  ic_index = uinstr_c(i);

            /* Resolve IC table (ic_resolve_pi, VM-008). */
            UProtoInstance *pi = ic_resolve_pi(s);
            if (pi == NULL || pi->ic_table == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "SETSLOT: no IC table bound");
                HALT();
            }
            UIC *ic = &pi->ic_table[ic_index];

            if (s->R[recv_reg].kind != (uint8_t)UVAL_OBJECT) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "SETSLOT: receiver is not an Object");
                HALT();
            }
            UObject *recv = (UObject *)s->R[recv_reg].v.p;
            UValue v = s->R[src_reg];

            int slow_path = 1;
            for (uint8_t k = 0; k < ic->n; k++) {
                if (ic->recv_shapes[k]  == recv->shape
                 && ic->topology_gen[k] == vm->topology_gen) {
                    if (ic->flags[k] & URBI_SLOT_FLAG_OSET) {
                        /* TODO(T39+): wire URBI_VM_DISPATCH_SETTER. */
                        vm->last_error = UVM_TYPE_ERROR;
                        vm_format_type_error_msg(vm, "SETSLOT: setter dispatch not yet implemented");
                        HALT();
                    }
                    if (ic->flags[k] & URBI_SLOT_FLAG_CONSTANT) {
                        vm->last_error = UVM_TYPE_ERROR;
                        {
                            UDiagWriter _w;
                            diag_init(&_w, vm->last_errmsg, UVM_ERRMSG_CAP);
                            diag_write_cstr(&_w, "TypeError: SETSLOT: cannot write to constant slot '");
                            if (ic->name != NULL)
                                diag_write_cstr(&_w, (const char *)ic->name);
                            diag_write_cstr(&_w, "'");
                        }
                        HALT();
                    }
                    if (ic->flags[k] & URBI_SLOT_FLAG_LOCAL) {
                        /* Direct in-place write.  Forward Dijkstra barrier
                         * fires on the parent UObject (the cell containing
                         * ic->slots[k]).  Cast UCell* via the pinned
                         * UObject layout: the slot pointer must be inside
                         * recv->slots[], so recv (which embeds UCell at
                         * offset 0) is the parent cell. */
                        urbi_gc_slot_write(vm, (UCell *)recv,
                                           (uint32_t)((ic->slots[k] - recv->slots)),
                                           v);
                        *ic->slots[k] = v;
                        urbi_emit_slot_change_if_subscribed(vm, recv, ic->name, v);
                        slow_path = 0;
                        break;
                    }
                    /* Proto-chain hit (no LOCAL, no OSET, not CONSTANT) →
                     * fall to slow path for COW. */
                    break;
                }
            }
            if (!slow_path) {
                NEXT();
            }

            /* Slow path. */
            int rc = urbi_slot_set_slow(vm, recv, ic, v);
            if (rc != 0) {
                vm->last_error = UVM_TYPE_ERROR;
                {
                    UDiagWriter _w;
                    diag_init(&_w, vm->last_errmsg, UVM_ERRMSG_CAP);
                    diag_write_cstr(&_w, "TypeError: SETSLOT: slot write failed for '");
                    if (ic->name != NULL)
                        diag_write_cstr(&_w, (const char *)ic->name);
                    diag_write_cstr(&_w, "' (constant, OOM, or resolve overflow)");
                }
                HALT();
            }
            uint8_t fresh_k = (uint8_t)((ic->replace_cursor + URBI_IC_ENTRIES_PER_SITE - 1u)
                                        % URBI_IC_ENTRIES_PER_SITE);
            if (ic->n > 0u && (ic->flags[fresh_k] & URBI_SLOT_FLAG_OSET)) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "SETSLOT: setter dispatch not yet implemented");
                HALT();
            }
            /* Fire the write barrier on the slow path so watchers whose
             * read-set includes recv see the write.  Mirrors the fast-path
             * urbi_gc_slot_write call above (line 1571).  The actual store
             * was already performed inside urbi_slot_set_slow; calling the
             * barrier after the store is correct because observer_dirty only
             * bumps watcher_dirty_count and watcher_eval_dirty runs at the
             * next safepoint, not inline here.  Slot index 0 is passed as a
             * conservative sentinel — observer_dirty ignores the key at M5. */
            urbi_gc_slot_write(vm, (UCell *)recv, 0u, v);
            urbi_emit_slot_change_if_subscribed(vm, recv, ic->name, v);
            NEXT();
        }

        /* --- M3 row 7 control-transfer opcodes (T10 real dispatch) --- */

        CASE(OP_THROW) {
            /* OP_THROW ABx: A = reg_value, Bx = 0 (unused).
             * Set pending_unwind = UEXEC_THROW and unwind_value = R[A],
             * then go to safepoint where urbi_unwind() will walk the
             * cleanup stack. */
            uint8_t a = uinstr_a(*s->pc);
            s->unwind_value   = s->R[a];
            s->pending_unwind = UEXEC_THROW;
            s->pc++;
            goto safepoint;
        }

        CASE(OP_TRY_BEGIN) {
            /* OP_TRY_BEGIN ABx: A = flags, Bx = handler_pc.
             * Push a UCLEANUP_TRY_FRAME entry onto the cleanup stack. */
            uint8_t  flags      = uinstr_a(*s->pc);
            uint16_t handler_pc = uinstr_bx(*s->pc);
            UCleanupEntry *entry = strand_cleanup_push(s);
            if (entry == NULL) {
                /* Cleanup stack full — strand fatal. */
                s->fatal_status = UEXEC_THROW;
                s->state        = USTRAND_STATE_DEAD;
                goto exit_strand;
            }
            entry->kind           = (uint8_t)UCLEANUP_TRY_FRAME;
            entry->flags          = flags;
            entry->handler_pc     = handler_pc;
            entry->register_base  = 0u;
            entry->register_count = 0u;
            entry->owning_tag     = NULL;
            entry->catch_pattern  = NULL;
            entry->next_member    = NULL;
            entry->strand_back    = NULL;
            NEXT();
        }

        CASE(OP_TRY_END) {
            /* OP_TRY_END ABC: no operands.  Pop the top UCLEANUP_TRY_FRAME entry. */
            if (s->cleanup_depth > 0) {
                strand_cleanup_pop(s, UCLEANUP_TRY_FRAME);
            }
            NEXT();
        }

        CASE(OP_RESUME) {
            /* OP_RESUME: end of a finally/cleanup body.
             * Exits dispatch_loop_until_yield so run_cleanup_with_replace()
             * can check pending_unwind and restore the saved unwind state.
             * State stays RUNNING; caller (run_cleanup_with_replace) handles
             * the transition. */
            s->pc++;
            goto exit_strand;
        }

        CASE(OP_LOAD_CATCH_VALUE) {
            /* OP_LOAD_CATCH_VALUE ABC: A = destination register, B = C = 0.
             * Load s->catch_value (written by urbi_unwind on catch absorption)
             * into R[A].  First instruction of every catch handler body. */
            s->R[uinstr_a(*s->pc)] = s->catch_value;
            NEXT();
        }

        CASE(OP_PUSH_TAG) {
            /* OP_PUSH_TAG ABx:
             *   A[7:4] = flags nibble (0 at M3 — no FLAG_HAS_ONLEAVE)
             *   A[3:0] = tag_reg nibble (register holding the tag value)
             *   Bx     = onleave_pc (handler PC; 0 at M3 since no onleave body)
             *
             * T30: allocate a per-scope UTag (no UVAL_TAG / register binding at M3).
             * Each tag-scope gets its own anonymous UTag; the tag's lifetime is
             * bounded by the corresponding OP_POP_TAG.
             * Walker-pop (urbi_unwind via OP_THROW etc.) will leak the UTag at M3 —
             * deferred for T31/walker integration when full tag lifecycle wires through.
             * strand_back = s for future tag.stop() walk (T31 uses). */
            uint8_t  a          = uinstr_a(*s->pc);
            uint8_t  flags      = (uint8_t)((a >> 4) & 0xFu);
            uint16_t handler_pc = uinstr_bx(*s->pc);
            UTag *tag = utag_create(s->vm);
            if (tag == NULL) {
                s->fatal_status = UEXEC_THROW;
                s->state        = USTRAND_STATE_DEAD;
                goto exit_strand;
            }
            UCleanupEntry *entry = strand_cleanup_push(s);
            if (entry == NULL) {
                utag_destroy(s->vm, tag);  /* roll back the tag alloc on overflow */
                s->fatal_status = UEXEC_THROW;
                s->state        = USTRAND_STATE_DEAD;
                goto exit_strand;
            }
            entry->kind           = (uint8_t)UCLEANUP_TAG_SCOPE;
            entry->flags          = flags;
            entry->handler_pc     = handler_pc;
            entry->register_base  = 0u;
            entry->register_count = 0u;
            entry->owning_tag     = tag;
            entry->catch_pattern  = NULL;
            entry->next_member    = tag->member_strands_head;  /* head-insert */
            entry->strand_back    = s;
            tag->member_strands_head = entry;
            /* T55: tier-2 enter event hook (spec #3 §8.3).
             * Fast-path: two loads + branch when no subscribers (typical case).
             * Zero alloc. Subscribers see the tag already ambient (entry pushed above). */
            if (tag->enter_event != NULL && tag->enter_event->at_watchers_head != NULL) {
                UValue nil_val = {0};
                nil_val.kind = (uint8_t)UVAL_NIL;
                c_event_emit_sync(s->vm, tag->enter_event, nil_val);
            }
            NEXT();
        }

        CASE(OP_POP_TAG) {
            /* OP_POP_TAG ABC: A = tag_reg (unused at M3), B = C = 0.
             * Pop the top UCLEANUP_TAG_SCOPE entry.
             * If FLAG_HAS_ONLEAVE is set in the entry's flags, the onleave
             * handler would run via run_cleanup_with_replace — but at M3
             * flags is always 0 (no onleave body is emitted), so the handler
             * branch is dead code.  Include the check for forward-compatibility. */
            if (s->cleanup_depth > 0) {
                UCleanupEntry *top = &s->cleanup_base[s->cleanup_depth - 1];
                if ((top->flags & FLAG_HAS_ONLEAVE) != 0u) {
                    /* onleave handler: not reachable at M3 (emit always sets flags=0).
                     * If somehow reached (bytecode corruption), halt safely. */
                    vm->last_error = UVM_TYPE_ERROR;
                    vm_format_type_error_msg(vm, "POP_TAG: FLAG_HAS_ONLEAVE not wired at M3");
                    HALT();
                }
                /* T30: capture owning_tag before pop — the slot remains valid memory but
                 * is below cleanup_depth after pop and may be reused by a later push. */
                UTag *tag = top->owning_tag;
                /* Unlink this entry from tag->member_strands_head (singly-linked
                 * list removal via next_member). Only unlink when tag is non-NULL
                 * — older bytecode emitted before T30 may have owning_tag == NULL. */
                if (tag != NULL) {
                    UCleanupEntry **pp = &tag->member_strands_head;
                    while (*pp != NULL && *pp != top) {
                        pp = &(*pp)->next_member;
                    }
                    if (*pp == top) {
                        *pp = top->next_member;
                    }
                }
                /* T55: tier-2 leave event hook (spec #3 §8.3).
                 * Fires BEFORE the tier-1 watcher cascade so subscribers see the
                 * tag still ambient (spec ordering rationale: tier-1 onleave runs last). */
                if (tag != NULL && tag->leave_event != NULL &&
                    tag->leave_event->at_watchers_head != NULL) {
                    UValue nil_val = {0};
                    nil_val.kind = (uint8_t)UVAL_NIL;
                    c_event_emit_sync(s->vm, tag->leave_event, nil_val);
                }
                /* Watcher cascade: push each watcher registered on this tag to
                 * the pending-onleave queue before cleanup_pop + utag_destroy.
                 * Snapshot-next iteration since push mutates member_watchers_head
                 * (unlinks the watcher from the tag's member list).
                 * Ordering: cascade BEFORE utag_destroy, which asserts the member
                 * list is empty — push empties it. */
                if (tag != NULL) {
                    UWatcher *ww = tag->member_watchers_head;
                    UWatcher *ww_next;
                    while (ww != NULL) {
                        ww_next = ww->next_in_tag;
                        pending_onleave_queue_push(s->vm, ww);
                        ww = ww_next;
                    }
                }
                strand_cleanup_pop(s, UCLEANUP_TAG_SCOPE);
                /* Destroy the per-scope UTag allocated in OP_PUSH_TAG.
                 * Precondition (checked by utag_destroy assertion): member lists
                 * must be empty — we just unlinked the only member above. */
                if (tag != NULL) {
                    utag_destroy(s->vm, tag);
                }
            }
            NEXT();
        }

        CASE(OP_PUSH_FRAME_GUARD) {
            /* OP_PUSH_FRAME_GUARD ABC: A = register_base, B = register_count, C = 0.
             * Push a UCLEANUP_CALL_FRAME entry onto the cleanup stack.
             * The T9 unwind walker absorbs UEXEC_RETURN at CALL_FRAME entries,
             * delivering the return value and popping the frame.
             * strand_back = s for compatibility with unwind walker. */
            uint8_t register_base  = uinstr_a(*s->pc);
            uint8_t register_count = uinstr_b(*s->pc);
            UCleanupEntry *entry = strand_cleanup_push(s);
            if (entry == NULL) {
                s->fatal_status = UEXEC_THROW;
                s->state        = USTRAND_STATE_DEAD;
                goto exit_strand;
            }
            entry->kind           = (uint8_t)UCLEANUP_CALL_FRAME;
            entry->flags          = 0u;
            entry->handler_pc     = 0u;
            entry->register_base  = register_base;
            entry->register_count = register_count;
            entry->owning_tag     = NULL;
            entry->catch_pattern  = NULL;
            entry->next_member    = NULL;
            entry->strand_back    = s;
            NEXT();
        }

        /* OP_TAG_STOP stays as a stub — T31 (urbi_tag_stop) wires the runtime.
         * No syntax emits this opcode yet at T11. */
#if UVM_USE_COMPUTED_GOTO
        label_row7_stub:
#else
        case OP_TAG_STOP:
#endif
        {
            URBI_DISPATCH_ASSERT(0 && "OP_TAG_STOP runtime owned by T31");
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm, "OP_TAG_STOP: not yet implemented (T31)");
            HALT();
        }

        /* === T41: OP_AT_INSTALL / OP_AT_SYNC_INSTALL / OP_WHENEVER_INSTALL ===
         *
         * ABC-encoded: A = cond_reg, B = body_reg, C = onleave_reg (0xFF = absent).
         * Routes through install_watcher_runtime with the appropriate UWATCHER_*
         * mode.  On return the watcher is installed and the strand continues to the
         * next instruction — at-watchers do not block the installing strand.
         * Spec #2 §6.3. */
        CASE(OP_AT_INSTALL) {
            uint8_t A = uinstr_a(*s->pc);
            uint8_t B = uinstr_b(*s->pc);
            uint8_t C = uinstr_c(*s->pc);
            UClosure *cond    = (UClosure *)s->R[A].v.p;
            UClosure *body    = (UClosure *)s->R[B].v.p;
            UClosure *onleave = (C == 0xFFu) ? NULL : (UClosure *)s->R[C].v.p;
            install_watcher_runtime(vm, s, UWATCHER_AT, cond, body, onleave, NULL);
            NEXT();
        }

        CASE(OP_AT_SYNC_INSTALL) {
            uint8_t A = uinstr_a(*s->pc);
            uint8_t B = uinstr_b(*s->pc);
            UClosure *cond = (UClosure *)s->R[A].v.p;
            UClosure *body = (UClosure *)s->R[B].v.p;
            install_watcher_runtime(vm, s, UWATCHER_AT_SYNC, cond, body, NULL, NULL);
            NEXT();
        }

        CASE(OP_WHENEVER_INSTALL) {
            uint8_t A = uinstr_a(*s->pc);
            uint8_t B = uinstr_b(*s->pc);
            uint8_t C = uinstr_c(*s->pc);
            UClosure *cond    = (UClosure *)s->R[A].v.p;
            UClosure *body    = (UClosure *)s->R[B].v.p;
            UClosure *onleave = (C == 0xFFu) ? NULL : (UClosure *)s->R[C].v.p;
            install_watcher_runtime(vm, s, UWATCHER_WHENEVER, cond, body, onleave, NULL);
            NEXT();
        }

        /* === T42: OP_WAITUNTIL_INSTALL — strand-block or pass-through ===
         *
         * A-encoded: A = cond_reg.
         *
         * Calls install_watcher_runtime which either:
         *   (a) fast-path: cond was truthy at install → watcher unregistered
         *       immediately, strand state unchanged (still RUNNING) → NEXT().
         *   (b) park path: cond was falsy → T40 set s->state = USTRAND_WAIT_WATCHER.
         *       Here we advance pc past this instruction, decrement
         *       strand_runnable_count (the strand leaves the runnable accounting),
         *       and goto exit_strand so the scheduler can pick up another strand.
         *       The eval-pass wake (T43) will resume the strand on the rising edge.
         *
         * Spec #2 §6.3. */
        CASE(OP_WAITUNTIL_INSTALL) {
            uint8_t A = uinstr_a(*s->pc);
            UClosure *cond = (UClosure *)s->R[A].v.p;
            UWatcherInstallResult r = install_watcher_runtime(
                vm, s, UWATCHER_WAITUNTIL, cond, NULL, NULL, s);
            if (r == URBI_INSTALL_OK && USTRAND_IS_WAITING(s)) {
                /* Strand parked by T40 (cond started false).  Advance pc past
                 * this instruction so resume lands at the correct next opcode.
                 * Decrement strand_runnable_count: the strand was RUNNING when
                 * this opcode dispatched; T40 set state to WAITING without
                 * going through sched_strand_block, so we do the accounting
                 * manually here. */
                s->pc++;
                if (vm->strand_runnable_count > 0)
                    vm->strand_runnable_count--;
                steps_consumed++;
                goto exit_strand;
            }
            /* Fast path (cond was truthy): watcher unregistered; strand RUNNING.
             * Fall through to next instruction. */
            NEXT();
        }

        /* === T47: OP_AT_EVENT_INSTALL / OP_AT_EVENT_SYNC_INSTALL ===
         *
         * ABC-encoded: A = event_reg, B = body_reg, C = onleave_reg (0xFF = absent).
         * Routes through install_at_event_runtime — no read-set trace, no
         * active_watchers_head linkage.  Watcher joins event->at_watchers_head
         * (FIFO) and owning_tag's member chain.
         * Spec #3 §6.2. */
        CASE(OP_AT_EVENT_INSTALL) {
            uint8_t A = uinstr_a(*s->pc);
            uint8_t B = uinstr_b(*s->pc);
            uint8_t C = uinstr_c(*s->pc);
            UEvent   *e       = (UEvent *)s->R[A].v.p;
            UClosure *body    = (UClosure *)s->R[B].v.p;
            UClosure *onleave = (C == 0xFFu) ? NULL : (UClosure *)s->R[C].v.p;
            install_at_event_runtime(vm, s, UWATCHER_AT_EVENT, e, body, onleave);
            NEXT();
        }

        CASE(OP_AT_EVENT_SYNC_INSTALL) {
            uint8_t A = uinstr_a(*s->pc);
            uint8_t B = uinstr_b(*s->pc);
            uint8_t C = uinstr_c(*s->pc);
            UEvent   *e       = (UEvent *)s->R[A].v.p;
            UClosure *body    = (UClosure *)s->R[B].v.p;
            UClosure *onleave = (C == 0xFFu) ? NULL : (UClosure *)s->R[C].v.p;
            install_at_event_runtime(vm, s, UWATCHER_AT_EVENT_SYNC, e, body, onleave);
            NEXT();
        }

        /* === T61: OP_GETSLOT_CHANGE_EVENT ===
         *
         * ABC: A = dst_reg, B = recv_reg, C = ic_index.
         * R[A] := the UEvent for (R[B], ic_table[C].name), lazy-created.
         * Non-object receiver: R[A] := NIL + URBI_LOG_WARN (fail-soft).
         * Spec #4 §4.1. */
        CASE(OP_GETSLOT_CHANGE_EVENT) {
            uint8_t A = uinstr_a(*s->pc);
            uint8_t B = uinstr_b(*s->pc);
            uint8_t C = uinstr_c(*s->pc);

            if (s->R[B].kind != (uint8_t)UVAL_OBJECT) {
                if (vm->host_log_fn)
                    vm->host_log_fn(vm, URBI_LOG_WARN,
                        "slot-change install on non-object receiver");
                UValue nil_val;
                nil_val.kind = (uint8_t)UVAL_NIL;
                nil_val.v.i  = 0;
                s->R[A] = nil_val;
                NEXT();
            }

            /* Resolve IC table.  NOTE: this site is missing the entry_closure
             * branch present in ic_resolve_pi — known VM-001 bug; wave-5-fixes. */
            UProtoInstance *pi = NULL;
            if (s->frame_count == 0) {
                if (s->module_instance != NULL
                    && s->module_instance->proto_instances != NULL) {
                    pi = &s->module_instance->proto_instances->entries[0];
                }
            } else {
                UClosure *cur_cl = s->frames[s->frame_count - 1].closure;
                if (cur_cl != NULL) pi = cur_cl->proto_inst;
            }
            if (pi == NULL || pi->ic_table == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "GETSLOT_CHANGE_EVENT: no IC table bound");
                HALT();
            }

            UObject *obj   = (UObject *)s->R[B].v.p;
            USymbol *name  = pi->ic_table[C].name;
            UEvent  *e     = urbi_object_get_or_create_change_event(vm, obj, name);
            if (e != NULL) {
                s->R[A] = uvalue_from_event(e);
            } else {
                UValue nil_val;
                nil_val.kind = (uint8_t)UVAL_NIL;
                nil_val.v.i  = 0;
                s->R[A] = nil_val;
            }
            NEXT();
        }

        /* M5 spec #5 §6: OP_LOAD_REALM_GLOBAL — loads realm->global_object into R[A].
         * Emitted as a prologue by the compiler when a function references any
         * realm global (spec #5 §5.1 + §5.2).  The register R[A] is then used
         * as the receiver for all OP_GETSLOT / OP_SETSLOT global accesses. */
        CASE(OP_LOAD_REALM_GLOBAL) {
            uint8_t A = uinstr_a(*s->pc);
            URealm *r = s->realm;
            if (r == NULL || r->global_object == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm,
                    "OP_LOAD_REALM_GLOBAL: strand has no realm");
                HALT();
            }
            s->R[A].kind = (uint8_t)UVAL_OBJECT;
            s->R[A].v.p  = r->global_object;
            NEXT();
        }

        /* M5 reactive-runtime stubs.  Each individual subsystem task replaces
         * its entry in the dispatch table (computed-goto) or this switch arm.
         * OP_INVOKE is the M4 reserve that also lands here until v1.x. */
#if UVM_USE_COMPUTED_GOTO
        label_m5_stub:
#else
        case OP_INVOKE:
#endif
        {
            URBI_DISPATCH_ASSERT(0 && "M5 opcode stub not yet wired");
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm, "M5 opcode dispatched before implementation");
            HALT();
        }

#if !UVM_USE_COMPUTED_GOTO
        default: {
            /* Unreachable — loader rejects unknown opcodes before uvm_run
               is called. The default: branch satisfies -Wswitch-enum. */
            vm->last_error = UVM_TYPE_ERROR;
            HALT();
        }
    }
#endif

    /* Unreachable for computed-goto path; switch path falls through from
       every NEXT() which ends with goto dispatch above. */

halt_error:
    /* Error path: strand is now dead. */
    s->state = USTRAND_STATE_DEAD;
    steps_consumed++;
    goto exit_strand;

safepoint:
    /* Safepoint actions (run at backward-branch, call, and non-top OP_RET).
       Order: unwind check → per-strand budget → VM-wide budget → GC → hooks. */
    if (s->pending_unwind != UEXEC_OK) {
        urbi_unwind(s);
        if (s->state == USTRAND_STATE_DEAD) goto exit_strand;
    }
    if (s->instruction_budget_remaining == 0) {
        s->state = USTRAND_STATE_READY;
        sched_strand_yield(s);
        goto exit_strand;
    }
    s->instruction_budget_remaining--;
    if (vm->step_budget_remaining == 0) {
        /* Budget exhausted from caller's perspective; state stays RUNNING.
           The uvm_run adapter treats RUNNING-but-exit as "continue". */
        goto exit_strand;
    }
    vm->step_budget_remaining--;
    if (vm->gc_pending)           urbi_gc_slice(vm, URBI_GC_SLICE_BUDGET);
    if (vm->pending_onleave_head) drain_pending_onleave_queue(vm);
    urbi_drain_deferred_slot_changes(vm);   /* spec #4 §5.4: before watcher_eval_dirty */
    if (vm->watcher_dirty_count > 0) watcher_eval_dirty(vm);
    /* Preemption flag reserved for v2; not checked at M3. */
    /* Resume dispatch. */
#if UVM_USE_COMPUTED_GOTO
    DISPATCH();
#else
    goto dispatch;
#endif

exit_strand:
    /* Spec #1 §6.1: notify the watcher that its body strand completed.
     * Called after the unwind/cleanup-stack walker has finished (the safepoint
     * and halt_error paths both run urbi_unwind before reaching here) but before
     * the strand object is freed by the scheduler's dead-path cleanup.
     * urbi_watcher_body_completed clears both s->watcher_body_owner and
     * w->body_strand atomically and handles PENDING_REFIRE / PENDING_UNREGISTER. */
    if (s->state == USTRAND_STATE_DEAD && s->watcher_body_owner != NULL) {
        urbi_watcher_body_completed(vm, s);
    }

    /* Wake any JOIN-blocked parents if this strand just reached DEAD. */
    if (s->state == USTRAND_STATE_DEAD && s->joiners_head != NULL) {
        fork_wake_joiners(s, vm);
    }

    /* strand_runnable_count ownership at exit:
     *   - uvm_run transient strands are not tracked in strand_runnable_count
     *     (they bypass sched_strand_make_runnable). The READY-cycle increment
     *     via sched_strand_yield is balanced by the dequeue decrement in the
     *     uvm_run loop (src/uvm.c, the strand_runnable_count-- block).
     *   - T16 urbi_step driver: strands dequeued from the ready queue before
     *     entering dispatch_loop_until_yield. T16 decrements strand_runnable_count
     *     in the driver after dispatch returns with state == USTRAND_STATE_DEAD,
     *     keeping the decrement co-located with the dequeue logic.
     *   - sched_strand_block handles RUNNING → WAITING decrements inline.
     * No decrement here; see T16 for the scheduler-driven DEAD-path decrement. */
    return steps_consumed;
}

/* uvm_run: moved to uvm_run.c (VM #6). */
