/* SPDX-License-Identifier: BSD-3-Clause */
/* src/vm/uvm_reactive_install.c — the seven reactive-install dispatch arms,
 * extracted from the uvm.c dispatch loop (v0.10.15-vm-decomp-2, W1 stage 2).
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
#include "vm/uvm_internal.h"          /* vm_format_type_error_msg, vm_format_oom */
#include "urbi/urbi.h"                /* UVM_TYPE_ERROR, UVM_OOM */
#include "sched/ustrand.h"            /* UStrand, USTRAND_IS_WAITING */
#include "chunk/uchunk.h"             /* uinstr_a/b/c */
#include "runtime/uclosure.h"         /* UClosure */
#include "runtime/umacros.h"          /* URBI_INTERNAL_ASSERT */
#include "watcher/uwatcher.h"         /* UWatcher, UWATCHER_* modes */
#include "watcher/uwatcher_install.h" /* install_watcher_runtime, install_at_event_runtime, UWatcherInstallResult, URBI_INSTALL_* */
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
   immediately.  vm_format_type_error_msg's bounded buffer is sufficient
   for the longest opcode name + slot name combination here. */
static int
vm_install_check_closure_operand(UVM *vm, const UStrand *s, uint8_t reg,
                                 const char *opcode_name, const char *slot_name)
{
    if (s->R[reg].kind != (uint8_t)UVAL_CLOSURE) {
        vm->last_error = UVM_TYPE_ERROR;
        vm_format_type_error_msg(vm,
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
   and install_at_event_runtime dereferences garbage.

   Routes through uvalue_is_event() rather than a raw kind comparison
   so the predicate location stays single-source-of-truth (T29's
   refactor pattern). */
static int
vm_install_check_event_operand(UVM *vm, const UStrand *s, uint8_t reg,
                               const char *opcode_name)
{
    if (!uvalue_is_event(s->R[reg])) {
        vm->last_error = UVM_TYPE_ERROR;
        vm_format_type_error_msg(vm,
            "AT_EVENT install: register operand is not an event");
        (void)opcode_name;
        return 0;
    }
    return 1;
}

/* --- vm_install_result_is_fatal / vm_install_fault (VM-002, VM-012) ---
   Translate a UWatcherInstallResult from install_watcher_runtime /
   install_at_event_runtime into a VM fault.  Prior to v0.5.7-fixes Phase 5
   the install opcodes ignored the return value entirely, so OOM-pool /
   trace-fault / recursive-install errors became silent no-ops: the strand
   sailed past a watcher that was never armed, with no observable diagnostic
   beyond a host_log_fn warning.  This pair makes the dispatcher promote
   those errors to UVM faults so the program halts cleanly instead of
   continuing with broken reactive semantics.

   READSET_OVER is currently treated as fatal as well: today
   install_watcher_runtime returns it from Phase 4 (before pool_alloc) so
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
    return r != URBI_INSTALL_OK;
}

static void
vm_install_fault(UVM *vm, UWatcherInstallResult r, const char *opcode_name)
{
    switch (r) {
        case URBI_INSTALL_OOM_POOL:
            vm->last_error = UVM_OOM;
            vm_format_oom(vm, sizeof(struct UWatcher));
            break;
        case URBI_INSTALL_READSET_OVER:
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm,
                "watcher install: read-set exceeds URBI_WATCHER_READSET_MAX");
            break;
        case URBI_INSTALL_TRACE_FAULT:
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm,
                "watcher install: condition threw during trace");
            break;
        case URBI_INSTALL_RECURSIVE:
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm,
                "watcher install attempted from within scratch-frame eval");
            break;
        case URBI_INSTALL_NO_OBSERVABLE_CELLS:
            /* W0/v0.10.2: cond watcher with empty read-set is a program error.
             * Use `whenever (e?) body` for event-driven subscriptions. */
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm,
                "watcher install: condition observes no slots; "
                "use 'whenever (e?) body' for event subscriptions");
            break;
        case URBI_INSTALL_OK:
        default:
            /* Caller should not invoke this on URBI_INSTALL_OK.  Defensive. */
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm, "watcher install: unknown result");
            break;
    }
    (void)opcode_name;  /* available for future diagnostic enrichment */
}

