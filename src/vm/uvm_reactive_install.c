/* SPDX-License-Identifier: BSD-3-Clause */
/* src/vm/uvm_reactive_install.c — the seven reactive-install dispatch arms,
 * extracted from the uvm.c dispatch loop (v0.10.15-vm-decomp-2, stage 2).
 *
 * Behavior-preserving move: each arm body is byte-for-byte its v0.10.14
 * version, with HALT() rewritten as `return UVM_INSTALL_HALT;`, the
 * fall-through NEXT() as `return UVM_INSTALL_NEXT;`, and the OP_WAITUNTIL_INSTALL
 * park (`steps_consumed++; goto exit_strand;`) split so the helper does the
 * pc-advance + runnable-count decrement and returns UVM_INSTALL_PARK_EXIT while
 * the arm keeps `steps_consumed++; goto exit_strand;` (a dispatch-loop local).
 * The four operand-check / fault helpers move with the arms (their only callers)
 * and stay static here.  See uvm_reactive_install.h. */

#include "vm/uvm_reactive_install.h"

#include "vm/uvm.h"
#include "vm/uvm_internal.h"          /* urbi_vm_format_type_error_msg, urbi_vm_format_oom */
#include "urbi/urbi.h"                /* UVM_TYPE_ERROR, UVM_OOM */
#include "sched/ustrand.h"            /* UStrand, USTRAND_IS_WAITING */
#include "chunk/uchunk.h"             /* uinstr_a/b/c */
#include "runtime/uclosure.h"         /* UClosure */
#include "runtime/umacros.h"          /* URBI_INTERNAL_ASSERT */
#include "watcher/uwatcher.h"         /* UWatcher, UWATCHER_* modes */
#include "watcher/uwatcher_install.h" /* urbi_watcher_install_watcher_runtime, urbi_watcher_install_at_event_runtime, UWatcherInstallResult, UWATCHER_INSTALL_* */
#include "event/uevent.h"             /* UEvent */
#include "event/uevent_native.h"      /* uvalue_is_event */
#include "value/uvalue.h"             /* UValue, UVAL_CLOSURE */

#include <stddef.h>
#include <stdint.h>

/* --- vm_install_check_closure_operand (VM-003) ---
   Reactive-install opcode operand-register kind check.  The emitter places
   UClosure values into the cond / body / onleave registers via
   OP_CLOSURE before the install opcode dispatches, so under correct
   bytecode this check is a no-op.  Hand-crafted bytecode (or a future
   emit bug) could leave a non-closure value there; without the check the
   dispatcher casts (UClosure *)R[reg].v.p anyway and downstream watcher
   ops dereference garbage.

   Returns 1 on success.  On failure sets vm->last_error = UVM_TYPE_ERROR
   with a diagnostic message and returns 0; caller is expected to HALT()
   immediately.  urbi_vm_format_type_error_msg's bounded buffer is sufficient
   for the longest opcode name + slot name combination here. */
static int
vm_install_check_closure_operand(UVM *vm, const UStrand *s, uint8_t reg,
                                 const char *opcode_name, const char *slot_name)
{
    if (s->R[reg].kind != (uint8_t)UVAL_CLOSURE) {
        vm->last_error = UVM_TYPE_ERROR;
        urbi_vm_format_type_error_msg(vm,
            "reactive install: register operand is not a closure");
        (void)opcode_name;  /* available for future diagnostic enrichment */
        (void)slot_name;
        return 0;
    }
    return 1;
}

/* --- vm_install_check_event_operand (VM-013) ---
   AT_EVENT install opcode operand-register kind check.  Mirrors
   vm_install_check_closure_operand but for OP_AT_EVENT_INSTALL /
   OP_AT_EVENT_SYNC_INSTALL: the A register must hold a UVAL_EVENT
   (produced by OP_GETSLOT_CHANGE_EVENT or by stdlib Event.new).
   Without this check the dispatcher casts (UEvent *)R[A].v.p directly
   and urbi_watcher_install_at_event_runtime dereferences garbage.

   Routes through uvalue_is_event() rather than a raw kind comparison
   so the predicate location stays single-source-of-truth (the uvalue_is_event
   refactor pattern). */
static int
vm_install_check_event_operand(UVM *vm, const UStrand *s, uint8_t reg,
                               const char *opcode_name)
{
    if (!uvalue_is_event(s->R[reg])) {
        vm->last_error = UVM_TYPE_ERROR;
        urbi_vm_format_type_error_msg(vm,
            "AT_EVENT install: register operand is not an event");
        (void)opcode_name;
        return 0;
    }
    return 1;
}

