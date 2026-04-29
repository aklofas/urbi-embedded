/* SPDX-License-Identifier: BSD-3-Clause */
/* uunwind.c — M3 control-transfer walker + row 7 C API (T12).
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
#include "uvm.h"          /* dispatch_loop_until_yield */
#include "urbi.h"         /* UErrCode, public API declarations */
#include "usched_cooperative.h" /* sched_strand_unblock, sched_strand_make_runnable */
#include "umacros.h"      /* URBI_INTERNAL_ASSERT */
#include "utag.h"               /* UTag, member_strands_head */
#include "uwatcher.h"           /* pending_onleave_queue_push */

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
                     value into the catch-clause's named register.

   struct UPattern is forward-declared in ucleanup.h (included above). */

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
    /* T10: write the caught value into s->catch_value; the catch handler's
     * first instruction (OP_LOAD_CATCH_VALUE) reads it into the named register.
     * Pattern is ignored at M3 (match-all stub); M5 refines to class dispatch. */
    (void)pat;
    s->catch_value = val;
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
         * Original saved values are silently dropped.  Emit a diagnostic so
         * embedders can detect inadvertent unwind-loss in their tag-leave
         * handlers.  host_log_fn is NULL-guarded per T19 pattern. */
        if (s->vm != NULL && s->vm->host_log_fn != NULL) {
            s->vm->host_log_fn(s->vm, URBI_LOG_WARN,
                               "URBI_WARN_SUPPRESSED_UNWIND: cleanup body raised; "
                               "original unwind dropped");
        }
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

/* ===== Row 7 public C API (T12) =====
 *
 * These functions expose strand control-transfer to host C code.
 * Thread safety: none at M3 — not ISR-safe.  T18 adds the ISR-safe path.
 * Functions that accept NULL for mandatory pointer args return URBI_ERR_INVALID_ARG.
 */

/* urbi_tag_stop — cross-strand TAG_STOP deposit (row 11 §3.5).
 *
 * Walks tag->member_strands_head and deposits UEXEC_TAG_STOP on every
 * member strand, honouring the row 7 C-1 priority rule:
 *   TAG_STOP overwrites OK / RETURN / THROW; does NOT overwrite CANCEL.
 *
 * For each fresh deposit, increments vm->host_call_pending_count once and
 * sets s->cross_strand_stop_pending = 1 (idempotent flag; decremented at
 * ustrand_destroy so sched_quiescent eventually converges).
 *
 * WAITING strands are woken via sched_strand_unblock (SLEEP) or
 * sched_strand_make_runnable (other reasons) so they run and process the
 * unwind before the scheduler reaches quiescence.
 *
 * Watcher cascade deferred to T34/T35 (UWatcher type not yet defined).
 * At M3 tag->member_watchers_head is always NULL.
 *
 * NOT ISR-safe.  Returns URBI_ERR_INVALID_ARG for NULL vm or tag. */
int
urbi_tag_stop(struct UVM *vm, struct UTag *tag, UValue value)
{
    UCleanupEntry *e;
    UCleanupEntry *next;

    if (!vm || !tag) return URBI_ERR_INVALID_ARG;
    URBI_ASSERT_NOT_ISR(vm);

    /* (1) Deposit pending TAG_STOP unwind on every member strand.
     * Snapshot next via UCleanupEntry.next_member — entries do not unlink
     * themselves during this walk; they unlink when the owning OP_POP_TAG /
     * row 7 walker pop fires. */
    for (e = tag->member_strands_head; e != NULL; e = next) {
        UStrand *s;
        bool fresh_deposit;

        next = e->next_member;
        s    = e->strand_back;
        URBI_INTERNAL_ASSERT(s != NULL);

        /* Row 7 C-1 priority: TAG_STOP wins over OK / RETURN / THROW;
         * TAG_STOP loses to CANCEL (don't overwrite).
         * A cross-strand TAG_STOP is a new event regardless of what the strand's
         * local pending unwind was — RETURN/THROW would have been replaced anyway,
         * so we count this as a fresh deposit for quiescence-counter purposes. */
        fresh_deposit = (s->pending_unwind == UEXEC_OK
                         || s->pending_unwind == UEXEC_RETURN
                         || s->pending_unwind == UEXEC_THROW);
        if (fresh_deposit) {
            s->pending_unwind = UEXEC_TAG_STOP;
            s->unwind_target  = tag;
            s->unwind_value   = value;
        }
        /* If already CANCEL or TAG_STOP, leave intact (idempotent). */

        /* host_call_pending_count: increment once per strand that receives
         * a fresh cross-strand deposit.  The cross_strand_stop_pending flag
         * is once-per-lifetime: set on first deposit, cleared only at ustrand_destroy.
         * Repeated deposits on a strand that already processed its TAG_STOP do not
         * re-increment (counter tracks lifetime cross-strand-affected strands, not
         * pending deposits). Counter is decremented at ustrand_destroy. */
        if (fresh_deposit && !s->cross_strand_stop_pending) {
            s->cross_strand_stop_pending = 1u;
            vm->host_call_pending_count++;
        }

        /* Wake any blocked strand so it can consume the unwind. */
        if (USTRAND_IS_WAITING(s)) {
            if (USTRAND_GET_REASON(s) == USTRAND_REASON_SLEEP)
                sched_strand_unblock(s);   /* removes from sleep_q + makes runnable */
            else
                sched_strand_make_runnable(s);  /* EVENT / JOIN / other reason */
        }
    }

    /* (2) Watcher cascade: push each watcher registered on this tag to the
     * pending-onleave queue.  Snapshot-next iteration since push unlinks each
     * watcher from tag->member_watchers_head as it goes. */
    {
        UWatcher *ww      = tag->member_watchers_head;
        UWatcher *ww_next;
        while (ww != NULL) {
            ww_next = ww->next_in_tag;
            pending_onleave_queue_push(vm, ww);
            ww = ww_next;
        }
    }

    /* (3) Return synchronously — all deposits are complete. */
    return URBI_OK;
}

