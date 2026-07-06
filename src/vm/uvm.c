/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode interpreter — dispatch loop (urbi_vm_init.c + urbi_vm_run.c hold lifecycle). */

#include "vm/uvm.h"
#include "runtime/umacros.h"
#include "urbi/urbi.h"    /* URBI_CALLBACK_WARN_US, URBI_WATCHDOG_WARN */
#include "runtime/uclosure.h"     /* UClosure full definition (embeds UCell) */
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h" /* urbi_sched_strand_yield */
#include "value/uvalue.h"
#include "runtime/uunwind.h"
#include "realm/urealm.h"            /* URealm — OP_LOAD_REALM_GLOBAL */
#include "urbi/gc.h" /* urbi_gc_slice + URBI_GC_SLICE_BUDGET */
#include "tag/utag.h"    /* UTag, utag_create/destroy */
#include "watcher/uwatcher.h"          /* UWatcher — watcher dispatch */
#include "watcher/uwatcher_install.h"  /* urbi_watcher_install_watcher_runtime, urbi_watcher_install_at_event_runtime */
#include "stdlib/temporal.h"           /* v0.9.4: urbi_periodic_body_completed */
#include "event/uevent.h"                    /* UEvent — cast target for OP_AT_EVENT_INSTALL */
#include "event/uevent_emit.h"               /* urbi_event_emit_sync — tier-2 tag enter/leave hooks */
#include "event/uevent_native.h"             /* uvalue_from_event — OP_GETSLOT_CHANGE_EVENT */
#include "vm/uop_fork.h" /* urbi_vm_op_fork_detach/join/wait + urbi_vm_fork_wake_joiners */
#include "vm/uvm_arith.h"    /* arith_add/sub/mul/div/neg + helpers */
#include "vm/uvm_internal.h" /* diag / closure cross-TU decls */
#include "object/uic.h"         /* UIC + urbi_slot_get_slow / urbi_slot_set_slow */
#include "object/uobject.h"     /* UObject — receivers for GETSLOT/SETSLOT */
#include "changed/uchanged_node.h"          /* urbi_object_get_or_create_change_event */
#include "gc/ugc.h"
#include "gc/ugc_incremental.h"
#include "chunk/uchunk.h"
#include "object/uchunk_instance.h"
#include "runtime/ucleanup.h"
#include "runtime/uframe.h"
#include "vm/uvm_op_overload.h"  /* urbi_vm_arith_method_fallback / _unary / _cmp (Gap #4) */
#include "value/uintern.h"       /* ustr_op_name (Gap #4 operator-name interning) */
#include "vm/uvm_slot.h"         /* urbi_vm_getslot_value, urbi_vm_setslot_value, urbi_vm_self_lookup, urbi_vm_dispatch_getter/setter, vm_trace_slot_read_if_needed */
#include "vm/uvm_tag_scope.h"    /* urbi_vm_push_tag_scope, urbi_vm_pop_tag_scope (v0.10.15, stage 1) */
#include "vm/uvm_reactive_install.h" /* urbi_vm_reactive_install — 7 install arms (v0.10.15, stage 2) */
#include "vm/uvm_reactive_drain.h"   /* vm_reactive_drain — centralised reactive pump (SCHED-02/SCHED-12) */
#include "stdlib/object_root.h"      /* urbi_raise_typed — typed Exception instance builder */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>              /* strlen, memcpy — OP_ADD String fast path */

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
   /* Computed-goto dispatch — DISPATCH expands to a `goto *<expr>` statement;
    * the replacement list cannot be wrapped in parentheses (you can't
    * parenthesize a statement), so bugprone-macro-parentheses is suppressed
    * here.  CASE(op) expands to a label, also unparenthesizable. */
#  define DISPATCH()  goto *dispatch_table[uinstr_op(*s->pc)]  /* NOLINT(bugprone-macro-parentheses) — `goto *expr` cannot be parenthesized */
#  define CASE(op)    label_##op:
#  define NEXT()      do { URBI_PERF_INC(s->vm, opcodes); s->pc++; DISPATCH(); } while (0)
#  define HALT()      goto halt_error
#else
#  define DISPATCH()  switch (uinstr_op(*s->pc))
#  define CASE(op)    case (op):
#  define NEXT()      do { URBI_PERF_INC(s->vm, opcodes); s->pc++; goto dispatch; } while (0)
#  define HALT()      goto halt_error
#endif

/* Dispatch-time assertion for placeholder opcode stubs.
 * POLICY: use URBI_DISPATCH_ASSERT ONLY for unreachable-in-production stubs
 * where the assert serves as a CI trip-wire but the code immediately below
 * sets a user-visible error and halts safely even if the assert strips.
 * Do NOT use URBI_DISPATCH_ASSERT for load-bearing invariants — those must
 * use unconditional HALT() paths so they fire in both debug and release builds.
 * In hosted builds this triggers assert() so CI catches stray opcodes.
 * In freestanding builds it is a no-op; the stub below sets UVM_TYPE_ERROR. */
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

/* The reactive-install operand-check / fault helpers
 * (vm_install_check_closure_operand / vm_install_check_event_operand /
 * vm_install_result_is_fatal / vm_install_fault) and the seven install arm
 * bodies were extracted to src/vm/uvm_reactive_install.c (v0.10.15
 * stage 2).  See urbi_vm_reactive_install(). */

/* ---------------------------------------------------------------------------
 * Cold helpers for typed throws at arith/compare/call error sites.
 *
 * Nine dispatch-loop HALT sites previously killed the strand directly,
 * bypassing try/catch, finally, and tag-leave cleanup.  These helpers route
 * each site through the same catchable typed-throw machinery that slot errors
 * already use (uvm_slot.c slot_throw_or_fatal pattern).
 *
 * Out-of-line on purpose: keeps ~9 cold stanzas out of the hot dispatch body
 * (C7/VM-SIMPLIFY-01 icache win).
 *
 * Proto resolution: vm->typeerror_proto — exactly as slot_throw_or_fatal
 * (uvm_slot.c:30-46) uses; no new lookup path invented.
 *
 * Safepoint consumption: callers `goto safepoint` on VM_BINOP_THREW, which
 * reaches the same `if (s->pending_unwind != UEXEC_OK) urbi_unwind(s)` arm
 * that slot throws use.
 * --------------------------------------------------------------------------- */

typedef enum { VM_BINOP_THREW, VM_BINOP_FATAL } VMBinopDirective;

/* Core: format is done; clone `proto`, bind `msg`, deposit as the strand's
 * pending throw so `goto safepoint` reaches the unwind walker. */
static VMBinopDirective
vm_dispatch_throw_core(UVM *vm, UStrand *s, struct UObject *proto, const char *msg)
{
    UValue inst;
    urbi_raise_typed(vm, proto, &inst, msg);
    s->unwind_value   = inst;
    s->pending_unwind = UEXEC_THROW;
    s->pc++;
    vm->last_error = UVM_OK;  /* consumed into the throw */
    return VM_BINOP_THREW;
}

/* Core: format is done; message is in vm->last_errmsg. */
static VMBinopDirective
vm_dispatch_typeerror_core(UVM *vm, UStrand *s)
{
    return vm_dispatch_throw_core(vm, s, vm->typeerror_proto, vm->last_errmsg);
}

/* v0.13.5: OP_DIV zero divisor → catchable DivByZero (legacy-conformant;
 * see arith_div).  Message mirrors legacy float.cc "division by 0" with
 * the same source-location prefix the arith TypeErrors carry. */
static VMBinopDirective
vm_divzero_error(UVM *vm, UStrand *s)
{
    UDiagWriter w;
    urbi_vm_diag_init(&w, vm->last_errmsg, UVM_ERRMSG_CAP);
    urbi_vm_diag_write_prefix(&w, s->root_proto, (size_t)(s->pc - s->pc_base));
    urbi_vm_diag_write_cstr(&w, "DivByZero: division by 0");
    return vm_dispatch_throw_core(vm, s, vm->divbyzero_proto, vm->last_errmsg);
}

/* Binary arith / compare ops (OP_ADD/SUB/MUL/DIV/LT/LE). */
static VMBinopDirective
vm_binop_typeerror(UVM *vm, UStrand *s, uint8_t opcode,
                   uint8_t bk, uint8_t ck)
{
    urbi_vm_format_type_error_binary(vm, s->root_proto,
        (size_t)(s->pc - s->pc_base), opcode, bk, ck);
    return vm_dispatch_typeerror_core(vm, s);
}

/* Unary op (OP_NEG). */
static VMBinopDirective
vm_unop_typeerror(UVM *vm, UStrand *s, uint8_t opcode, uint8_t bk)
{
    urbi_vm_format_type_error_unary(vm, s->root_proto,
        (size_t)(s->pc - s->pc_base), opcode, bk);
    return vm_dispatch_typeerror_core(vm, s);
}

/* Call-shape sites (OP_CALL callee-not-closure / arity): message already
 * formatted via urbi_vm_format_type_error_msg() at the call site. */
static VMBinopDirective
vm_call_typeerror(UVM *vm, UStrand *s)
{
    return vm_dispatch_typeerror_core(vm, s);
}

/* The FATAL arm is unreachable by construction today: urbi_raise_typed degrades
 * to throw-nil on OOM, matching slot_throw_or_fatal.  It is kept as the
 * documented defensive contract. */

/* Dispatch macros — same goto-safepoint / HALT() pattern for all 9 sites. */
#define VM_BINOP_TYPEERROR(opc, bk, ck) do {                              \
    if (vm_binop_typeerror(vm, s, (opc), (bk), (ck)) ==                   \
        VM_BINOP_THREW) goto safepoint;                                    \
    HALT();                                                                \
} while (0)