/* --- vm_install_result_is_fatal / vm_install_fault (VM-002, VM-012) ---
   Translate a UWatcherInstallResult from urbi_watcher_install_watcher_runtime /
   urbi_watcher_install_at_event_runtime into a VM fault.  Prior to v0.5.7-fixes Phase 5
   the install opcodes ignored the return value entirely, so OOM-pool /
   trace-fault / recursive-install errors became silent no-ops: the strand
   sailed past a watcher that was never armed, with no observable diagnostic
   beyond a host_log_fn warning.  This pair makes the dispatcher promote
   those errors to UVM faults so the program halts cleanly instead of
   continuing with broken reactive semantics.

   READSET_OVER is currently treated as fatal as well: today
   urbi_watcher_install_watcher_runtime returns it from Phase 4 (before pool_alloc) so
   the watcher is not actually installed; treating it as recoverable would
   diverge from the spec #2 §7.4 "fires on any slot write — conservative
   but correct" intent without the matching install path.  When that path
   lands (filed as backlog), demote READSET_OVER here to a non-fatal log
   and continue to NEXT(). */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
static inline int
vm_install_result_is_fatal(UWatcherInstallResult r)
{
    return r != UWATCHER_INSTALL_OK;
}

static void
vm_install_fault(UVM *vm, UWatcherInstallResult r, const char *opcode_name)
{
    switch (r) {
        case UWATCHER_INSTALL_OOM_POOL:
            vm->last_error = UVM_OOM;
            urbi_vm_format_oom(vm, sizeof(struct UWatcher));
            break;
        case UWATCHER_INSTALL_READSET_OVER:
            vm->last_error = UVM_TYPE_ERROR;
            urbi_vm_format_type_error_msg(vm,
                "watcher install: read-set exceeds URBI_WATCHER_READSET_MAX");
            break;
        case UWATCHER_INSTALL_TRACE_FAULT:
            vm->last_error = UVM_TYPE_ERROR;
            urbi_vm_format_type_error_msg(vm,
                "watcher install: condition threw during trace");
            break;
        case UWATCHER_INSTALL_RECURSIVE:
            vm->last_error = UVM_TYPE_ERROR;
            urbi_vm_format_type_error_msg(vm,
                "watcher install attempted from within scratch-frame eval");
            break;
        case UWATCHER_INSTALL_NO_OBSERVABLE_CELLS:
            /* v0.10.2: cond watcher with empty read-set is a program error.
             * Use `whenever (e?) body` for event-driven subscriptions. */
            vm->last_error = UVM_TYPE_ERROR;
            urbi_vm_format_type_error_msg(vm,
                "watcher install: condition observes no slots; "
                "use 'whenever (e?) body' for event subscriptions");
            break;
        case UWATCHER_INSTALL_OK:
        default:
            /* Caller should not invoke this on UWATCHER_INSTALL_OK.  Defensive. */
            vm->last_error = UVM_TYPE_ERROR;
            urbi_vm_format_type_error_msg(vm, "watcher install: unknown result");
            break;
    }
    (void)opcode_name;  /* available for future diagnostic enrichment */
}

/* Per-opcode install descriptor — data-tables the per-opcode facts that were
 * duplicated across the 7 ABC-parallel arms.
 *
 * is_event  — true: A-operand is a UEvent (checked with vm_install_check_event_operand);
 *             false: A-operand is a UClosure "cond" (vm_install_check_closure_operand).
 * has_onleave — true: C-operand carries an onleave closure or 0xFF sentinel;
 *               false: C is ignored (AT_SYNC has no onleave by spec).
 * mode      — UWATCHER_* constant passed to the install function.
 */
typedef struct {
    uint8_t mode;
    uint8_t is_event;    /* 1 = event install; 0 = cond install */
    uint8_t has_onleave; /* 1 = check/pass C; 0 = always NULL   */
} UInstallSpec;

/* Indexed by opcode value.  Only the six standard ABC install opcodes are
 * populated; WAITUNTIL is handled separately (park logic). */
static const UInstallSpec install_specs[] = {
    [OP_AT_INSTALL]            = { UWATCHER_AT,             0, 1 },
    [OP_AT_SYNC_INSTALL]       = { UWATCHER_AT_SYNC,        0, 0 },
    [OP_WHENEVER_INSTALL]      = { UWATCHER_WHENEVER,       0, 1 },
    [OP_AT_EVENT_INSTALL]      = { UWATCHER_AT_EVENT,       1, 1 },
    [OP_AT_EVENT_SYNC_INSTALL] = { UWATCHER_AT_EVENT_SYNC,  1, 1 },
    [OP_WHENEVER_EVENT_INSTALL]= { UWATCHER_WHENEVER_EVENT, 1, 1 },
};