UVmReactiveInstallResult
vm_reactive_install(UVM *vm, UStrand *s, uint8_t op)
{
    switch (op) {
    case OP_AT_INSTALL: {
        uint8_t A = uinstr_a(*s->pc);
        uint8_t B = uinstr_b(*s->pc);
        uint8_t C = uinstr_c(*s->pc);
        if (!vm_install_check_closure_operand(vm, s, A, "OP_AT_INSTALL", "cond")) return UVM_INSTALL_HALT;
        if (!vm_install_check_closure_operand(vm, s, B, "OP_AT_INSTALL", "body")) return UVM_INSTALL_HALT;
        if (C != 0xFFU
            && !vm_install_check_closure_operand(vm, s, C, "OP_AT_INSTALL", "onleave"))
            return UVM_INSTALL_HALT;
        UClosure *cond    = (UClosure *)s->R[A].v.p;
        UClosure *body    = (UClosure *)s->R[B].v.p;
        UClosure *onleave = (C == 0xFFU) ? NULL : (UClosure *)s->R[C].v.p;
        UWatcherInstallResult r =
            install_watcher_runtime(vm, s, UWATCHER_AT, cond, body, onleave, NULL);
        if (vm_install_result_is_fatal(r)) {
            vm_install_fault(vm, r, "OP_AT_INSTALL");
            return UVM_INSTALL_HALT;
        }
        return UVM_INSTALL_NEXT;
    }

    case OP_AT_SYNC_INSTALL: {
        uint8_t A = uinstr_a(*s->pc);
        uint8_t B = uinstr_b(*s->pc);
        if (!vm_install_check_closure_operand(vm, s, A, "OP_AT_SYNC_INSTALL", "cond")) return UVM_INSTALL_HALT;
        if (!vm_install_check_closure_operand(vm, s, B, "OP_AT_SYNC_INSTALL", "body")) return UVM_INSTALL_HALT;
        UClosure *cond = (UClosure *)s->R[A].v.p;
        UClosure *body = (UClosure *)s->R[B].v.p;
        UWatcherInstallResult r =
            install_watcher_runtime(vm, s, UWATCHER_AT_SYNC, cond, body, NULL, NULL);
        if (vm_install_result_is_fatal(r)) {
            vm_install_fault(vm, r, "OP_AT_SYNC_INSTALL");
            return UVM_INSTALL_HALT;
        }
        return UVM_INSTALL_NEXT;
    }

    case OP_WHENEVER_INSTALL: {
        uint8_t A = uinstr_a(*s->pc);
        uint8_t B = uinstr_b(*s->pc);
        uint8_t C = uinstr_c(*s->pc);
        if (!vm_install_check_closure_operand(vm, s, A, "OP_WHENEVER_INSTALL", "cond")) return UVM_INSTALL_HALT;
        if (!vm_install_check_closure_operand(vm, s, B, "OP_WHENEVER_INSTALL", "body")) return UVM_INSTALL_HALT;
        if (C != 0xFFU
            && !vm_install_check_closure_operand(vm, s, C, "OP_WHENEVER_INSTALL", "onleave"))
            return UVM_INSTALL_HALT;
        UClosure *cond    = (UClosure *)s->R[A].v.p;
        UClosure *body    = (UClosure *)s->R[B].v.p;
        UClosure *onleave = (C == 0xFFU) ? NULL : (UClosure *)s->R[C].v.p;
        UWatcherInstallResult r =
            install_watcher_runtime(vm, s, UWATCHER_WHENEVER, cond, body, onleave, NULL);
        if (vm_install_result_is_fatal(r)) {
            vm_install_fault(vm, r, "OP_WHENEVER_INSTALL");
            return UVM_INSTALL_HALT;
        }
        return UVM_INSTALL_NEXT;
    }

    /* === T42: OP_WAITUNTIL_INSTALL — strand-block or pass-through ===
     *
     * A-encoded: A = cond_reg.
     *
     * Calls install_watcher_runtime which either:
     *   (a) fast-path: cond was truthy at install → watcher unregistered
     *       immediately, strand state unchanged (still RUNNING) → NEXT().
     *   (b) park path: cond was falsy → install parked the strand via
     *       sched_strand_block(USTRAND_REASON_WATCHER) (refactor-3 SCHED-01:
     *       block owns the runnable-count decrement).  Here we advance pc
     *       past this instruction and goto exit_strand so the scheduler can
     *       pick up another strand.  The eval-pass wake (T43) will resume
     *       the strand on the rising edge.
     *
     * Spec #2 §6.3. */
    case OP_WAITUNTIL_INSTALL: {
        uint8_t A = uinstr_a(*s->pc);
        if (!vm_install_check_closure_operand(vm, s, A, "OP_WAITUNTIL_INSTALL", "cond"))
            return UVM_INSTALL_HALT;
        UClosure *cond = (UClosure *)s->R[A].v.p;
        UWatcherInstallResult r = install_watcher_runtime(
            vm, s, UWATCHER_WAITUNTIL, cond, NULL, NULL, s);
        if (vm_install_result_is_fatal(r)) {
            vm_install_fault(vm, r, "OP_WAITUNTIL_INSTALL");
            return UVM_INSTALL_HALT;
        }
        if (r == URBI_INSTALL_OK && USTRAND_IS_WAITING(s)) {
            /* Strand parked (cond started false).  Advance pc past this
             * instruction so resume lands at the correct next opcode.
             * Runnable-count accounting is owned by sched_strand_block
             * inside install_watcher_runtime (SCHED-01) — no manual
             * adjustment here.  The arm completes the exit with
             * `steps_consumed++; goto exit_strand;`. */
            s->pc++;
            return UVM_INSTALL_PARK_EXIT;
        }
        /* Fast path (cond was truthy): watcher unregistered; strand RUNNING.
         * Fall through to next instruction. */
        return UVM_INSTALL_NEXT;
    }

    /* === T47: OP_AT_EVENT_INSTALL / OP_AT_EVENT_SYNC_INSTALL ===
     *
     * ABC-encoded: A = event_reg, B = body_reg, C = onleave_reg (0xFF = absent).
     * Routes through install_at_event_runtime — no read-set trace, no
     * active_watchers_head linkage.  Watcher joins event->at_watchers_head
     * (FIFO) and owning_tag's member chain.
     * Spec #3 §6.2. */
    case OP_AT_EVENT_INSTALL: {
        uint8_t A = uinstr_a(*s->pc);
        uint8_t B = uinstr_b(*s->pc);
        uint8_t C = uinstr_c(*s->pc);
        if (!vm_install_check_event_operand(vm, s, A, "OP_AT_EVENT_INSTALL"))
            return UVM_INSTALL_HALT;
        if (!vm_install_check_closure_operand(vm, s, B, "OP_AT_EVENT_INSTALL", "body"))
            return UVM_INSTALL_HALT;
        if (C != 0xFFU
            && !vm_install_check_closure_operand(vm, s, C, "OP_AT_EVENT_INSTALL", "onleave"))
            return UVM_INSTALL_HALT;
        UEvent   *e       = (UEvent *)s->R[A].v.p;
        UClosure *body    = (UClosure *)s->R[B].v.p;
        UClosure *onleave = (C == 0xFFU) ? NULL : (UClosure *)s->R[C].v.p;
        UWatcherInstallResult r =
            install_at_event_runtime(vm, s, UWATCHER_AT_EVENT, e, body, onleave);
        if (vm_install_result_is_fatal(r)) {
            vm_install_fault(vm, r, "OP_AT_EVENT_INSTALL");
            return UVM_INSTALL_HALT;
        }
        return UVM_INSTALL_NEXT;
    }

    case OP_AT_EVENT_SYNC_INSTALL: {
        uint8_t A = uinstr_a(*s->pc);
        uint8_t B = uinstr_b(*s->pc);
        uint8_t C = uinstr_c(*s->pc);
        if (!vm_install_check_event_operand(vm, s, A, "OP_AT_EVENT_SYNC_INSTALL"))
            return UVM_INSTALL_HALT;
        if (!vm_install_check_closure_operand(vm, s, B, "OP_AT_EVENT_SYNC_INSTALL", "body"))
            return UVM_INSTALL_HALT;
        if (C != 0xFFU
            && !vm_install_check_closure_operand(vm, s, C, "OP_AT_EVENT_SYNC_INSTALL", "onleave"))
            return UVM_INSTALL_HALT;
        UEvent   *e       = (UEvent *)s->R[A].v.p;
        UClosure *body    = (UClosure *)s->R[B].v.p;
        UClosure *onleave = (C == 0xFFU) ? NULL : (UClosure *)s->R[C].v.p;
        UWatcherInstallResult r =
            install_at_event_runtime(vm, s, UWATCHER_AT_EVENT_SYNC, e, body, onleave);
        if (vm_install_result_is_fatal(r)) {
            vm_install_fault(vm, r, "OP_AT_EVENT_SYNC_INSTALL");
            return UVM_INSTALL_HALT;
        }
        return UVM_INSTALL_NEXT;
    }

    /* === W0/v0.10.2: OP_WHENEVER_EVENT_INSTALL ===
     *
     * ABC-encoded: A = event_reg, B = body_reg, C = onleave_reg (0xFF = absent).
     * Identical shape to OP_AT_EVENT_INSTALL but arms the watcher with
     * UWATCHER_WHENEVER_EVENT mode.  The body re-fires on every event emission
     * (perpetual subscriber — no one-shot teardown).  Closes reactive F1. */
    case OP_WHENEVER_EVENT_INSTALL: {
        uint8_t A = uinstr_a(*s->pc);
        uint8_t B = uinstr_b(*s->pc);
        uint8_t C = uinstr_c(*s->pc);
        if (!vm_install_check_event_operand(vm, s, A, "OP_WHENEVER_EVENT_INSTALL"))
            return UVM_INSTALL_HALT;
        if (!vm_install_check_closure_operand(vm, s, B, "OP_WHENEVER_EVENT_INSTALL", "body"))
            return UVM_INSTALL_HALT;
        if (C != 0xFFU
            && !vm_install_check_closure_operand(vm, s, C, "OP_WHENEVER_EVENT_INSTALL", "onleave"))
            return UVM_INSTALL_HALT;
        UEvent   *ev      = (UEvent *)s->R[A].v.p;
        UClosure *body    = (UClosure *)s->R[B].v.p;
        UClosure *onleave = (C == 0xFFU) ? NULL : (UClosure *)s->R[C].v.p;
        UWatcherInstallResult r =
            install_at_event_runtime(vm, s, UWATCHER_WHENEVER_EVENT, ev, body, onleave);
        if (vm_install_result_is_fatal(r)) {
            vm_install_fault(vm, r, "OP_WHENEVER_EVENT_INSTALL");
            return UVM_INSTALL_HALT;
        }
        return UVM_INSTALL_NEXT;
    }

    default:
        /* Unreachable: the dispatch arm passes only the seven install opcodes.
         * Defensive — set an error so a HALT() has a populated last_error. */
        URBI_INTERNAL_ASSERT(0);
        vm->last_error = UVM_TYPE_ERROR;
        vm_format_type_error_msg(vm, "reactive install: unknown opcode");
        return UVM_INSTALL_HALT;
    }
}
