/* SPDX-License-Identifier: BSD-3-Clause */
/* uunwind.c — M3 control-transfer walker.
 *
 * T9: real 3-kind walker replacing T8's bridging stub.
 * Handles UEXEC_RETURN, UEXEC_THROW, and UEXEC_TAG_STOP (stub) via
 * a cleanup-stack walk that processes CALL_FRAME, TRY_FRAME, and TAG_SCOPE
 * entries from innermost to outermost.
 *
 * Backward compatibility: when cleanup_depth == 0 and pending_unwind ==
 * UEXEC_RETURN, the walker performs the direct frame-pop path (identical
 * to T8's bridging stub) so all M2/M3 existing tests continue to pass.
 * T11 (OP_PUSH_FRAME_GUARD emit) will push CALL_FRAME entries so the
 * walker's CALL_FRAME branch handles it instead; the direct-pop path
 * remains the fallback for frames without an associated guard entry.
 *
 * Replace-on-raise (C-1): when a finally/onleave body raises a new unwind
 * during run_cleanup_with_replace(), the new pending state wins and the
 * original is silently suppressed.  Warning emission deferred to T16/T19.
 *
 * Recursion bound: run_cleanup_with_replace() re-enters dispatch_loop_until_yield.
 * Maximum depth is bounded by URBI_CLEANUP_MAX (default 64, footprint preset 16).
 * At 64 levels and ~8 KB per frame: ~512 KB max — safe on host.
 * TODO T20: evaluate non-recursive cleanup executor if Cortex-M stack budget
 * proves insufficient at URBI_CLEANUP_MAX=16. */

#include "uunwind.h"
#include "ustrand.h"
#include "uframe.h"       /* UCallFrame */
#include "ucleanup.h"     /* UCleanupEntry, UCleanupKind, FLAG_* */
#include "uvm_internal.h" /* vm_close_upvalues */
#include "uvm.h"          /* dispatch_loop_until_yield */

/* ===== Freestanding-safe zero loop =====
   No memset; mirrors the volatile-byte pattern from uarena.c and ucleanup.c. */
static void
zero_registers(UStrand *s, uint16_t base, uint16_t count)
{
    uint16_t i;
    for (i = 0; i < count; i++) {
        UValue z;
        z.kind = 0;
        z.v.i  = 0;
        s->stack[base + i] = z;
    }
}

/* ===== M3 stubs for types/functions that land in later tasks =====

   pattern_matches: M3 catch-everything stub.  T10/M5 refine to actual
                    pattern dispatch (exception class matching, etc.).
   bind_catch_value: no-op stub.  T10 will emit a MOVE to bind the caught
                     value into the catch-clause's named register. */

struct UPattern;  /* forward-decl; struct definition lands at T10 */

static int
pattern_matches(struct UPattern *pat, UValue val)
{
    /* M3 stub: always match — any throw is caught by any catch clause. */
    (void)pat; (void)val;
    return 1;
}

static void
bind_catch_value(UStrand *s, struct UPattern *pat, UValue val)
{
    /* M3 stub: no-op.  T10 will write val into the catch register. */
    (void)s; (void)pat; (void)val;
}

/* ===== pop_call_frame: restore caller's execution context =====
   Called from CALL_FRAME branch and the backward-compat direct-pop path.
   After this returns, s->R points to the caller's register base and
   s->pc points one past the OP_CALL instruction. */
static void
pop_call_frame(UStrand *s)
{
    UCallFrame *done = &s->frames[--s->frame_count];

    /* Close any open upvalue cells pointing into this frame's registers.
     * Threshold: one past the result-destination slot in the caller's window,
     * covering all locals allocated above it in the callee's window. */
    vm_close_upvalues(s, done->base + done->result_dest_reg + 1,
                      &s->closed_cells);

    /* Restore caller's register window, instruction pointer, and constant pool. */
    s->R       = done->base;
    s->pc      = done->pc + 1;   /* advance past the OP_CALL instruction */
    s->pc_base = s->module->instructions;
    s->cur_consts = (s->frame_count > 0 &&
                     s->frames[s->frame_count - 1].closure != NULL)
                    ? s->frames[s->frame_count - 1].closure->proto->constants
                    : s->module->constants;
}