UVmReactiveInstallResult
urbi_vm_reactive_install(UVM *vm, UStrand *s, uint8_t op)
{
    switch (op) {
    /* === Standard ABC install opcodes (6 arms collapsed to one body) ===
     *
     * Cond installs: A=cond(closure), B=body(closure), C=onleave|0xFF.
     *   Calls urbi_watcher_install_watcher_runtime(vm, s, mode, cond, body, onleave, NULL).
     *   AT_SYNC has no onleave (has_onleave=0; C ignored).
     * Event installs: A=event(UEvent), B=body(closure), C=onleave|0xFF.
     *   Calls urbi_watcher_install_at_event_runtime(vm, s, mode, event, body, onleave).
     *
     * Per-opcode facts live in install_specs[] above. */
    case OP_AT_INSTALL:
    case OP_AT_SYNC_INSTALL:
    case OP_WHENEVER_INSTALL:
    case OP_AT_EVENT_INSTALL:
    case OP_AT_EVENT_SYNC_INSTALL:
    case OP_WHENEVER_EVENT_INSTALL: {
        const UInstallSpec *spec = &install_specs[op];
        const char *opn = urbi_vm_op_name(op);
        uint8_t A = uinstr_a(*s->pc);
        uint8_t B = uinstr_b(*s->pc);
        uint8_t C = uinstr_c(*s->pc);
        if (spec->is_event) {
            if (!vm_install_check_event_operand(vm, s, A, opn))
                return UVM_INSTALL_HALT;
        } else {
            if (!vm_install_check_closure_operand(vm, s, A, opn, "cond"))
                return UVM_INSTALL_HALT;
        }
        if (!vm_install_check_closure_operand(vm, s, B, opn, "body"))
            return UVM_INSTALL_HALT;
        if (spec->has_onleave && C != 0xFFU
            && !vm_install_check_closure_operand(vm, s, C, opn, "onleave"))
            return UVM_INSTALL_HALT;
        UClosure *body    = (UClosure *)s->R[B].v.p;
        UClosure *onleave = (spec->has_onleave && C != 0xFFU)
                            ? (UClosure *)s->R[C].v.p : NULL;
        UWatcherInstallResult r;
        if (spec->is_event) {
            UEvent *e = (UEvent *)s->R[A].v.p;
            r = urbi_watcher_install_at_event_runtime(vm, s, spec->mode, e, body, onleave);
        } else {
            UClosure *cond = (UClosure *)s->R[A].v.p;
            r = urbi_watcher_install_watcher_runtime(vm, s, spec->mode, cond, body, onleave, NULL);
        }
        if (vm_install_result_is_fatal(r)) {
            vm_install_fault(vm, r, opn);
            return UVM_INSTALL_HALT;
        }
        return UVM_INSTALL_NEXT;
    }

    /* === OP_WAITUNTIL_INSTALL — strand-block or pass-through ===
     *
     * A-encoded: A = cond_reg.
     *
     * Calls urbi_watcher_install_watcher_runtime which either:
     *   (a) fast-path: cond was truthy at install → watcher unregistered
     *       immediately, strand state unchanged (still RUNNING) → NEXT().
     *   (b) park path: cond was falsy → install parked the strand via
     *       urbi_sched_strand_block(USTRAND_REASON_WATCHER) (refactor-3 SCHED-01:
     *       block owns the runnable-count decrement).  Here we advance pc
     *       past this instruction and goto exit_strand so the scheduler can
     *       pick up another strand.  The eval-pass wake will resume
     *       the strand on the rising edge.
     *
     * Spec #2 §6.3. */
    case OP_WAITUNTIL_INSTALL: {
        uint8_t A = uinstr_a(*s->pc);
        if (!vm_install_check_closure_operand(vm, s, A, "OP_WAITUNTIL_INSTALL", "cond"))
            return UVM_INSTALL_HALT;
        UClosure *cond = (UClosure *)s->R[A].v.p;
        UWatcherInstallResult r = urbi_watcher_install_watcher_runtime(
            vm, s, UWATCHER_WAITUNTIL, cond, NULL, NULL, s);
        if (vm_install_result_is_fatal(r)) {
            vm_install_fault(vm, r, "OP_WAITUNTIL_INSTALL");
            return UVM_INSTALL_HALT;
        }
        if (r == UWATCHER_INSTALL_OK && USTRAND_IS_WAITING(s)) {
            /* Strand parked (cond started false).  Advance pc past this
             * instruction so resume lands at the correct next opcode.
             * Runnable-count accounting is owned by urbi_sched_strand_block
             * inside urbi_watcher_install_watcher_runtime (SCHED-01) — no manual
             * adjustment here.  The arm completes the exit with
             * `steps_consumed++; goto exit_strand;`. */
            s->pc++;
            return UVM_INSTALL_PARK_EXIT;
        }
        /* Fast path (cond was truthy): watcher unregistered; strand RUNNING.
         * Fall through to next instruction. */
        return UVM_INSTALL_NEXT;
    }

    default:
        /* Unreachable: the dispatch arm passes only the seven install opcodes.
         * Defensive — set an error so a HALT() has a populated last_error. */
        URBI_INTERNAL_ASSERT(0);
        vm->last_error = UVM_TYPE_ERROR;
        urbi_vm_format_type_error_msg(vm, "reactive install: unknown opcode");
        return UVM_INSTALL_HALT;
    }
}
