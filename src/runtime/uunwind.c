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

#include "runtime/uunwind.h"
#include "sched/ustrand.h"
#include "runtime/uclosure.h"     /* UClosure full definition (M4: embeds UCell) */
#include "runtime/uframe.h"       /* UCallFrame */
#include "runtime/ucleanup.h"     /* UCleanupEntry, UCleanupKind, FLAG_* */
#include "vm/uvm.h"          /* dispatch_loop_until_yield */
#include "urbi/urbi.h"         /* UErrCode, public API declarations */
#include "sched/usched_cooperative.h" /* sched_strand_unblock, sched_strand_make_runnable */
#include "runtime/umacros.h"      /* URBI_INTERNAL_ASSERT */
#include "tag/utag.h"               /* UTag, member_strands_head */
#include "watcher/uwatcher.h"           /* pending_onleave_queue_push */
#include "event/uevent_emit.h"        /* uevent_waiter_unregister (spec #3 §6.4) */
#include <stddef.h>
#include <stdint.h>

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
 *
 * pattern_matches  (FOUND-026): M3 catch-everything stub.  Returns 1
 *                  unconditionally.  Wave 2 of M6 stdlib refines to
 *                  actual class-pattern dispatch (exception subclass
 *                  matching for `try { ... } catch (E e) { ... }`)
 *                  once class-decl AST_CLASS_DECL emit (Phase 6) lands
 *                  along with Wave 2's exception class hierarchy.
 * bind_catch_value (FOUND-027): no-op pattern parameter.  Wave 2 lands
 *                  the named-register binding when patterns gain
 *                  destructuring; for Wave 1 the catch variable is
 *                  always bound to the bare caught value (current
 *                  behaviour) and `pat` stays unused.
 *
 * Both stubs are not yet load-bearing at v1.0 — match-all is the
 * only behaviour exercised by the test corpus today.  Filed as Wave 2
 * deferral in the M6 stdlib design-risks register.
 *
 * struct UPattern is forward-declared in ucleanup.h (included above). */

static int
pattern_matches(struct UPattern *pat, UValue val)
{
    /* M3 stub: always match — any throw is caught by any catch clause.
     * FOUND-026: Wave 2 refines to class-pattern dispatch. */
    (void)pat; (void)val;
    return 1;
}

static void
bind_catch_value(UStrand *s, struct UPattern *pat, UValue val)
{
    /* T10: write the caught value into s->catch_value; the catch handler's
     * first instruction (OP_LOAD_CATCH_VALUE) reads it into the named
     * register.  FOUND-027: Wave 2 of M6 stdlib refines to use `pat` for
     * destructuring; until then the catch-var is always the bare value. */
    (void)pat;
    s->catch_value = val;
}

/* SCHED-001: discriminate "is parked on Event waiter chain?" by class+reason
 * rather than by full state-byte equality.  Pre-v0.5.5 the JOIN reason byte
 * collided with EVENT (both 0x03), making `s->state == USTRAND_WAIT_EVENT`
 * ambiguous; v0.5.5 (CHSTR-016) renumbered JOIN to 0x04 so the literal
 * composite is now distinct, but the architectural pattern of comparing
 * full-state bytes is fragile against any future reason renumbering.  This
 * helper isolates the predicate so additions to the WAITING reason space
 * cannot re-introduce an alias.
 *
 * Closes SCHED-001 (and the architectural follow-up to EMITR-001). */