/* urbi_strand_cancel — deposit CANCEL (fatal, no catch) on a strand. */
int
urbi_strand_cancel(struct UStrand *s, UValue cancel_reason)
{
    if (!s) return URBI_ERR_INVALID_ARG;
    if (s->vm) { URBI_ASSERT_NOT_ISR(s->vm); }
    if (USTRAND_GET_STATE(s) == USTRAND_DEAD) return URBI_ERR_STRAND_FATAL;
    s->pending_unwind = UEXEC_CANCEL;
    s->unwind_value   = cancel_reason;
    /* If the strand is sleeping/waiting, unblock it so it can process the
     * unwind.  USTRAND_IS_WAITING checks the upper nibble of s->state. */
    if (USTRAND_IS_WAITING(s))
        s->state = USTRAND_STATE_READY;
    return URBI_OK;
}

/* urbi_strand_panic — skip walker, mark strand DEAD immediately.
 * No cleanup runs.  The msg parameter is for diagnostic context; at M3 it
 * is not stored (no string heap) — T16/T19 diagnostic infra will wire it.
 * The fatal_value is set to nil; T29 may upgrade to a string UValue. */
int
urbi_strand_panic(struct UStrand *s, const char *msg)
{
    UValue nil;
    nil.kind  = UVAL_NIL;
    nil.v.i   = 0;

    if (!s) return URBI_ERR_INVALID_ARG;
    if (s->vm) { URBI_ASSERT_NOT_ISR(s->vm); }
    (void)msg;  /* stored as nil at M3; T16/T19 will emit a diagnostic string */
    s->fatal_status = UEXEC_CANCEL;
    s->fatal_value  = nil;
    s->state        = USTRAND_STATE_DEAD;
    /* pending_unwind stays as-is; the strand is immediately dead.
     * No cleanup runs — panic is the "kill unconditionally" path. */
    return URBI_OK;
}

/* urbi_strand_unwind_status — read pending unwind state (non-destructive). */
UExecStatus
urbi_strand_unwind_status(const struct UStrand *s)
{
    if (s && s->vm) { URBI_ASSERT_NOT_ISR(s->vm); }
    return s ? s->pending_unwind : UEXEC_OK;
}

/* urbi_strand_is_fatal — query whether the strand has hit a fatal unwind.
 * Returns true and populates out_status / out_value (both nullable) if fatal. */
bool
urbi_strand_is_fatal(const struct UStrand *s,
                     UExecStatus *out_status, UValue *out_value)
{
    if (s && s->vm) { URBI_ASSERT_NOT_ISR(s->vm); }
    if (!s || s->fatal_status == UEXEC_OK) return false;
    if (out_status) *out_status = s->fatal_status;
    if (out_value)  *out_value  = s->fatal_value;
    return true;
}

/* urbi_strand_reset — REPL session restart: clear all unwind / fatal state,
 * reset cleanup-stack depth, return strand to DORMANT.
 * Does not free or reallocate memory.  The register window (stack/R) is
 * left intact; callers are expected to re-initialise it per their session
 * semantics before the next dispatch. */
int
urbi_strand_reset(struct UStrand *s)
{
    UValue nil;
    nil.kind = UVAL_NIL;
    nil.v.i  = 0;

    if (!s) return URBI_ERR_INVALID_ARG;
    if (s->vm) { URBI_ASSERT_NOT_ISR(s->vm); }

    s->pending_unwind  = UEXEC_OK;
    s->unwind_value    = nil;
    s->unwind_target   = NULL;
    s->fatal_status    = UEXEC_OK;
    s->fatal_value     = nil;
    s->cleanup_depth   = 0;
    s->cleanup_top     = NULL;
    s->state           = USTRAND_STATE_DORMANT;
    return URBI_OK;
}

/* === Host-callback reentrance helpers ===
 *
 * These functions inject control-transfer events from inside a host C
 * callback that is executing on the strand's behalf.  The dispatch loop
 * reads s->pending_unwind when the callback returns and starts unwinding.
 */

/* urbi_throw — deposit THROW unwind (equiv to bytecode OP_THROW). */
void
urbi_throw(struct UStrand *s, UValue value)
{
    if (s->vm) { URBI_ASSERT_NOT_ISR(s->vm); }
    s->pending_unwind = UEXEC_THROW;
    s->unwind_value   = value;
}

/* urbi_return_val — deposit RETURN unwind (equiv to bytecode OP_RETURN).
 * Named urbi_return_val (not urbi_return) to avoid conflict with the C
 * keyword `return` in macro expansion contexts and to be unambiguous. */
void
urbi_return_val(struct UStrand *s, UValue value)
{
    if (s->vm) { URBI_ASSERT_NOT_ISR(s->vm); }
    s->pending_unwind = UEXEC_RETURN;
    s->unwind_value   = value;
}

/* urbi_tag_stop_local — deposit TAG_STOP from within the same strand. */
void
urbi_tag_stop_local(struct UStrand *s, struct UTag *tag, UValue value)
{
    if (s->vm) { URBI_ASSERT_NOT_ISR(s->vm); }
    s->pending_unwind  = UEXEC_TAG_STOP;
    s->unwind_target   = tag;
    s->unwind_value    = value;
}