/* ===== deliver_return_value: write retval into caller's result slot =====
   Must be called AFTER pop_call_frame() restores s->R to caller's base.
   result_dest_reg is relative to the caller's register base. */
static void
deliver_return_value(UStrand *s, UValue val, int result_dest_reg)
{
    s->R[result_dest_reg] = val;
}

/* ===== run_cleanup_with_replace: execute a finally/onleave body =====
   C-1 replace-on-raise policy:
   - If the cleanup body completes normally (UEXEC_OK), restore the original
     unwind state so the walker continues propagating it outward.
   - If the cleanup body raises a new unwind, the new state wins and the
     original is silently suppressed (warning deferred to T16/T19 diag infra).

   Re-enters dispatch_loop_until_yield; recursion depth bounded by
   URBI_CLEANUP_MAX (see file-level comment). */
static void
run_cleanup_with_replace(UStrand *s, uint16_t handler_pc)
{
    UExecStatus saved_unwind = s->pending_unwind;
    UValue       saved_value = s->unwind_value;
    UValue       nil;

    nil.kind = 0;
    nil.v.i  = 0;

    /* Clear unwind state so the cleanup body executes as normal code. */
    s->pending_unwind = UEXEC_OK;
    s->unwind_value   = nil;
    s->pc = s->module->instructions + handler_pc;

    /* Run the cleanup body until it completes, yields, or raises a new unwind.
     * dispatch_loop_until_yield exits via the safepoint path if pending_unwind
     * becomes non-OK; urbi_unwind is called recursively from that safepoint.
     * Recursion depth is bounded by URBI_CLEANUP_MAX. */
    (void)dispatch_loop_until_yield(s, /*step_budget*/ 4096);

    if (s->pending_unwind == UEXEC_OK) {
        /* Cleanup body completed normally: restore the original unwind state. */
        s->pending_unwind = saved_unwind;
        s->unwind_value   = saved_value;
    } else {
        /* C-1: cleanup body raised a new unwind; new state wins.
         * Original saved values are silently dropped.
         * TODO: emit URBI_WARN_SUPPRESSED_UNWIND once diagnostic infra
         *       lands at T16/T19. */
    }
}

/* ===== urbi_unwind: main 3-kind walker =====
   Called from the safepoint in dispatch_loop_until_yield when
   s->pending_unwind != UEXEC_OK.  Walks the cleanup stack from innermost
   (top) to outermost (bottom), processing each entry by kind.

   On return:
   - s->pending_unwind == UEXEC_OK and strand continues running, OR
   - s->state == USTRAND_STATE_DEAD and the strand has terminated. */