static inline int
is_event_parked_strand(const UStrand *s)
{
    return ((s->state & USTRAND_STATE_MASK) == USTRAND_WAITING) &&
           ((s->state & USTRAND_REASON_MASK) == USTRAND_REASON_EVENT);
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
    /* Restore the caller's pc_base.  Three cases mirror the cur_consts /
     * origin_nested fallback chain used by ustrand_consts_for_closure +
     * OP_CLOSURE:
     *   - frame_count > 0  : returning into an outer call frame; use that
     *                        closure's proto instructions
     *   - module != NULL   : returning to chunk-top of a chunk-top strand
     *   - entry_closure    : returning to root of a non-chunk strand
     *                        (e.g. watcher body strand — s->module is NULL
     *                        for body strands; task #23, 2026-05-16).
     *
     * Each branch NULL-guards the chain so test mocks that leave closure /
     * module / entry_closure unset (e.g. test_unwind's hand-built strands
     * setting frames[i].closure = NULL) fall through to the next source. */
    if (s->frame_count > 0
        && s->frames[s->frame_count - 1].closure != NULL
        && s->frames[s->frame_count - 1].closure->proto != NULL) {
        s->pc_base = s->frames[s->frame_count - 1].closure->proto->instructions;
    } else if (s->module != NULL) {
        s->pc_base = s->module->instructions;
    } else if (s->entry_closure != NULL && s->entry_closure->proto != NULL) {
        s->pc_base = s->entry_closure->proto->instructions;
    }
    /* FOUND-032: route through the shared helper so the OP_CALL rule and the
     * pop-frame rule cannot drift. */
    {
        const UClosure *outer = (s->frame_count > 0)
            ? s->frames[s->frame_count - 1].closure
            : NULL;
        s->cur_consts = ustrand_consts_for_closure(s, outer);
    }
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
   URBI_CLEANUP_MAX (see file-level comment).

   T29 / FOUND-009: the recursion bound is now enforced via the per-strand
   cleanup_run_depth counter.  Before the guard a misbehaving cleanup body
   that itself raised an unwind whose handler raised again ad infinitum
   could exhaust the C stack.  Returns 0 on normal completion (including
   C-1 replace-on-raise) or URBI_ERR_CLEANUP_OVERFLOW when the recursion
   bound is hit — the walker handles the latter by marking the strand
   fatal and returning. */
static int
run_cleanup_with_replace(UStrand *s, uint16_t handler_pc)
{
    UExecStatus saved_unwind = s->pending_unwind;
    UValue       saved_value = s->unwind_value;
    UValue       nil;

    nil.kind = 0;
    nil.v.i  = 0;

    /* T29 / FOUND-009: enforce the documented URBI_CLEANUP_MAX recursion
     * bound.  Returning the error code (rather than escalating in place)
     * lets the walker handle the failure with the same fatal-state shape
     * it uses for unhandled THROW (avoids duplicating cleanup logic). */
    if (s->cleanup_run_depth >= (uint16_t)URBI_CLEANUP_MAX) {
        if (s->vm != NULL && s->vm->host_log_fn != NULL) {
            s->vm->host_log_fn(s->vm, URBI_LOG_ERROR,
                               "URBI_ERR_CLEANUP_OVERFLOW: run_cleanup_with_replace "
                               "exceeded URBI_CLEANUP_MAX recursion depth");
        }
        return URBI_ERR_CLEANUP_OVERFLOW;
    }
    s->cleanup_run_depth++;

    /* Clear unwind state so the cleanup body executes as normal code.
     * Task #24 / S44 (2026-05-16): use `s->pc_base` rather than
     * `s->module->instructions`.  The cleanup handler's PC is relative
     * to whichever proto emitted the try-finally — `s->pc_base` already
     * tracks that across method calls AND for body strands (where
     * `s->module` is NULL).  Before this fix, finally bodies in
     * non-chunk-top contexts dereferenced a stale or NULL pointer. */
    s->pending_unwind = UEXEC_OK;
    s->unwind_value   = nil;
    s->pc = s->pc_base + handler_pc;

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

    s->cleanup_run_depth--;
    return 0;
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

    /* RETURN absorption (T11 / T8 bridging stub).
     *
     * Two paths:
     *   (a) T11-forward: a CALL_FRAME cleanup entry exists on the stack.
     *       Let the walker run — it absorbs RETURN at the first CALL_FRAME
     *       and runs the register-zeroing pass (Inv-5, row 7 §7.1).
     *   (b) T11 deferred (today's production reality — no CALL_FRAME entries
     *       are pushed by bytecode): use the backward-compat direct-pop
     *       path REGARDLESS of cleanup_depth.  Earlier code gated direct-pop
     *       on cleanup_depth == 0, so any RETURN through an at-body (which
     *       carries a TAG_SCOPE entry) entered the walker, popped the
     *       TAG_SCOPE without absorbing, and fell through to fatal: —
     *       converting a legitimate return into a fatal_status (task #23,
     *       surfaced 2026-05-16 when an at-body invoked a scripted method).
     *
     * RETURN crosses exactly one call-frame boundary; intervening tag /
     * try scopes belong to the CALLER and must stay intact. */
    if (s->pending_unwind == UEXEC_RETURN && s->frame_count > 0) {
        int has_call_frame = 0;
        for (uint16_t i = 0; i < s->cleanup_depth; i++) {
            if (s->cleanup_base[i].kind == (uint8_t)UCLEANUP_CALL_FRAME) {
                has_call_frame = 1;
                break;
            }
        }
        if (!has_call_frame) {
            UValue retval = s->unwind_value;
            int result_reg = s->frames[s->frame_count - 1].result_dest_reg;
            pop_call_frame(s);
            deliver_return_value(s, retval, result_reg);
            s->pending_unwind = UEXEC_OK;
            s->unwind_value   = nil;
            return;
        }
        /* else fall through to walker — T11-forward absorbs at CALL_FRAME. */
    }

    if (s->cleanup_depth == 0) {
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

            /* pattern_matches is a documented M3 / FOUND-026 stub that
             * always returns 1 (match-all).  Wave 2 will refine to
             * class-pattern dispatch; until then the stub keeps the call
             * site stable for the spec evolution.  cppcheck flags the
             * tautology — suppressed file-wide in .cppcheck.suppressions
             * (knownConditionTrueFalse:src/runtime/uunwind.c).  Returns
             * to actionable status when pattern_matches gains class
             * dispatch and the condition can actually evaluate false. */
            if (s->pending_unwind == UEXEC_THROW &&
                (e->flags & FLAG_HAS_CATCH) &&
                pattern_matches(e->catch_pattern, s->unwind_value)) {
                /* Catch absorption: bind caught value and jump to handler.
                 * Task #24 / S44 (2026-05-16): use `s->pc_base` rather
                 * than `s->module->instructions`.  Handler PC is relative
                 * to the proto that emitted the try-catch, which `s->pc_base`
                 * already tracks (set on every OP_CALL, restored by
                 * `pop_call_frame`).  Before this fix, try-catch in a
                 * method invoked from an at-body crashed with a NULL
                 * deref on `s->module` (body strands have module=NULL). */
                bind_catch_value(s, e->catch_pattern, s->unwind_value);
                uint16_t handler_pc = e->handler_pc;
                strand_cleanup_pop(s, UCLEANUP_TRY_FRAME);
                s->pc = s->pc_base + handler_pc;
                s->pending_unwind = UEXEC_OK;
                s->unwind_value   = nil;
                return;  /* absorbed */
            }

            if (e->flags & FLAG_HAS_FINALLY) {
                /* Finally execution: run body, then continue unwinding.
                 * C-1 replace-on-raise: if body raises, new state wins.
                 * T29 / FOUND-009: helper returns URBI_ERR_CLEANUP_OVERFLOW
                 * when recursion depth would exceed URBI_CLEANUP_MAX. */
                uint16_t handler_pc = e->handler_pc;
                int      rc;
                strand_cleanup_pop(s, UCLEANUP_TRY_FRAME);
                rc = run_cleanup_with_replace(s, handler_pc);
                if (rc == URBI_ERR_CLEANUP_OVERFLOW) {
                    UValue overflow_marker;
                    overflow_marker.kind = (uint8_t)UVAL_INT;
                    overflow_marker.v.i  = (int64_t)URBI_ERR_CLEANUP_OVERFLOW;
                    s->fatal_status = UEXEC_CANCEL;
                    s->fatal_value  = overflow_marker;
                    s->state        = USTRAND_STATE_DEAD;
                    return;
                }
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
                /* onleave handler: run under C-1 replace-on-raise.
                 * T29 / FOUND-009: handle URBI_ERR_CLEANUP_OVERFLOW from helper. */
                uint16_t handler_pc = e->handler_pc;
                int      rc;
                strand_cleanup_pop(s, UCLEANUP_TAG_SCOPE);
                rc = run_cleanup_with_replace(s, handler_pc);
                if (rc == URBI_ERR_CLEANUP_OVERFLOW) {
                    UValue overflow_marker;
                    overflow_marker.kind = (uint8_t)UVAL_INT;
                    overflow_marker.v.i  = (int64_t)URBI_ERR_CLEANUP_OVERFLOW;
                    s->fatal_status = UEXEC_CANCEL;
                    s->fatal_value  = overflow_marker;
                    s->state        = USTRAND_STATE_DEAD;
                    return;
                }
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
            s->cross_strand_stop_pending = 1U;
            vm->host_call_pending_count++;
        }

        /* Unlink from event waiter chain before waking (spec #3 §6.4).
         * Must happen before state transition so the waiter list is consistent
         * when the strand next runs.  Idempotent if not on an event chain.
         * Use the class+reason discriminator (SCHED-001) rather than full-
         * state byte equality so future reason-byte additions cannot alias. */
        if (is_event_parked_strand(s))
            uevent_waiter_unregister(s);

        /* Wake any blocked strand so it can consume the unwind. */
        if (USTRAND_IS_WAITING(s)) {
            if (USTRAND_GET_REASON(s) == USTRAND_REASON_SLEEP)
                sched_strand_unblock(s);   /* removes from sleep_q + makes runnable */
            else
                sched_strand_make_runnable(s);  /* EVENT / JOIN / other reason */
        }
    }

    /* (1b) Mark the tag as stopped so urbi_tag_info can report URBI_TAG_STOPPED.
     * UTAG_FLAG_STOPPED was declared at v0.5.x as RESERVED; v0.7.1 activates it
     * here (Gap M) so the public urbi_tag_state_t surface reflects real state. */
    tag->flags |= UTAG_FLAG_STOPPED;

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
urbi_strand_cancel(struct UStrand *strand, UValue cancel_reason)
{
    if (!strand) return URBI_ERR_INVALID_ARG;
    if (strand->vm) { URBI_ASSERT_NOT_ISR(strand->vm); }
    if (USTRAND_GET_STATE(strand) == USTRAND_DEAD) return URBI_ERR_STRAND_FATAL;
    strand->pending_unwind = UEXEC_CANCEL;
    strand->unwind_value   = cancel_reason;
    /* If the strand is sleeping/waiting, unblock it so it can process the
     * unwind.  USTRAND_IS_WAITING checks the upper nibble of strand->state.
     * Unlink from event waiter chain first (spec #3 §6.4).  Use the class+
     * reason discriminator (SCHED-001) rather than full-state byte equality. */
    if (USTRAND_IS_WAITING(strand)) {
        if (is_event_parked_strand(strand))
            uevent_waiter_unregister(strand);
        strand->state = USTRAND_STATE_READY;
    }
    return URBI_OK;
}

/* urbi_strand_panic — skip walker, mark strand DEAD immediately.
 * No cleanup runs.  The msg parameter is for diagnostic context; at M3 it
 * is not stored (no string heap) — T16/T19 diagnostic infra will wire it.
 * The fatal_value is set to nil; T29 may upgrade to a string UValue. */
int
urbi_strand_panic(struct UStrand *strand, const char *msg)
{
    UValue nil;
    nil.kind  = UVAL_NIL;
    nil.v.i   = 0;

    if (!strand) return URBI_ERR_INVALID_ARG;
    if (strand->vm) { URBI_ASSERT_NOT_ISR(strand->vm); }
    /* FOUND-045: route diagnostic msg through host_log_fn before marking the
     * strand dead so embedders can correlate panic causes with their own
     * logging pipeline.  URBI_LOG_FATAL is not defined; use ERROR (highest
     * level we have).  NULL-guarded — many tests wire vm without a log
     * callback. */
    if (msg != NULL && strand->vm != NULL && strand->vm->host_log_fn != NULL) {
        strand->vm->host_log_fn(strand->vm, (int)URBI_LOG_ERROR, "%s", msg);
    }
    /* Unlink from event waiter chain before marking dead (spec #3 §6.4).
     * Prevents stale pointers in e->waiters_head if the strand is freed
     * without ever being woken by an emit.  Use the class+reason
     * discriminator (SCHED-001) rather than full-state byte equality. */
    if (is_event_parked_strand(strand))
        uevent_waiter_unregister(strand);
    strand->fatal_status = UEXEC_CANCEL;
    strand->fatal_value  = nil;
    strand->state        = USTRAND_STATE_DEAD;
    /* pending_unwind stays as-is; the strand is immediately dead.
     * No cleanup runs — panic is the "kill unconditionally" path. */
    return URBI_OK;
}

/* urbi_strand_unwind_status — read pending unwind state (non-destructive). */
UExecStatus
urbi_strand_unwind_status(const struct UStrand *strand)
{
    if (strand && strand->vm) { URBI_ASSERT_NOT_ISR(strand->vm); }
    return strand ? strand->pending_unwind : UEXEC_OK;
}

/* urbi_strand_is_fatal — query whether the strand has hit a fatal unwind.
 * Returns true and populates out_status / out_value (both nullable) if fatal. */
bool
urbi_strand_is_fatal(const struct UStrand *strand,
                     UExecStatus *out_status, UValue *out_value)
{
    if (strand && strand->vm) { URBI_ASSERT_NOT_ISR(strand->vm); }
    if (!strand || strand->fatal_status == UEXEC_OK) return false;
    if (out_status) *out_status = strand->fatal_status;
    if (out_value)  *out_value  = strand->fatal_value;
    return true;
}

/* urbi_strand_reset — REPL session restart: clear all unwind / fatal state,
 * reset cleanup-stack depth, return strand to DORMANT.
 * Does not free or reallocate memory.  The register window (stack/R) is
 * left intact; callers are expected to re-initialise it per their session
 * semantics before the next dispatch. */
int
urbi_strand_reset(struct UStrand *strand)
{
    UValue nil;
    nil.kind = UVAL_NIL;
    nil.v.i  = 0;

    if (!strand) return URBI_ERR_INVALID_ARG;
    if (strand->vm) { URBI_ASSERT_NOT_ISR(strand->vm); }

    strand->pending_unwind    = UEXEC_OK;
    strand->unwind_value      = nil;
    strand->unwind_target     = NULL;
    strand->fatal_status      = UEXEC_OK;
    strand->fatal_value       = nil;
    strand->cleanup_depth     = 0;
    strand->cleanup_run_depth = 0;        /* T29 / FOUND-009: clear recursion counter */
    strand->cleanup_top       = NULL;
    strand->state             = USTRAND_STATE_DORMANT;
    return URBI_OK;
}

/* === Host-callback reentrance helpers ===
 *
 * These functions inject control-transfer events from inside a host C
 * callback that is executing on the strand's behalf.  The dispatch loop
 * reads s->pending_unwind when the callback returns and starts unwinding.
 */

/* urbi_throw — deposit THROW unwind (equiv to bytecode OP_THROW).
 * API-002: NULL strand or NULL strand->vm is a no-op (defensive); the prior
 * code derefed strand to read strand->vm and would crash on either. */
void
urbi_throw(struct UStrand *strand, UValue value)
{
    if (!strand || !strand->vm) return;
    URBI_ASSERT_NOT_ISR(strand->vm);
    strand->pending_unwind = UEXEC_THROW;
    strand->unwind_value   = value;
}

/* urbi_return_val — deposit RETURN unwind (equiv to bytecode OP_RETURN).
 * Named urbi_return_val (not urbi_return) to avoid conflict with the C
 * keyword `return` in macro expansion contexts and to be unambiguous.
 * API-002: NULL strand or NULL strand->vm is a no-op. */
void
urbi_return_val(struct UStrand *strand, UValue value)
{
    if (!strand || !strand->vm) return;
    URBI_ASSERT_NOT_ISR(strand->vm);
    strand->pending_unwind = UEXEC_RETURN;
    strand->unwind_value   = value;
}

/* urbi_tag_stop_local — deposit TAG_STOP from within the same strand.
 * API-002: NULL strand or NULL strand->vm is a no-op. */
void
urbi_tag_stop_local(struct UStrand *strand, struct UTag *tag, UValue value)
{
    if (!strand || !strand->vm) return;
    URBI_ASSERT_NOT_ISR(strand->vm);
    strand->pending_unwind  = UEXEC_TAG_STOP;
    strand->unwind_target   = tag;
    strand->unwind_value    = value;
}