#define VM_UNOP_TYPEERROR(opc, bk) do {                                    \
    if (vm_unop_typeerror(vm, s, (opc), (bk)) ==                           \
        VM_BINOP_THREW) goto safepoint;                                    \
    HALT();                                                                \
} while (0)

#define VM_CALL_TYPEERROR() do {                                           \
    if (vm_call_typeerror(vm, s) == VM_BINOP_THREW) goto safepoint;       \
    HALT();                                                                \
} while (0)

#define VM_DIVZERO() do {                                                  \
    if (vm_divzero_error(vm, s) == VM_BINOP_THREW) goto safepoint;         \
    HALT();                                                                \
} while (0)

/* --- urbi_vm_dispatch_loop_until_yield ---
   The core execution engine.  Runs s's bytecode until one of:
   - strand reaches DEAD (top-level OP_RET or halt_error)
   - strand yields via OP_YIELD (state → READY)
   - step_budget_in opcodes are consumed (state remains RUNNING)
   Returns the number of opcodes consumed.

   The urbi_vm_run() function below is a thin adapter that creates a transient
   UStrand, loops calling this function until DEAD, then tears down.
   All dispatch state lives on the strand; vm holds only VM-wide state. */

uint64_t
urbi_vm_dispatch_loop_until_yield(UStrand *s, uint64_t step_budget_in)
{
    UVM *vm = s->vm;
    uint64_t steps_consumed = 0;

    s->state = USTRAND_STATE_RUNNING;
    vm->step_budget_remaining = step_budget_in;

    /* v0.10.5: deliver event payload to the OP_CALL result register on
     * resume from WAIT_EVENT.  urbi_event_waituntil parks the strand by setting
     * state=WAIT_EVENT, advancing pc past the OP_CALL, and exiting.  On wake,
     * c_event_emit_* deposits the payload in s->last_event_payload before
     * urbi_sched_strand_make_runnable (see wake_event_waiters in uevent_emit.c).
     * At this point s->pc points to the instruction AFTER the OP_CALL, so
     * s->pc[-1] is the parked OP_CALL instruction; uinstr_a(s->pc[-1]) gives
     * the destination register.  Writing there before the first dispatch
     * delivers the payload as the waituntil() call's return value.
     *
     * Guard: pc[-1] must be OP_CALL (not another park opcode) and pc must be
     * at least one instruction past the base.  UVAL_NIL payload is the "no
     * data" sentinel used throughout uevent_emit.c (EMITR-007 convention). */
    if (s->last_event_payload.kind != (uint8_t)UVAL_NIL
            && s->pc > s->pc_base) {
        uint32_t parked_instr = s->pc[-1];
        if (uinstr_op(parked_instr) == OP_CALL) {
            s->R[uinstr_a(parked_instr)] = s->last_event_payload;
            s->last_event_payload = (UValue){0};
        }
    }

#if UVM_USE_COMPUTED_GOTO
    /* Suppress -Wpedantic for the computed-goto dispatch: both `&&label`
       (label-address) and `goto *expr` (indirect goto) are GCC extensions.
       Scope limited to this function so other pedantic violations in uvm.c
       still surface. The switch-fallback build (URBI_VM_FORCE_SWITCH or
       non-GCC compilers) skips this entirely. */
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
    /* Dispatch table keyed by opcode.  All opcodes populated; loader
       validates opcode is in [0, OP_MAX) before urbi_vm_run is called. */
    static const void *const dispatch_table[OP_MAX] = {
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
        /* row 7 control-transfer — THROW/TRY_BEGIN/TRY_END/RESUME/LOAD_CATCH_VALUE
         * PUSH_TAG/POP_TAG/PUSH_FRAME_GUARD; TAG_STOP wired at v0.10.2. */
        [OP_THROW]            = &&label_OP_THROW,
        [OP_TAG_STOP]         = &&label_op_tag_stop,
        [OP_TRY_BEGIN]        = &&label_OP_TRY_BEGIN,
        [OP_TRY_END]          = &&label_OP_TRY_END,
        [OP_PUSH_TAG]         = &&label_OP_PUSH_TAG,
        [OP_POP_TAG]          = &&label_OP_POP_TAG,
        [OP_PUSH_FRAME_GUARD] = &&label_OP_PUSH_FRAME_GUARD,
        [OP_RESUME]           = &&label_OP_RESUME,
        [OP_LOAD_CATCH_VALUE] = &&label_OP_LOAD_CATCH_VALUE,
        /* reactive runtime — AT/WHENEVER install opcodes: */
        [OP_AT_INSTALL]            = &&label_OP_AT_INSTALL,
        [OP_AT_SYNC_INSTALL]       = &&label_OP_AT_SYNC_INSTALL,
        [OP_WHENEVER_INSTALL]      = &&label_OP_WHENEVER_INSTALL,
        [OP_WAITUNTIL_INSTALL]     = &&label_OP_WAITUNTIL_INSTALL,
        [OP_AT_EVENT_INSTALL]      = &&label_OP_AT_EVENT_INSTALL,
        [OP_AT_EVENT_SYNC_INSTALL] = &&label_OP_AT_EVENT_SYNC_INSTALL,
        [OP_GETSLOT_CHANGE_EVENT]  = &&label_OP_GETSLOT_CHANGE_EVENT,
        [OP_LOAD_REALM_GLOBAL]     = &&label_OP_LOAD_REALM_GLOBAL,
        [OP_LOAD_RECV]             = &&label_OP_LOAD_RECV,
        /* v0.7.2 S42 method-call ABI cleanup. */
        [OP_SELF]                  = &&label_OP_SELF,
        /* v0.10.2-reactive — whenever (e?) install. */
        [OP_WHENEVER_EVENT_INSTALL] = &&label_OP_WHENEVER_EVENT_INSTALL,
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

            /* S-string-plus: atom fast path for String + String concat.
             * Mixed-type coercion ("x" + 1) deferred to v1.x — caller
             * must use explicit .asString(). urbi_vm_arith_method_fallback
             * short-circuits on non-UVAL_OBJECT, so installing a "+" slot
             * on string_proto would not fire here. */
            if (b->kind == UVAL_STR && cc->kind == UVAL_STR) {
                const char *bs = (const char *)b->v.p;
                const char *cs = (const char *)cc->v.p;
                size_t bn = strlen(bs);
                size_t cn = strlen(cs);
                size_t total = bn + cn;
                const char *interned;
                if (total == 0) {
                    interned = ustr_intern(vm, "", 0);
                } else {
                    /* ustr_intern takes (bytes, nbytes) and does NOT read a
                     * NUL terminator — no terminator needed in `tmp`. */
                    char *tmp = (char *)vm->alloc_fn(NULL, total, vm->alloc_ud);
                    if (tmp == NULL) { vm->last_error = UVM_OOM; HALT(); }
                    /* NOLINTNEXTLINE(bugprone-not-null-terminated-result) — ustr_intern uses explicit nbytes. */
                    if (bn > 0) memcpy(tmp, bs, bn);
                    /* NOLINTNEXTLINE(bugprone-not-null-terminated-result) — ustr_intern uses explicit nbytes. */
                    if (cn > 0) memcpy(tmp + bn, cs, cn);
                    interned = ustr_intern(vm, tmp, total);
                    vm->alloc_fn(tmp, 0, vm->alloc_ud);
                }
                if (interned == NULL) { vm->last_error = UVM_OOM; HALT(); }
                a->kind = (uint8_t)UVAL_STR;
                a->v.p  = (void *)interned;
                NEXT();
            }

            UVMError rc = arith_add(a, b, cc);
            if (rc != UVM_OK) {
                /* Gap #4: type-error fallback to operator-method dispatch. */
                USymbol *op = ustr_op_name(vm, "+", 1);
                if (op != NULL) {
                    int frc = urbi_vm_arith_method_fallback(vm, a, b, cc, op,
                            (uint32_t)(s->pc - s->pc_base));
                    if (frc == VM_OP_OVERLOAD_OK) {
                        NEXT();
                    }
                    if (frc == VM_OP_OVERLOAD_THREW) {
                        /* VM-07: re-deposit the overload body's
                         * exception on this strand — mirrors OP_THROW's
                         * deposit shape (advance pc past this opcode so a
                         * catch handler resumes after the faulting op). */
                        s->unwind_value   = *a;   /* thrown value, in dst reg */
                        s->pending_unwind = UEXEC_THROW;
                        s->pc++;
                        goto safepoint;
                    }
                }
                VM_BINOP_TYPEERROR(OP_ADD, b->kind, cc->kind);
            }
            NEXT();
        }

        CASE(OP_SUB) {
            UValue *a = &s->R[uinstr_a(*s->pc)];
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *cc = &s->R[uinstr_c(*s->pc)];
            UVMError rc = arith_sub(a, b, cc);
            if (rc != UVM_OK) {
                /* Gap #4: type-error fallback to operator-method dispatch. */
                USymbol *op = ustr_op_name(vm, "-", 1);
                if (op != NULL) {
                    int frc = urbi_vm_arith_method_fallback(vm, a, b, cc, op,
                            (uint32_t)(s->pc - s->pc_base));
                    if (frc == VM_OP_OVERLOAD_OK) {
                        NEXT();
                    }
                    if (frc == VM_OP_OVERLOAD_THREW) {
                        /* VM-07: mirrors OP_THROW (see OP_ADD). */
                        s->unwind_value   = *a;   /* thrown value, in dst reg */
                        s->pending_unwind = UEXEC_THROW;
                        s->pc++;
                        goto safepoint;
                    }
                }
                VM_BINOP_TYPEERROR(OP_SUB, b->kind, cc->kind);
            }
            NEXT();
        }

        CASE(OP_MUL) {
            UValue *a = &s->R[uinstr_a(*s->pc)];
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *cc = &s->R[uinstr_c(*s->pc)];
            UVMError rc = arith_mul(a, b, cc);
            if (rc != UVM_OK) {
                /* Gap #4: type-error fallback to operator-method dispatch. */
                USymbol *op = ustr_op_name(vm, "*", 1);
                if (op != NULL) {
                    int frc = urbi_vm_arith_method_fallback(vm, a, b, cc, op,
                            (uint32_t)(s->pc - s->pc_base));
                    if (frc == VM_OP_OVERLOAD_OK) {
                        NEXT();
                    }
                    if (frc == VM_OP_OVERLOAD_THREW) {
                        /* VM-07: mirrors OP_THROW (see OP_ADD). */
                        s->unwind_value   = *a;   /* thrown value, in dst reg */
                        s->pending_unwind = UEXEC_THROW;
                        s->pc++;
                        goto safepoint;
                    }
                }
                VM_BINOP_TYPEERROR(OP_MUL, b->kind, cc->kind);
            }
            NEXT();
        }

        CASE(OP_DIV) {
            UValue *a = &s->R[uinstr_a(*s->pc)];
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *cc = &s->R[uinstr_c(*s->pc)];
            UVMError rc = arith_div(a, b, cc);
            if (rc != UVM_OK) {
                /* v0.13.5: a numeric zero divisor is a DivByZero, not a
                 * TypeError — both operands are numbers, so skip the
                 * operator-method fallback and raise the catchable subclass. */
                if (rc == UVM_DIV_ZERO) {
                    VM_DIVZERO();
                }
                /* Gap #4: type-error fallback to operator-method dispatch. */
                USymbol *op = ustr_op_name(vm, "/", 1);
                if (op != NULL) {
                    int frc = urbi_vm_arith_method_fallback(vm, a, b, cc, op,
                            (uint32_t)(s->pc - s->pc_base));
                    if (frc == VM_OP_OVERLOAD_OK) {
                        NEXT();
                    }
                    if (frc == VM_OP_OVERLOAD_THREW) {
                        /* VM-07: mirrors OP_THROW (see OP_ADD). */
                        s->unwind_value   = *a;   /* thrown value, in dst reg */
                        s->pending_unwind = UEXEC_THROW;
                        s->pc++;
                        goto safepoint;
                    }
                }
                VM_BINOP_TYPEERROR(OP_DIV, b->kind, cc->kind);
            }
            NEXT();
        }

        CASE(OP_NEG) {
            UValue *a = &s->R[uinstr_a(*s->pc)];
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            UVMError rc = arith_neg(a, b);
            if (rc != UVM_OK) {
                /* Gap #4: type-error fallback to operator-method dispatch.
                 * Unary neg uses "-" slot name (same as binary minus; contextual). */
                USymbol *op = ustr_op_name(vm, "-", 1);
                if (op != NULL) {
                    int frc = urbi_vm_arith_method_fallback_unary(vm, a, b, op,
                            (uint32_t)(s->pc - s->pc_base));
                    if (frc == VM_OP_OVERLOAD_OK) {
                        NEXT();
                    }
                    if (frc == VM_OP_OVERLOAD_THREW) {
                        /* VM-07: mirrors OP_THROW (see OP_ADD). */
                        s->unwind_value   = *a;   /* thrown value, in dst reg */
                        s->pending_unwind = UEXEC_THROW;
                        s->pc++;
                        goto safepoint;
                    }
                }
                VM_UNOP_TYPEERROR(OP_NEG, b->kind);
            }
            NEXT();
        }

        CASE(OP_RET) {
            URBI_PERF_INC(vm, returns);
            UValue retval = s->R[uinstr_a(*s->pc)];

            if (s->frame_count == 0) {
                /* Top-frame return — strand becomes DEAD.
                 * Adapter (urbi_vm_run) extracts result via out_slot. */
                if (s->out_slot != NULL) {
                    *s->out_slot = retval;
                }
                s->state = USTRAND_STATE_DEAD;
                steps_consumed++;
                goto exit_strand;
            }

            /* Non-top-frame: hand off to walker.
             * urbi_unwind() handles pop+deliver (5-kind unwind walker). */
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
             * created by urbi_emit_lazy_thunk can read their captured upvalues. */
            UClosure *cur_cl = (s->frame_count > 0)
                             ? s->frames[s->frame_count - 1].closure
                             : s->entry_closure;
            if (cur_cl == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm, "GETUPVAL: no closure in current frame");
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
                urbi_vm_format_type_error_msg(vm, "SETUPVAL: no closure in current frame");
                HALT();
            }
            {
                uint8_t a = uinstr_a(*s->pc);
                uint8_t b = uinstr_b(*s->pc);
                UUpvalCell *uvc = cur_cl->upvals[b];
                if (uvc->on_heap) {
                    /* Task 9c (GC-07): Dijkstra barrier on the
                     * CELL, not the executing closure.  The UUpvalCell is
                     * shared between sibling closures (OP_CLOSURE re-capture
                     * arm), so its color diverges from cur_cl's: a BLACK
                     * shared cell + GRAY executing closure stored a white
                     * value with no shade, and the gray sibling's later
                     * trace idempotency-skips the black cell — lost object.
                     * This store mutates the cell, not the closure, so the
                     * closure's color is irrelevant here. */
                    urbi_gc_upvalue_pre_store(vm, &uvc->cell, s->R[a]);
                    uvc->u.value = s->R[a];
                } else {
                    /* Stack-resident upvalue: the store target is a strand
                     * register — a ROOT, re-walked at ATOMIC_FINISH
                     * (GC-02) — so no barrier is needed (same
                     * rationale as urbi_gc_register_write). */
                    *uvc->u.stack_ptr = s->R[a];
                }
            }
            NEXT();
        }

        CASE(OP_CLOSURE) {
            /* ABx: R[A] := new closure from executing_proto->nested[Bx].
             * Reads nupvals pseudo-instructions (OP_MOVE-encoded) immediately
             * after, each specifying (B=in_stack, C=src_idx).
             *
             * v0.8.5: executing_proto is the UProto whose bytecode this
             * instruction lives in — cur_cl->proto at frame depth > 0,
             * s->root_proto at chunk-top.  Bx is the per-parent index
             * into that proto's own nested[] array (truly-recursive
             * emitter contract).  Replaces the prior cur_cl->origin_nested
             * fallback chain, which existed only because the pre-v0.8.5
             * flat emitter routed every OP_CLOSURE to the root's nested[]
             * regardless of lexical scope. */
            uint8_t  a  = uinstr_a(*s->pc);
            uint16_t bx = uinstr_bx(*s->pc);
            UProto *executing_proto = (s->frame_count > 0)
                ? (s->frames[s->frame_count - 1].closure
                       ? s->frames[s->frame_count - 1].closure->proto
                       : NULL)
                : s->root_proto;

            struct UProto **nested_arr =
                executing_proto ? executing_proto->nested : NULL;
            size_t nested_cnt =
                executing_proto ? executing_proto->nested_count : 0U;

            if (nested_arr == NULL || (size_t)bx >= nested_cnt) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm, "CLOSURE: proto index out of range");
                HALT();
            }
            UProto *child_proto = nested_arr[bx];
            UClosure *cl = urbi_vm_alloc_closure(vm, child_proto);
            if (cl == NULL) {
                vm->last_error = UVM_OOM;
                urbi_vm_format_oom(vm, sizeof(UClosure));
                HALT();
            }
            /* v0.9.0-repl: cross-session-IC binding via per-UProto back-pointer.
             * child_proto->owning_module_instance was stamped at instance
             * creation (Task 2) and is the mi where this proto was born.
             *
             * Replaces the v0.8.5 partial-bundle fallback chain that tried
             * s->module_instance first, then the parent closure's
             * origin_module_instance.  Eliminates both the two-branch
             * proto_inst binding AND the origin_module_instance propagation. */
            /* runtime-invariants F2: promote load-bearing owning_module_instance
             * guards from URBI_DISPATCH_ASSERT (strips in release) to
             * unconditional HALT paths.  These three checks are not debug
             * diagnostics — a NULL omi or out-of-range ic_index would cause
             * a NULL dereference or OOB read on the very next line, producing
             * silent corruption in release builds. */
            struct UChunkInstance *omi = child_proto->owning_module_instance;
            if (UNLIKELY(omi == NULL)) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm, "CLOSURE: owning_module_instance not wired");
                HALT();
            }
            if (UNLIKELY(omi->proto_instances == NULL)) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm, "CLOSURE: proto_instances array not allocated");
                HALT();
            }
            if (UNLIKELY((size_t)child_proto->ic_index >=
                         (size_t)omi->proto_instances->n)) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm, "CLOSURE: ic_index out of proto_instances range");
                HALT();
            }
            cl->proto_inst =
                &omi->proto_instances->entries[child_proto->ic_index];

            /* GC soundness (v0.13.2, TEST-GAP-01 discovery
             * chain): publish the closure into its destination register
             * BEFORE the upvalue-capture loop, not after.  urbi_vm_open_upvalue
             * below allocates UUpvalCells; a collection triggered there
             * (URBI_GC_STRESS collects on every alloc) swept `cl` while it
             * was reachable only through this C local, leaving R[A] dangling
             * after the late store (caught by the GC-15 no-sidecar assert
             * during the next strand-register root walk).  Registers are
             * roots, so the early store pins cl by construction.  Safe to
             * reorder: walk_uclosure skips NULL upvals[] entries (partially-
             * populated arrays are anticipated), in-stack captures take the
             * ADDRESS of R[src_idx] (no value read, so a src_idx == A alias
             * sees identical behaviour), and the re-capture arm reads the
             * parent closure's upvals, not registers. */
            s->R[a].kind  = (uint8_t)UVAL_CLOSURE;
            s->R[a].v.p   = cl;

            /* Read nupvals pseudo-instructions. */
            {
                int i;
                for (i = 0; i < (int)child_proto->nupvals; i++) {
                    s->pc++;
                    uint8_t in_stack = uinstr_b(*s->pc);
                    uint8_t src_idx  = uinstr_c(*s->pc);
                    if (in_stack) {
                        UUpvalCell *uvc = urbi_vm_open_upvalue(vm, s, &s->R[src_idx]);
                        if (uvc == NULL) {
                            /* VM-005: Step C-3: cl is GC-managed; do NOT free via
                             * alloc_fn (double-free hazard post-C-2).  The GC sweep
                             * reclaims it when it becomes unreachable after HALT. */
                            vm->last_error = UVM_OOM;
                            urbi_vm_format_oom(vm, sizeof(UUpvalCell));
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
                            /* Step C-3: GC-managed closure; no alloc_fn free needed. */
                            vm->last_error = UVM_TYPE_ERROR;
                            urbi_vm_format_type_error_msg(vm, "CLOSURE: upvalue re-capture out of range");
                            HALT();
                        }
                        cl->upvals[i] = par_cl->upvals[src_idx];
                    }
                }
            }
            /* R[A] store moved BEFORE the capture loop (v0.13.2; see the
             * GC-soundness comment above). */
            NEXT();
        }

        CASE(OP_CLOSE) {
            /* ABC: heapify all open upvalue cells at R >= R[A]. */
            urbi_vm_close_upvalues(s, &s->R[uinstr_a(*s->pc)]);
            NEXT();
        }

        CASE(OP_CALL) {
            /* ABC: R[A](args); B = nargs+1 (plain) or nargs+2 (method).
             *      C low 7 bits = nresults+1 (currently always 2).
             *      C bit 7 (0x80) = method-call flag (v1.6 S42).
             * After the call, the result overwrites R[A].
             *
             * Plain   (C & 0x80 == 0): args at R[A+1..A+B-1], nargs = B-1,
             *                          self = nil.
             * Method  (C & 0x80 != 0): R[A+1] = self (placed by preceding
             *                          OP_SELF); args at R[A+2..A+B-1],
             *                          nargs = B-2; self forwarded to the
             *                          callee verbatim. */
            URBI_PERF_INC(vm, calls);
            uint8_t a = uinstr_a(*s->pc);
            uint8_t b = uinstr_b(*s->pc);
            uint8_t c = uinstr_c(*s->pc);
            bool    is_method = (c & 0x80U) != 0U;
            int     nargs     = is_method ? (int)b - 2 : (int)b - 1;
            uint8_t arg_off   = is_method ? 2U : 1U;

            if (s->R[a].kind != (uint8_t)UVAL_CLOSURE) {
                urbi_vm_format_type_error_msg(vm, "function call: callee is not a closure");
                VM_CALL_TYPEERROR();
            }
            UClosure *callee = (UClosure *)s->R[a].v.p;
            UValue    self_value = is_method ? s->R[a + 1U] : urbi_make_nil();

            /* C-native dispatch.  When native_fn is set, the
             * closure has no bytecode body — invoke the C function instead.
             * The receiver (`self`) is R[A+1] on method calls (set by the
             * preceding OP_SELF) or nil on plain calls.  Result lands in
             * R[A]; nargs supplied via R[A+arg_off..A+B-1].
             *
             * VM-009 closure (closed at v0.6.1): the audit
             * flagged that native-register paths allocate UClosure cells
             * with `proto_inst = NULL`, leaving any subsequent OP_GETSLOT
             * inside the callee with no IC table to bind.  This native-
             * dispatch arm short-circuits BEFORE the new bytecode frame is
             * pushed and BEFORE proto_inst is read — the C function runs
             * inline on the caller's frame. */
            if (callee->native_fn != NULL) {
                URBI_PERF_INC(vm, native_calls);
                UValue *args_ptr = (nargs > 0) ? &s->R[a + arg_off] : NULL;
                UValue native_out;
                /* GC soundness (v0.13.2, TEST-GAP-01 discovery
                 * chain): root the native's out-slot (and the self copy)
                 * for the duration of the call.  native_out is a C stack
                 * local, NOT a register — natives that build a result
                 * incrementally (e.g. Job.jobs storing its fresh List in
                 * *out before appending) relied on *out being rooted,
                 * which was false: a collection triggered by a later
                 * allocation inside the native swept the half-built
                 * result.  Rooting here makes the documented "*out is
                 * reachable" contract true for every native centrally. */
                UCRootFrame nf_out, nf_self;
                urbi_zero(&native_out, sizeof native_out);
                ustrand_c_root_push(s, &nf_out, &native_out);
                ustrand_c_root_push(s, &nf_self, &self_value);
                int rc = callee->native_fn(vm, self_value, args_ptr,
                                           (uint8_t)nargs, &native_out);
                ustrand_c_root_pop(s, &nf_self);
                ustrand_c_root_pop(s, &nf_out);
                if (rc == UEXEC_OK) {
                    s->R[a] = native_out;
                    if (s->pending_unwind != UEXEC_OK) {
                        s->pc++;
                        goto safepoint;
                    }
                    /* v0.10.2: a native method may block the strand (e.g.
                     * sleep()) by calling urbi_sched_strand_block.  If the strand
                     * is now WAITING, advance pc past this OP_CALL so the
                     * scheduler resumes at the correct next instruction, then
                     * exit the dispatch loop.  Mirrors OP_WAITUNTIL_INSTALL's
                     * WAITING-exit at line ~1591.
                     * strand_runnable_count is already decremented by
                     * urbi_sched_strand_block (RUNNING → WAITING path). */
                    if (USTRAND_IS_WAITING(s)) {
                        s->pc++;
                        steps_consumed++;
                        goto exit_strand;
                    }
                    /* VM-03/B12: a native may also SUSPEND the
                     * running strand (t.block()/t.freeze() from inside the
                     * tag's own scope via urbi_strand_suspend's RUNNING arm).
                     * Mirror the WAITING exit: advance pc past the OP_CALL
                     * so resume continues at the next instruction, then
                     * leave dispatch.  The ustep driver skips SUSPENDED
                     * strands at dequeue; the unblock/unfreeze ungated
                     * resume (urbi_strand_resume_if_ungated) re-enqueues.
                     * No accounting here: urbi_strand_suspend's RUNNING arm
                     * already decremented strand_runnable_count (SCHED-01
                     * single-writer scheme), same as the READY-arm
                     * convention where unbind_from_ready_queue decrements. */
                    if (USTRAND_IS_SUSPENDED(s)) {
                        s->pc++;
                        steps_consumed++;
                        goto exit_strand;
                    }
                    /* v1.0 (design-risks v1.0-stm32f4-hang): run the reactive
                     * eval pass after a successful native call.
                     *
                     * urbi_vm_watcher_eval_dirty normally runs only at the dispatcher
                     * `safepoint:` label, reached by a bytecode call, a backward
                     * branch, or a non-top OP_RET — NOT by a native call, which
                     * returns via NEXT().  An event/watcher body whose state
                     * mutation is followed only by a native call therefore never
                     * triggers the eval: e.g. the STM32F4 mandelbrot handler
                     * `at (gyro_tick?) { ...; Realm.redraw_requested = true;
                     * lcd_fill_rect(...) }`.  Its urbi_watcher_observer_dirty bumps then
                     * accumulate and the rising edge is never seen, so the
                     * demo's loader-strand `while (true) { ...;
                     * waituntil (Realm.redraw_requested) }` never wakes
                     * (test_whenever_double_fire.c documents the same "body
                     * needs a call to drain dirty" contract — but only bytecode
                     * calls qualified; native calls did not).
                     *
                     * We run only the drain + eval here, NOT the full safepoint
                     * (no GC slice, no per-strand budget yield), to keep native
                     * calls cheap and to avoid shifting GC color/timing.  This
                     * is bounded: the eval runs mid-body while the running body
                     * is still its watcher's body_strand, so
                     * urbi_watcher_do_spawn_body_coroutine's gate prevents the unbounded
                     * level-trigger re-spawn that a *post-dispatch* drain caused
                     * (the reverted S46 attempt — see test_whenever_double_fire).
                     * Skip when already inside an eval/scratch/install context
                     * (mirrors urbi_emit_slot_change_slow's re-entry guard); the
                     * enclosing pass will drain on return. */
                    vm_reactive_drain(vm, /*bounded_whenever=*/0);
                    NEXT();
                }
                if (rc == UEXEC_THROW) {
                    /* v0.11.4: native helpers (urbi_raise_*) build a typed
                     * Exception instance into native_out.  Deposit it as a
                     * catchable throw instead of fatal-halting; the safepoint
                     * walks the cleanup stack to a try-handler (or terminates
                     * the strand if none).  Mirrors OP_THROW's deposit shape. */
                    s->R[a]           = native_out;  /* register-window rooting
                                                         parity with the OK arm;
                                                         the unwind reads
                                                         unwind_value, not R[a] */
                    s->unwind_value   = native_out;
                    s->pending_unwind = UEXEC_THROW;
                    s->pc++;
                    goto safepoint;
                }
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm, "function call: native method raised");
                HALT();
            }

            /* Arity check.  v0.13.5: protos compiled under the arity
             * self-check discipline (module header flag bit 0 →
             * proto->arity_prologue) enforce their own minimum in a
             * bytecode prologue, so the VM only rejects too-many here
             * (`nargs <= nparams`); pre-v0.13.5 blobs keep the historic
             * exact-match check — no semantic shift for old bytecode. */
            if (callee->proto->arity_prologue != 0U
                    ? (nargs > (int)callee->proto->nparams)
                    : (nargs != (int)callee->proto->nparams)) {
                urbi_vm_format_type_error_msg(vm, "function call: wrong argument count");
                VM_CALL_TYPEERROR();
            }
            if (s->frame_count >= UVM_MAX_FRAMES) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm, "function call: call stack overflow");
                HALT();
            }

            /* Check stack space: callee's frame starts at R[a+arg_off]. */
            if ((s->R + a + arg_off + callee->proto->max_reg + 1) > (s->stack + UVM_STACK_CAP)) {
                vm->last_error = UVM_OOM;
                urbi_vm_format_oom(vm, (size_t)(callee->proto->max_reg + 1) * sizeof(UValue));
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
            /* Gap #3 (v0.6.2 Phase 2 / v1.6 S42): save receiver for
             * OP_LOAD_RECV (`this`).  Sourced from R[A+1] when the caller
             * flagged a method call; nil for plain calls. */
            new_frame->recv            = self_value;

            /* Switch to callee frame. Args R[a+arg_off..a+nargs+arg_off-1]
             * become R[0..nargs-1]. */
            s->R        = &s->R[a + arg_off];
            s->pc       = callee->proto->instructions;
            s->pc_base  = s->pc;
            /* Route through the shared helper so OP_CALL and
             * pop_call_frame cannot drift on the constants-from-closure rule. */
            s->cur_consts = ustrand_consts_for_closure(s, callee);

            /* Zero registers beyond nparams up to max_reg. */
            {
                int si;
                for (si = nargs; si <= (int)callee->proto->max_reg; si++) {
                    UValue z = {0};
                    s->R[si] = z;
                }
            }

            /* v0.13.5: seed the actual passed-arg count into the synthetic
             * \x01nargs local at R[nparams] for arity-self-check protos.
             * The prologue reads it to enforce min arity and fill omitted
             * defaulted params.  Gated on the flag: unflagged protos may
             * have max_reg == nparams - 1 exactly (no slot to write);
             * flagged >=1-param protos always reserved the slot. */
            if (callee->proto->arity_prologue != 0U
                    && callee->proto->nparams > 0U) {
                s->R[callee->proto->nparams] = urbi_make_int((int64_t)nargs);
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
            /* ABC: if ((R[B]==R[C]) != A) pc++
             * Gap #4: when lhs is a user object, try "==" slot before
             * falling back to identity/structural equality. */
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *c = &s->R[uinstr_c(*s->pc)];
            bool eq;
            if (b->kind == (uint8_t)UVAL_OBJECT) {
                USymbol *op = ustr_op_name(vm, "==", 2);
                if (op != NULL) {
                    UValue thrown = {0};
                    int frc = urbi_vm_cmp_method_fallback(vm, &eq, &thrown, b, c,
                            op, (uint32_t)(s->pc - s->pc_base));
                    if (frc == VM_OP_OVERLOAD_OK) {
                        if ((int)eq != (int)uinstr_a(*s->pc)) { s->pc++; }
                        NEXT();
                    }
                    if (frc == VM_OP_OVERLOAD_THREW) {
                        /* VM-07: mirrors OP_THROW (see OP_ADD).
                         * unwind_value is a rooted strand field; nothing
                         * allocates between the fallback's hand-off and
                         * this deposit. */
                        s->unwind_value   = thrown;
                        s->pending_unwind = UEXEC_THROW;
                        s->pc++;
                        goto safepoint;
                    }
                }
            }
            eq = uvalue_equal(b, c);
            if ((int)eq != (int)uinstr_a(*s->pc)) { s->pc++; }
            NEXT();
        }

        CASE(OP_NEQ) {
            /* ABC: if ((R[B]!=R[C]) != A) pc++
             * Gap #4: when lhs is a user object, try "!=" slot first. */
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *c = &s->R[uinstr_c(*s->pc)];
            bool neq;
            if (b->kind == (uint8_t)UVAL_OBJECT) {
                USymbol *op = ustr_op_name(vm, "!=", 2);
                if (op != NULL) {
                    UValue thrown = {0};
                    int frc = urbi_vm_cmp_method_fallback(vm, &neq, &thrown, b, c,
                            op, (uint32_t)(s->pc - s->pc_base));
                    if (frc == VM_OP_OVERLOAD_OK) {
                        if ((int)neq != (int)uinstr_a(*s->pc)) { s->pc++; }
                        NEXT();
                    }
                    if (frc == VM_OP_OVERLOAD_THREW) {
                        /* VM-07: mirrors OP_THROW (see OP_EQ). */
                        s->unwind_value   = thrown;
                        s->pending_unwind = UEXEC_THROW;
                        s->pc++;
                        goto safepoint;
                    }
                }
            }
            neq = !uvalue_equal(b, c);
            if ((int)neq != (int)uinstr_a(*s->pc)) { s->pc++; }
            NEXT();
        }

        CASE(OP_LT) {
            /* ABC: if ((R[B]<R[C]) != A) pc++ — numeric comparison only */
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *c = &s->R[uinstr_c(*s->pc)];
            bool lt = false;
            if (uvalue_lt(b, c, &lt) != UVAL_CMP_OK) {
                /* Gap #4: type-error fallback to "<" slot on lhs. */
                USymbol *op = ustr_op_name(vm, "<", 1);
                if (op != NULL) {
                    UValue thrown = {0};
                    int frc = urbi_vm_cmp_method_fallback(vm, &lt, &thrown, b, c,
                            op, (uint32_t)(s->pc - s->pc_base));
                    if (frc == VM_OP_OVERLOAD_OK) {
                        if ((int)lt != (int)uinstr_a(*s->pc)) { s->pc++; }
                        NEXT();
                    }
                    if (frc == VM_OP_OVERLOAD_THREW) {
                        /* VM-07: mirrors OP_THROW (see OP_EQ). */
                        s->unwind_value   = thrown;
                        s->pending_unwind = UEXEC_THROW;
                        s->pc++;
                        goto safepoint;
                    }
                }
                /* Mirror retry (H12): a > b compiles to swapped OP_LT(b,a),
                 * so c is the syntactic left operand.  On lookup miss for
                 * "<" on b, try ">" on c with operands swapped.
                 * A THREW from a found ">" method must propagate; only a
                 * true miss falls through to the TypeError below. */
                {
                    USymbol *mirror = ustr_op_name(vm, ">", 1);
                    if (mirror != NULL) {
                        UValue mthrown = {0};
                        int mfrc = urbi_vm_cmp_method_fallback(vm, &lt, &mthrown,
                                c, b, mirror,
                                (uint32_t)(s->pc - s->pc_base));
                        if (mfrc == VM_OP_OVERLOAD_OK) {
                            if ((int)lt != (int)uinstr_a(*s->pc)) { s->pc++; }
                            NEXT();
                        }
                        if (mfrc == VM_OP_OVERLOAD_THREW) {
                            s->unwind_value   = mthrown;
                            s->pending_unwind = UEXEC_THROW;
                            s->pc++;
                            goto safepoint;
                        }
                    }
                }
                VM_BINOP_TYPEERROR(OP_LT, b->kind, c->kind);
            }
            if ((int)lt != (int)uinstr_a(*s->pc)) { s->pc++; }
            NEXT();
        }

        CASE(OP_LE) {
            /* ABC: if ((R[B]<=R[C]) != A) pc++ — numeric comparison only */
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *c = &s->R[uinstr_c(*s->pc)];
            bool le = false;
            if (uvalue_le(b, c, &le) != UVAL_CMP_OK) {
                /* Gap #4: type-error fallback to "<=" slot on lhs. */
                USymbol *op = ustr_op_name(vm, "<=", 2);
                if (op != NULL) {
                    UValue thrown = {0};
                    int frc = urbi_vm_cmp_method_fallback(vm, &le, &thrown, b, c,
                            op, (uint32_t)(s->pc - s->pc_base));
                    if (frc == VM_OP_OVERLOAD_OK) {
                        if ((int)le != (int)uinstr_a(*s->pc)) { s->pc++; }
                        NEXT();
                    }
                    if (frc == VM_OP_OVERLOAD_THREW) {
                        /* VM-07: mirrors OP_THROW (see OP_EQ). */
                        s->unwind_value   = thrown;
                        s->pending_unwind = UEXEC_THROW;
                        s->pc++;
                        goto safepoint;
                    }
                }
                /* Mirror retry (H12): a >= b compiles to swapped OP_LE(b,a),
                 * so c is the syntactic left operand.  On lookup miss for
                 * "<=" on b, try ">=" on c with operands swapped.
                 * A THREW from a found ">=" method must propagate. */
                {
                    USymbol *mirror = ustr_op_name(vm, ">=", 2);
                    if (mirror != NULL) {
                        UValue mthrown = {0};
                        int mfrc = urbi_vm_cmp_method_fallback(vm, &le, &mthrown,
                                c, b, mirror,
                                (uint32_t)(s->pc - s->pc_base));
                        if (mfrc == VM_OP_OVERLOAD_OK) {
                            if ((int)le != (int)uinstr_a(*s->pc)) { s->pc++; }
                            NEXT();
                        }
                        if (mfrc == VM_OP_OVERLOAD_THREW) {
                            s->unwind_value   = mthrown;
                            s->pending_unwind = UEXEC_THROW;
                            s->pc++;
                            goto safepoint;
                        }
                    }
                }
                VM_BINOP_TYPEERROR(OP_LE, b->kind, c->kind);
            }
            if ((int)le != (int)uinstr_a(*s->pc)) { s->pc++; }
            NEXT();
        }

        CASE(OP_YIELD) {
            /* Cooperative yield: advance past this opcode, transition to READY,
               and return to the scheduler.  The urbi_vm_run adapter re-enters
               urbi_vm_dispatch_loop_until_yield until strand is DEAD.
               urbi_sched_strand_yield asserts entry state == RUNNING (SCHED-003)
               and overwrites with READY on enqueue, so no pre-set here. */
            s->pc++;
            if (s->is_transient_strand) {
                /* B11/SCHED-03 + B4/REVIVAL §S5a: transient strands (scratch
                 * at-sync/onleave bodies, urbi_vm_run) run their body
                 * ATOMICALLY — `;` is non-yielding there and the body runs to
                 * completion, "never silent truncation" (§S5a; AT_SYNC bodies
                 * are "synchronous scratch frame; no yield" per
                 * docs/internals/reactive-runtime.md).  Two reasons NOT to
                 * enqueue + exit here:
                 *   (1) the UStrand lives on the C stack — urbi_sched_strand_yield
                 *       would put a dead-stack pointer on vm->ready_head (UAF);
                 *   (2) exiting at the first `;` would drop every statement
                 *       after it (the bug this replaces).
                 * PC is already advanced past OP_YIELD; continue dispatch into
                 * the next statement.  Over-budget bodies still fail-soft via
                 * the safepoint-budget arm reached on the body's own backward
                 * branches / bytecode calls — same bound as finally/onleave.
                 * ASYNC at/event/whenever bodies run on REAL (non-transient)
                 * strands via urbi_watcher_do_spawn_body_coroutine → urbi_strand_create
                 * (is_transient_strand stays 0), so they fall through to
                 * urbi_sched_strand_yield below and still yield on `;` as before. */
                URBI_PERF_INC(s->vm, opcodes);
#if UVM_USE_COMPUTED_GOTO
                DISPATCH();
#else
                goto dispatch;
#endif
            }
            urbi_sched_strand_yield(s);
            steps_consumed++;
            goto exit_strand;
        }

        CASE(OP_FORK_DETACH) {
            /* `,` separator: spawn child closure as detached strand.
             * A = closure_reg.  Parent continues; child runs concurrently.
             * See src/uop_fork.c for closure-spawn vs. spec §7.1 rationale.
             * Rejected from urbi_vm_run's stack-local transient because that
             * adapter only dispatches its own strand and would leak any
             * spawned children.  The transient routes onto
             * vm->global_realm->strands_head for GC-walker visibility, so
             * realm == NULL no longer discriminates; the dedicated flag
             * is_transient_strand does. */
            if (s->is_transient_strand) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm, "',' (parallel-detach) requires an event-loop strand");
                HALT();
            }
            int rc = urbi_vm_op_fork_detach(s, vm, *s->pc);
            if (rc != 0) goto exit_strand;
            NEXT();
        }

        CASE(OP_FORK_JOIN) {
            /* `&` separator LHS: spawn child closure, store handle in R[B].
             * A = closure_reg, B = child_handle_reg.
             * Same urbi_vm_run-transient guard as OP_FORK_DETACH; see note above. */
            if (s->is_transient_strand) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm, "'&' (parallel-join) requires an event-loop strand");
                HALT();
            }
            int rc = urbi_vm_op_fork_join(s, vm, *s->pc);
            if (rc != 0) goto exit_strand;
            NEXT();
        }

        CASE(OP_JOIN_WAIT) {
            /* `&` separator join-point: block until child handle in R[A] is DEAD.
             * A = child_handle_reg. */
            int rc = urbi_vm_op_join_wait(s, vm, *s->pc);
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
             *
             * Thin arm — decode, resolve IC + recv, call helpers.
             * All policy (IC fast-path, trace, getter, slow path + error)
             * lives in uvm_slot.c. */
            URBI_PERF_INC(vm, slot_get);
            uint32_t i = *s->pc;
            uint8_t  dst_reg  = uinstr_a(i);
            uint8_t  recv_reg = uinstr_b(i);
            uint8_t  ic_index = uinstr_c(i);

            UProtoInstance *pi = ic_resolve_pi(s);
            if (pi == NULL || pi->ic_table == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm, "slot access: no IC table bound");
                HALT();
            }
            UIC *ic = &pi->ic_table[ic_index];
            UObject *recv;
            if (s->R[recv_reg].kind == (uint8_t)UVAL_OBJECT) {
                recv = (UObject *)s->R[recv_reg].v.p;
            } else {
                recv = urbi_atom_proto_for_value(vm, s->R[recv_reg]);
                if (recv == NULL) {
                    vm->last_error = UVM_OOM;
                    urbi_vm_format_type_error_msg(vm, "slot access: atom proto allocation failed");
                    HALT();
                }
            }
            UValue out_val; uint8_t fk = 0;
            UVmSlotResult sr = urbi_vm_getslot_value(vm, ic, recv, &out_val, &fk);
            if (sr == UVM_SLOT_OK)             { s->R[dst_reg] = out_val; NEXT(); }
            if (sr == UVM_SLOT_GETTER_NEEDED)  {
                UValue gr;
                /* v0.11.4: UVM_SLOT_THREW → catchable throw deposited on the
                 * strand; advance pc past this OP_GETSLOT (mirrors OP_THROW)
                 * so a catch-handler resumes after the faulting op. */
                UVmSlotResult _r = urbi_vm_dispatch_getter(vm, ic->uprops[fk], "slot access", &gr);
                if (_r == UVM_SLOT_THREW) { s->pc++; goto safepoint; }
                if (_r != UVM_SLOT_OK) HALT();
                s->R[dst_reg] = gr; NEXT();
            }
            /* MISSING — slow path (error formatting + getter check inside helper). */
            {
                UVmSlotResult _r = urbi_vm_getslot_slow(vm, ic, recv, "slot access", &out_val);
                if (_r == UVM_SLOT_THREW) { s->pc++; goto safepoint; }
                if (_r != UVM_SLOT_OK) HALT();
            }
            s->R[dst_reg] = out_val;
            NEXT();
        }

        CASE(OP_SETSLOT) {
            /* OP_SETSLOT ABC: R[B].slot[ic_index] := R[A].
             *
             * Thin arm — decode, guard, call helpers.
             * All IC policy (fast-path + slow-path + error) lives in uvm_slot.c. */
            URBI_PERF_INC(vm, slot_set);
            uint32_t i = *s->pc;
            uint8_t  src_reg  = uinstr_a(i);
            uint8_t  recv_reg = uinstr_b(i);
            uint8_t  ic_index = uinstr_c(i);

            UProtoInstance *pi = ic_resolve_pi(s);
            if (pi == NULL || pi->ic_table == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm, "slot write: no IC table bound");
                HALT();
            }
            UIC *ic = &pi->ic_table[ic_index];
            if (s->R[recv_reg].kind != (uint8_t)UVAL_OBJECT) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm, "slot write: receiver is not an Object");
                HALT();
            }
            UObject *recv = (UObject *)s->R[recv_reg].v.p;
            if (recv == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm, "slot write: receiver is NULL");
                HALT();
            }
            if ((recv->flags & URBI_OBJ_FLAG_READONLY) != 0U) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm,
                    "slot write: cannot mutate a frozen object");
                HALT();
            }
            UValue v = s->R[src_reg];
            uint8_t fk = 0;
            UVmSlotResult sr = urbi_vm_setslot_value(vm, ic, recv, v, &fk);
            if (sr == UVM_SLOT_OK)            { NEXT(); }
            if (sr == UVM_SLOT_SETTER_NEEDED) {
                /* v0.11.4: UVM_SLOT_THREW → catchable throw; advance pc past
                 * this OP_SETSLOT (mirrors OP_THROW). */
                UVmSlotResult _r = urbi_vm_dispatch_setter(vm, ic->uprops[fk], "slot write", v);
                if (_r == UVM_SLOT_THREW) { s->pc++; goto safepoint; }
                if (_r != UVM_SLOT_OK) HALT();
                NEXT();
            }
            if (sr == UVM_SLOT_CONST_WRITE)   {
                vm->last_error = UVM_TYPE_ERROR;
                {
                    UDiagWriter _w;
                    urbi_vm_diag_init(&_w, vm->last_errmsg, UVM_ERRMSG_CAP);
                    urbi_vm_diag_write_cstr(&_w, "TypeError: slot write: cannot write to constant slot '");
                    if (ic->name != NULL) urbi_vm_diag_write_cstr(&_w, (const char *)ic->name);
                    urbi_vm_diag_write_cstr(&_w, "'");
                }
                HALT();
            }
            /* MISSING — slow path (error formatting + setter/barrier inside helper). */
            {
                UVmSlotResult _r = urbi_vm_setslot_slow(vm, ic, recv, v, "slot write");
                if (_r == UVM_SLOT_THREW) { s->pc++; goto safepoint; }
                if (_r != UVM_SLOT_OK) HALT();
            }
            NEXT();
        }

        /* --- row 7 control-transfer opcodes --- */

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
            UCleanupEntry *entry = urbi_sched_strand_cleanup_push(s);
            if (entry == NULL) {
                /* Cleanup stack full — strand fatal. */
                s->fatal_status = UEXEC_THROW;
                s->state        = USTRAND_STATE_DEAD;
                goto exit_strand;
            }
            entry->kind           = (uint8_t)UCLEANUP_TRY_FRAME;
            entry->flags          = flags;
            entry->handler_pc     = handler_pc;
            entry->frame_depth    = (uint16_t)s->frame_count;  /* VM-01 */
            entry->register_base  = 0U;
            entry->register_count = 0U;
            entry->owning_tag     = NULL;
            entry->catch_pattern  = NULL;
            entry->next_member    = NULL;
            entry->strand_back    = NULL;
            NEXT();
        }

        CASE(OP_TRY_END) {
            /* OP_TRY_END ABC: no operands.  Pop the top UCLEANUP_TRY_FRAME entry. */
            if (s->cleanup_depth > 0) {
                urbi_sched_strand_cleanup_pop(s, UCLEANUP_TRY_FRAME);
            }
            NEXT();
        }

        CASE(OP_RESUME) {
            /* OP_RESUME: end of a finally/cleanup body.
             * Exits urbi_vm_dispatch_loop_until_yield so run_cleanup_with_replace()
             * can check pending_unwind and restore the saved unwind state.
             * State stays RUNNING; caller (run_cleanup_with_replace) handles
             * the transition.  cleanup_body_done is the completion marker
             * distinguishing this exit from a yield/budget exit
             * (VM-02). */
            s->cleanup_body_done = 1U;
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
            /* Body extracted to urbi_vm_push_tag_scope (uvm_tag_scope.c, v0.10.15
             * stage 1).  The helper cannot goto a dispatch label, so its
             * fatal path returns UVM_TAG_SCOPE_FATAL for `goto exit_strand`. */
            UVmTagScopeResult r = urbi_vm_push_tag_scope(vm, s);
            if (r == UVM_TAG_SCOPE_FATAL) goto exit_strand;
            if (r == UVM_TAG_SCOPE_HALT)  HALT();
            NEXT();
        }

        CASE(OP_POP_TAG) {
            /* Body extracted to urbi_vm_pop_tag_scope (uvm_tag_scope.c, v0.10.15
             * stage 1).  The (v1.0-dead) FLAG_HAS_ONLEAVE branch returns
             * UVM_TAG_SCOPE_HALT for HALT(). */
            UVmTagScopeResult r = urbi_vm_pop_tag_scope(vm, s);
            if (r == UVM_TAG_SCOPE_FATAL) goto exit_strand;
            if (r == UVM_TAG_SCOPE_HALT)  HALT();
            NEXT();
        }

        CASE(OP_PUSH_FRAME_GUARD) {
            /* OP_PUSH_FRAME_GUARD ABC: A = register_base, B = register_count, C = 0.
             * Push a UCLEANUP_CALL_FRAME entry onto the cleanup stack.
             * The unwind walker absorbs UEXEC_RETURN at CALL_FRAME entries,
             * delivering the return value and popping the frame.
             * strand_back = s for compatibility with unwind walker. */
            uint8_t register_base  = uinstr_a(*s->pc);
            uint8_t register_count = uinstr_b(*s->pc);
            UCleanupEntry *entry = urbi_sched_strand_cleanup_push(s);
            if (entry == NULL) {
                s->fatal_status = UEXEC_THROW;
                s->state        = USTRAND_STATE_DEAD;
                goto exit_strand;
            }
            entry->kind           = (uint8_t)UCLEANUP_CALL_FRAME;
            entry->flags          = 0U;
            entry->handler_pc     = 0U;
            entry->frame_depth    = (uint16_t)s->frame_count;  /* VM-01: walker
                                       skips frame-pop for CALL_FRAME (absorb
                                       arm manages its own frame teardown) */
            entry->register_base  = register_base;
            entry->register_count = register_count;
            entry->owning_tag     = NULL;
            entry->catch_pattern  = NULL;
            entry->next_member    = NULL;
            entry->strand_back    = s;
            NEXT();
        }

        /* OP_TAG_STOP: v0.10.2 implementation
         * reserved-stub.  ABC encoding: A = reg_tag (UVAL_TAG), B = reg_value
         * (stop payload, reserved at v1.0 — pass nil), C = 0.
         *
         * Forwards to urbi_tag_stop which deposits UEXEC_TAG_STOP on every
         * member strand, walks the onleave watcher cascade, and sets
         * UTAG_FLAG_STOPPED.  Wire format v1.8 / 0x18 unchanged — OP_TAG_STOP
         * opcode value 30 is already in the bytecode table; no emit path
         * produced it until v0.10.2, when Tag.new() + .stop() became scripted.
         *
         * Type-check: R[A] must be UVAL_TAG.  script-side `.stop()` resolves
         * through Tag.new() which always returns UVAL_TAG, so a TYPE_ERROR
         * here indicates a VM bug, not a user error. */