void
urbi_unwind(UStrand *s)
{
    UValue nil;
    nil.kind = 0;
    nil.v.i  = 0;

    /* Fast path: nothing to unwind (defensive; safepoint guards this). */
    if (s->pending_unwind == UEXEC_OK)
        return;

    /* Backward-compat direct-pop path (mirrors T8 bridging stub):
     * When there is no CALL_FRAME cleanup entry (cleanup_depth == 0) but
     * we have a RETURN pending and are inside a call (frame_count > 0),
     * pop the frame directly without walking the cleanup stack.
     * T11 (OP_PUSH_FRAME_GUARD) will push CALL_FRAME entries; until then,
     * this path ensures all M2/M3 function-return tests keep passing.
     *
     * Note: if cleanup_depth > 0, the walker loop handles everything including
     * CALL_FRAME entries (T11 forward). */
    if (s->cleanup_depth == 0) {
        if (s->pending_unwind == UEXEC_RETURN && s->frame_count > 0) {
            UValue retval = s->unwind_value;
            int result_reg = s->frames[s->frame_count - 1].result_dest_reg;
            pop_call_frame(s);
            deliver_return_value(s, retval, result_reg);
            s->pending_unwind = UEXEC_OK;
            s->unwind_value   = nil;
            return;
        }
        /* Any other unwind with empty cleanup stack → fatal escalation. */
        goto fatal;
    }

    /* Walk cleanup-stack entries from innermost to outermost. */
    while (s->cleanup_depth > 0) {
        UCleanupEntry *e = &s->cleanup_base[s->cleanup_depth - 1];
        UCleanupKind kind = (UCleanupKind)e->kind;

        /* Inv-5 (row 7 §7.1): zero the frame's register range before running
         * any cleanup body or popping the entry.  Ensures dangling register
         * references from closed upvalues see null values after scope exit. */
        if (e->register_count > 0) {
            zero_registers(s, e->register_base, e->register_count);
        }

        switch (kind) {

        case UCLEANUP_CALL_FRAME: {
            /* CALL_FRAME: function call boundary.
             * UEXEC_RETURN is absorbed at the first CALL_FRAME encountered.
             * THROW / TAG_STOP / CANCEL propagate past the frame boundary. */

            /* Capture result_dest_reg from s->frames[] BEFORE pop_call_frame
             * decrements s->frame_count; both must be in sync. */
            int result_reg = (s->frame_count > 0)
                ? s->frames[s->frame_count - 1].result_dest_reg
                : 0;

            /* Pop the cleanup entry. */
            strand_cleanup_pop(s, UCLEANUP_CALL_FRAME);

            if (s->pending_unwind == UEXEC_RETURN) {
                /* Absorb: restore caller frame and deliver the return value. */
                UValue retval = s->unwind_value;
                pop_call_frame(s);            /* restores s->R to caller's base */
                deliver_return_value(s, retval, result_reg);
                s->pending_unwind = UEXEC_OK;
                s->unwind_value   = nil;
                return;  /* absorbed */
            }

            /* THROW / TAG_STOP / CANCEL: restore caller context and
             * continue propagating the unwind outward. */
            pop_call_frame(s);
            continue;
        }

        case UCLEANUP_TRY_FRAME: {
            /* TRY_FRAME: try-catch or try-finally boundary. */

            if (s->pending_unwind == UEXEC_THROW &&
                (e->flags & FLAG_HAS_CATCH) &&
                pattern_matches(e->catch_pattern, s->unwind_value)) {
                /* Catch absorption: bind caught value and jump to handler. */
                bind_catch_value(s, e->catch_pattern, s->unwind_value);
                uint16_t handler_pc = e->handler_pc;
                strand_cleanup_pop(s, UCLEANUP_TRY_FRAME);
                s->pc = s->module->instructions + handler_pc;
                s->pending_unwind = UEXEC_OK;
                s->unwind_value   = nil;
                return;  /* absorbed */
            }

            if (e->flags & FLAG_HAS_FINALLY) {
                /* Finally execution: run body, then continue unwinding.
                 * C-1 replace-on-raise: if body raises, new state wins. */
                uint16_t handler_pc = e->handler_pc;
                strand_cleanup_pop(s, UCLEANUP_TRY_FRAME);
                run_cleanup_with_replace(s, handler_pc);
                /* After run_cleanup_with_replace, s->pending_unwind is either
                 * the original (body OK) or a new one (C-1 replace).
                 * Either way, continue the walker loop. */
                continue;
            }

            /* No matching catch and no finally: pop and propagate. */
            strand_cleanup_pop(s, UCLEANUP_TRY_FRAME);
            continue;
        }

        case UCLEANUP_TAG_SCOPE: {
            /* TAG_SCOPE: M3 stub — T29 owns absorption (lands UTag).
             * Walker pops TAG_SCOPE entries and continues unwinding.
             * UEXEC_TAG_STOP will reach fatal escalation below, which is
             * correct at T9 since UTag doesn't exist as a real type yet.
             * TODO T29: implement tag-stop absorption per row 7 §6.1. */

            if (e->flags & FLAG_HAS_ONLEAVE) {
                /* onleave handler: run under C-1 replace-on-raise. */
                uint16_t handler_pc = e->handler_pc;
                strand_cleanup_pop(s, UCLEANUP_TAG_SCOPE);
                run_cleanup_with_replace(s, handler_pc);
                continue;
            }

            strand_cleanup_pop(s, UCLEANUP_TAG_SCOPE);
            continue;
        }

        default: {
            /* Unknown entry kind — safety net; pop and continue to avoid
             * an infinite loop on corrupted cleanup stack. */
            strand_cleanup_pop(s, (UCleanupKind)kind);
            continue;
        }
        } /* switch */
    } /* while cleanup_depth > 0 */

fatal:
    /* Cleanup stack exhausted (or was empty from the start) with unwind still
     * pending → unhandled control transfer.  Mark the strand dead. */
    s->fatal_status = s->pending_unwind;
    s->fatal_value  = s->unwind_value;
    s->state        = USTRAND_STATE_DEAD;
}