#if UVM_USE_COMPUTED_GOTO
        label_op_tag_stop:
#else
        case OP_TAG_STOP:
#endif
        {
            uint8_t A = uinstr_a(*s->pc);
            if (s->R[A].kind != (uint8_t)UVAL_TAG) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm,
                    "tag.stop(): argument must be a Tag");
                HALT();
            }
            UTag *_stop_tag = (UTag *)s->R[A].v.p;
            URBI_INTERNAL_ASSERT(_stop_tag != NULL);
            urbi_tag_stop(vm, _stop_tag, urbi_make_nil());
            NEXT();
        }

        /* === OP_AT_INSTALL / OP_AT_SYNC_INSTALL / OP_WHENEVER_INSTALL ===
         *
         * ABC-encoded: A = cond_reg, B = body_reg, C = onleave_reg (0xFF = absent).
         * Routes through urbi_watcher_install_watcher_runtime with the appropriate UWATCHER_*
         * mode.  On return the watcher is installed and the strand continues to the
         * next instruction — at-watchers do not block the installing strand.
         * Spec #2 §6.3. */
        /* The seven reactive-install arm bodies are extracted to
         * urbi_vm_reactive_install (uvm_reactive_install.c, v0.10.15, stage 2).
         * Each helper cannot goto a dispatch label, so the (v1.0-unreachable)
         * fault path returns UVM_INSTALL_HALT for HALT(); OP_WAITUNTIL_INSTALL's
         * park path returns UVM_INSTALL_PARK_EXIT, leaving the dispatch-loop-
         * local `steps_consumed++; goto exit_strand;` here in the arm. */
        CASE(OP_AT_INSTALL) {
            if (urbi_vm_reactive_install(vm, s, OP_AT_INSTALL) == UVM_INSTALL_HALT) HALT();
            NEXT();
        }

        CASE(OP_AT_SYNC_INSTALL) {
            if (urbi_vm_reactive_install(vm, s, OP_AT_SYNC_INSTALL) == UVM_INSTALL_HALT) HALT();
            NEXT();
        }

        CASE(OP_WHENEVER_INSTALL) {
            if (urbi_vm_reactive_install(vm, s, OP_WHENEVER_INSTALL) == UVM_INSTALL_HALT) HALT();
            NEXT();
        }

        CASE(OP_WAITUNTIL_INSTALL) {
            UVmReactiveInstallResult r = urbi_vm_reactive_install(vm, s, OP_WAITUNTIL_INSTALL);
            if (r == UVM_INSTALL_HALT) HALT();
            if (r == UVM_INSTALL_PARK_EXIT) { steps_consumed++; goto exit_strand; }
            NEXT();
        }

        CASE(OP_AT_EVENT_INSTALL) {
            if (urbi_vm_reactive_install(vm, s, OP_AT_EVENT_INSTALL) == UVM_INSTALL_HALT) HALT();
            NEXT();
        }

        CASE(OP_AT_EVENT_SYNC_INSTALL) {
            if (urbi_vm_reactive_install(vm, s, OP_AT_EVENT_SYNC_INSTALL) == UVM_INSTALL_HALT) HALT();
            NEXT();
        }

        CASE(OP_WHENEVER_EVENT_INSTALL) {
            if (urbi_vm_reactive_install(vm, s, OP_WHENEVER_EVENT_INSTALL) == UVM_INSTALL_HALT) HALT();
            NEXT();
        }

        /* === OP_GETSLOT_CHANGE_EVENT ===
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
                    vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                        "slot-change install on non-object receiver");
                UValue nil_val;
                nil_val.kind = (uint8_t)UVAL_NIL;
                nil_val.v.i  = 0;
                s->R[A] = nil_val;
                NEXT();
            }

            /* Resolve IC table.  Mirrors ic_resolve_pi (VM-008): at
             * frame_count == 0, prefer s->entry_closure->proto_inst when
             * present so closures imported from a foreign module/chunk
             * use their own IC table rather than the calling chunk's
             * entries[0].  VM-001 closed in v0.5.7-fixes Phase 5. */
            UProtoInstance *pi = ic_resolve_pi(s);
            if (pi == NULL || pi->ic_table == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm, "slot-change event: no IC table bound");
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

        /* OP_LOAD_REALM_GLOBAL — loads realm->global_object into R[A] (spec #5 §6).
         * Emitted as a prologue by the compiler when a function references any
         * realm global (spec #5 §5.1 + §5.2).  The register R[A] is then used
         * as the receiver for all OP_GETSLOT / OP_SETSLOT global accesses. */
        CASE(OP_LOAD_REALM_GLOBAL) {
            uint8_t A = uinstr_a(*s->pc);
            URealm *r = s->realm;
            if (r == NULL || r->global_object == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm,
                    "global access: strand has no realm");
                HALT();
            }
            s->R[A].kind = (uint8_t)UVAL_OBJECT;
            s->R[A].v.p  = r->global_object;
            NEXT();
        }

        /* v0.6.2 Phase 2 — OP_LOAD_RECV: loads the receiver saved in the
         * current call frame's .recv field into R[A].  The receiver is
         * set at OP_CALL dispatch time from R[A+1] when OP_CALL's C
         * carries the method-flag bit (set by the emitter following an
         * OP_SELF).  Plain (non-method) calls and top-level evaluation
         * load nil.  Emitted by the compiler for `this` inside a method
         * body (AST_THIS with fs->parent != NULL). */
        CASE(OP_LOAD_RECV) {
            uint8_t A = uinstr_a(*s->pc);
            /* recv is a UValue (nil when called from top-level or plain
             * variable call; the receiver object for method-call pattern). */
            s->R[A] = s->frame_count > 0
                      ? s->frames[s->frame_count - 1].recv
                      : urbi_make_nil();
            NEXT();
        }

        /* v0.7.2 S42 — OP_SELF: load method + receiver into adjacent
         * registers atomically.
         *
         *   ABC: A = dst_reg, B = recv_reg, C = ic_index.
         *   R[A+1] := R[B]                            (preserves recv kind)
         *   R[A]   := lookup_slot(R[B], K[ic_index])  (atom-proto routed
         *                                              identically to OP_GETSLOT)
         *
         * Emitted by the compiler as the prelude to a method-flagged OP_CALL
         * (C & 0x80) so the call site reads its receiver from R[A+1] instead
         * of the deleted vm->last_recv global.  Eliminates the silent-elision
         * bug where intervening OP_GETSLOTs in argument evaluation clobbered
         * the global receiver before the outer OP_CALL.
         *
         * Dispatch mirrors OP_GETSLOT (same IC machinery, getter path,
         * atom-proto routing, watcher-install trace probe).  Differences:
         *   * Receiver snapshot is unconditionally written to R[A+1] (the
         *     getter path also writes R[A+1] so `this` is correct inside
         *     the getter body once implicit-this lands).
         *   * No vm->last_recv side effect — that field is gone at v1.6. */
        CASE(OP_SELF) {
            /* OP_SELF ABC: R[A] := R[B].slot[ic_index],  R[A+1] := R[B].
             *
             * Thin arm.  Receiver-snapshot BEFORE lookup (dst_reg
             * may alias recv_reg); R[A+1] is always written before R[A]. */
            uint32_t i = *s->pc;
            uint8_t  dst_reg  = uinstr_a(i);
            uint8_t  recv_reg = uinstr_b(i);
            uint8_t  ic_index = uinstr_c(i);

            UProtoInstance *pi = ic_resolve_pi(s);
            if (pi == NULL || pi->ic_table == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                urbi_vm_format_type_error_msg(vm, "method call: no IC table bound");
                HALT();
            }
            UIC *ic = &pi->ic_table[ic_index];

            UValue self_value = s->R[recv_reg]; /* snapshot before possible alias */
            UObject *recv;
            if (self_value.kind == (uint8_t)UVAL_OBJECT) {
                recv = (UObject *)self_value.v.p;
            } else {
                recv = urbi_atom_proto_for_value(vm, self_value);
                if (recv == NULL) {
                    vm->last_error = UVM_OOM;
                    urbi_vm_format_type_error_msg(vm, "method call: atom proto allocation failed");
                    HALT();
                }
            }

            UValue out_slot; uint8_t fk = 0;
            UVmSlotResult sr = urbi_vm_self_lookup(vm, ic, recv, &out_slot, &fk);
            if (sr == UVM_SLOT_OK) {
                s->R[dst_reg + 1U] = self_value; s->R[dst_reg] = out_slot; NEXT();
            }
            if (sr == UVM_SLOT_GETTER_NEEDED) {
                UValue gr;
                /* v0.11.4: UVM_SLOT_THREW → catchable throw; advance pc past
                 * this OP_SELF (mirrors OP_THROW). */
                UVmSlotResult _r = urbi_vm_dispatch_getter(vm, ic->uprops[fk], "method call", &gr);
                if (_r == UVM_SLOT_THREW) { s->pc++; goto safepoint; }
                if (_r != UVM_SLOT_OK) HALT();
                s->R[dst_reg + 1U] = self_value; s->R[dst_reg] = gr; NEXT();
            }
            /* MISSING — slow path. */
            {
                UVmSlotResult _r = urbi_vm_getslot_slow(vm, ic, recv, "method call", &out_slot);
                if (_r == UVM_SLOT_THREW) { s->pc++; goto safepoint; }
                if (_r != UVM_SLOT_OK) HALT();
            }
            s->R[dst_reg + 1U] = self_value;
            s->R[dst_reg]      = out_slot;
            NEXT();
        }

#if !UVM_USE_COMPUTED_GOTO
        default: {
            /* Unreachable — loader rejects unknown opcodes before urbi_vm_run
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
        /* v0.13.1-B: a cleanup body's replacement unwind absorbed at an
         * outer handler may leave the strand parked (WAITING — e.g. a sleep
         * in the absorbing catch handler) or yielded (READY) when the
         * nested dispatch exits — the walker returns with pending == OK and
         * the strand no longer RUNNING.  Exit dispatch; the scheduler
         * resumes it (its pc already points at the post-park instruction).
         * Resuming the dispatch loop here would execute a parked strand. */
        if (USTRAND_GET_STATE(s) != USTRAND_RUNNING) goto exit_strand;
    }
    if (s->safepoint_budget_remaining == 0) {
        /* B11/SCHED-03: transient strands (scratch, urbi_vm_run) must not
         * be enqueued via urbi_sched_strand_yield — the UStrand lives on the C
         * stack and a later urbi_step traversal via vm->ready_head would be
         * a dead-stack UAF.  Exit dispatch with state == RUNNING; the callers
         * (run_on_scratch_core, urbi_vm_run) handle the RUNNING-on-return
         * case by re-arming the budget and continuing. */
        if (s->is_transient_strand) goto exit_strand;
        /* urbi_sched_strand_yield asserts entry state == RUNNING (SCHED-003)
         * and overwrites with READY on enqueue, so no pre-set here. */
        urbi_sched_strand_yield(s);
        goto exit_strand;
    }
    s->safepoint_budget_remaining--;
    if (vm->step_budget_remaining == 0) {
        /* VM-04/SCHED-11 (scheduler-liveness wedge): the caller's
           urbi_step budget is exhausted mid-slice.  Re-enqueue the still-RUNNING
           strand as READY (count-neutral RUNNING -> READY) so the NEXT urbi_step
           re-dispatches it via sched_pick_next.  Leaving it RUNNING-but-off the
           ready queue would wedge urbi_step at URBI_STEP_RUNNING forever:
           sched_pick_next only returns vm->ready_head, so no later call could
           ever pick the strand back up (an embedder `while (urbi_step(vm, N) ==
           RUNNING)` loop would spin without progress).  The state == RUNNING
           assert inside urbi_sched_strand_yield is guaranteed here — the unwind and
           safepoint-budget arms above already diverted every non-RUNNING case.
           The urbi_vm_run adapter passes UINT64_MAX and never reaches this arm;
           were it to, its READY arm re-runs the strand identically.
           B11/SCHED-03: same transient guard as the safepoint-budget arm above —
           do not enqueue stack-local strands onto the scheduler queues. */
        if (s->is_transient_strand) goto exit_strand;
        urbi_sched_strand_yield(s);
        goto exit_strand;
    }
    vm->step_budget_remaining--;
    if (vm->gc_pending)           urbi_gc_slice(vm, URBI_GC_SLICE_BUDGET);
    vm_reactive_drain(vm, /*bounded_whenever=*/0);
    /* Preemption flag reserved for v2; not checked at v1.0. */
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
    if (s->state == USTRAND_STATE_DEAD) {
        URBI_TP(vm, URBI_TRACE_SCHED, URBI_LOG_DEBUG, URBI_TP_SCHED_EXIT,
                (uint32_t)(uintptr_t)s, (uint32_t)s->state);
    }

    /* v0.9.4 every() periodic-spawn: re-arm or unregister on body death. */
    if (s->state == USTRAND_STATE_DEAD && s->periodic_owner != NULL) {
        urbi_periodic_body_completed(vm, s);
    }

    /* Wake any JOIN-blocked parents if this strand just reached DEAD. */
    if (s->state == USTRAND_STATE_DEAD && s->joiners_head != NULL) {
        urbi_vm_fork_wake_joiners(s, vm);
    }

    /* strand_runnable_count ownership at exit (SCHED-01
     * single-writer scheme — urbi_sched_runnable_inc/dec are the only writers):
     *   - Transient strands (urbi_vm_run) never participate in the count;
     *     both helpers skip them.
     *   - DEAD: the strand was RUNNING (counted); urbi_sched_post_dispatch step 1
     *     decrements after the driver clears vm->cur_strand.
     *   - WAITING: urbi_sched_strand_block decremented at the parking site.
     *   - SUSPENDED: urbi_strand_suspend decremented (RUNNING arm) or
     *     unbind_from_ready_queue did (READY arm).
     *   - READY (yield): urbi_sched_strand_yield re-enqueued count-neutrally.
     * No count mutation in the dispatch loop or its drivers. */

    /* VM-06a canary: at outermost dispatch exit (not nested under
     * a cleanup body — cleanup_run_depth == 0), the C-stack root chain must
     * be empty.  A frame leaked past its push/pop pair would leave
     * strand_walk_roots reading a dead C stack frame on the next mark phase
     * (silent corruption, not a crash); catch it loudly in hosted builds.
     * Outermost-exit-only: nested cleanup re-entries (cleanup_run_depth > 0)
     * intentionally leave c_roots_head non-NULL while their frame is live. */
    URBI_INTERNAL_ASSERT(s->cleanup_run_depth != 0U || s->c_roots_head == NULL);
#if UVM_USE_COMPUTED_GOTO
#  pragma GCC diagnostic pop
#endif
    return steps_consumed;
}

/* urbi_vm_run: moved to urbi_vm_run.c (VM #6). */
