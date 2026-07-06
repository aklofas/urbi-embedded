/* SPDX-License-Identifier: BSD-3-Clause */
/* uunwind.c — control-transfer walker + row 7 C API.
 *
 * Handles UEXEC_RETURN, UEXEC_THROW, and UEXEC_TAG_STOP via a cleanup-stack
 * walk that processes CALL_FRAME, TRY_FRAME, and TAG_SCOPE entries from
 * innermost to outermost.
 *
 * The direct frame-pop path through frame_depth stamps is the permanent
 * canonical path (BLOCKED — the t11_backward_compat_path is
 * the live path; OP_PUSH_FRAME_GUARD was never wired by the emitter).
 * The walker's CALL_FRAME branch is present but unreachable until a future
 * bytecode extension pushes CALL_FRAME entries.
 *
 * Replace-on-raise (C-1): when a finally/onleave body raises a new unwind
 * during run_cleanup_with_replace(), the new pending state wins and the
 * original is silently suppressed.  Warning emission deferred to diag infra.
 *
 * Recursion bound: run_cleanup_with_replace() re-enters urbi_vm_dispatch_loop_until_yield.
 * Maximum depth is bounded by URBI_CLEANUP_MAX (default 64, footprint preset 16).
 * At 64 levels and ~8 KB per frame: ~512 KB max — safe on host.
 * TODO: evaluate non-recursive cleanup executor if Cortex-M stack budget
 * proves insufficient at URBI_CLEANUP_MAX=16. */

#include "runtime/uunwind.h"
#include "sched/ustrand.h"
#include "runtime/uclosure.h"     /* UClosure full definition (embeds UCell) */
#include "runtime/uframe.h"       /* UCallFrame */
#include "runtime/ucleanup.h"     /* UCleanupEntry, UCleanupKind, FLAG_* */
#include "vm/uvm.h"          /* urbi_vm_dispatch_loop_until_yield */
#include "vm/uvm_tag_scope.h"     /* urbi_vm_tag_scope_teardown (v0.10.15-B absorption) */
#include "urbi/urbi.h"         /* UErrCode, public API declarations */
#include "sched/usched_cooperative.h" /* urbi_sched_strand_unpark, urbi_sched_runnable_inc */
#include "runtime/umacros.h"      /* URBI_INTERNAL_ASSERT */
#include "runtime/ulist.h"        /* URBI_SLIST_FOREACH_SAFE */
#include "tag/utag.h"               /* UTag, member_strands_head */
#include "stdlib/temporal.h"        /* urbi_periodics_stop_owned_by (B5/SCHED-N2) */
#include "watcher/uwatcher.h"           /* urbi_watcher_pending_onleave_queue_push */
#include "runtime/uscratch.h"           /* URBI_SCRATCH_BUDGET_OPS */
#include <stddef.h>
#include <stdint.h>

/* Truncation guard (v0.13.1-E): safepoint_budget_remaining
 * is uint16_t; URBI_SCRATCH_BUDGET_OPS must fit without silent truncation. */
URBI_STATIC_ASSERT(URBI_SCRATCH_BUDGET_OPS > 0 && URBI_SCRATCH_BUDGET_OPS <= 65535,
                   "URBI_SCRATCH_BUDGET_OPS must fit uint16_t (v0.13.1-E)");

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

/* ===== v1.0 intentional behavior: match-all catch pattern =====
 *
 * pattern_matches  (NR-14 confirmed): returns 1 unconditionally.
 *                  Urbiscript v1.0 catch clauses match any thrown value
 *                  (catch-all semantics, NR-14).  Class-pattern dispatch
 *                  for `catch (ExceptionClass e)` is a v1.x extension.
 * bind_catch_value: catch variable is bound to the bare caught
 *                  value; `pat` is reserved for future destructuring syntax.
 *
 * struct UPattern is forward-declared in ucleanup.h (included above). */

static int
pattern_matches(struct UPattern *pat, UValue val)
{
    /* Catch-all: any throw is caught by any catch clause (NR-14 v1.0 behavior).
     * Class-pattern dispatch is a v1.x extension. */
    (void)pat; (void)val;
    return 1;
}

static void
bind_catch_value(UStrand *s, struct UPattern *pat, UValue val)
{
    /* Write the caught value into s->catch_value; the catch handler's
     * first instruction (OP_LOAD_CATCH_VALUE) reads it into the named
     * register.  A future stdlib pass refines to use `pat` for
     * destructuring; until then the catch-var is always the bare value. */
    (void)pat;
    s->catch_value = val;
}

/* (v0.13.3: the SCHED-001 is_event_parked_strand discriminator was deleted —
 * every former caller now routes through urbi_sched_strand_unpark, whose EVENT
 * arm dispatches on USTRAND_GET_REASON inside strand_unlink_park.) */

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
    urbi_vm_close_upvalues(s, done->base + done->result_dest_reg + 1);

    /* Restore caller's register window, instruction pointer, and constant pool. */
    s->R       = done->base;
    s->pc      = done->pc + 1;   /* advance past the OP_CALL instruction */
    /* Restore the caller's pc_base.  Three cases mirror the cur_consts
     * fallback chain used by ustrand_consts_for_closure:
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
    } else if (s->root_proto != NULL) {
        s->pc_base = s->root_proto->instructions;
    } else if (s->entry_closure != NULL && s->entry_closure->proto != NULL) {
        s->pc_base = s->entry_closure->proto->instructions;
    }
    /* Route through the shared helper so the OP_CALL rule and the
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
     original is silently suppressed (warning deferred to diag infra).

   Re-enters urbi_vm_dispatch_loop_until_yield; recursion depth bounded by
   URBI_CLEANUP_MAX (see file-level comment).

   The recursion bound is now enforced via the per-strand
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
    /* v0.13.1-L: the nested dispatch below can run GC
     * slices, and this C local is otherwise invisible to the GC (no
     * conservative C-stack scan) — with the try scope's registers already
     * zeroed (Inv-5), saved_value may be the suppressed RETURN value's
     * ONLY reference.  Pin it on the strand's C-stack root chain for the
     * duration; popped on every exit below the push. */
    UCRootFrame  saved_value_root;
    UValue       nil;

    nil.kind = 0;
    nil.v.i  = 0;

    /* Enforce the documented URBI_CLEANUP_MAX recursion
     * bound.  Returning the error code (rather than escalating in place)
     * lets the walker handle the failure with the same fatal-state shape
     * it uses for unhandled THROW (avoids duplicating cleanup logic). */
    if (s->cleanup_run_depth >= (uint16_t)URBI_CLEANUP_MAX) {
        if (s->vm != NULL && s->vm->host_log_fn != NULL) {
            s->vm->host_log_fn(s->vm, s->vm->host_log_ud, URBI_LOG_ERROR,
                               "URBI_ERR_CLEANUP_OVERFLOW: run_cleanup_with_replace "
                               "exceeded URBI_CLEANUP_MAX recursion depth");
        }
        return URBI_ERR_CLEANUP_OVERFLOW;
    }
    s->cleanup_run_depth++;
    /* v0.13.1-B (spec-review hazard 2): every cleanup run starts
     * un-absorbed.  Without this entry-clear, a flag left set by an earlier
     * run (absorption at a handler INSIDE the body, body then completed
     * normally — the !done consume branch never ran) would mask a later
     * GENUINE truncation in a different cleanup body as
     * "absorbed-and-continued", silently parking the strand instead of
     * escalating CLEANUP_OVERFLOW.  Absorption during THIS run's nested
     * dispatch re-sets the flag; the post-dispatch consumer below is
     * unchanged. */
    s->cleanup_absorbed = 0U;

    /* Pushed AFTER the early-exit guard above so every push has a matching
     * pop (VM-06a).  saved_value and the frame both live on this C frame,
     * which provably outlives the nested dispatch: cleanup bodies are
     * atomic (REVIVAL §14 S-cleanup-atomic, v0.13.1) — they cannot yield
     * out from under us. */
    ustrand_c_root_push(s, &saved_value_root, &saved_value);

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
     * urbi_vm_dispatch_loop_until_yield exits via the safepoint path if pending_unwind
     * becomes non-OK; urbi_unwind is called recursively from that safepoint.
     * Recursion depth is bounded by URBI_CLEANUP_MAX.
     *
     * The nested dispatch must not disturb the
     * embedder's urbi_step budget (urbi_vm_dispatch_loop_until_yield overwrites
     * vm->step_budget_remaining at entry), and a mature strand's depleted
     * per-strand safepoint budget must not park the cleanup body mid-run.
     * Save both, arm fresh values, restore after.  URBI_SCRATCH_BUDGET_OPS
     * (watcher/uwatcher.h) replaces the former bare 4096 literal (XC-13). */
    {
        UVM *vm = s->vm;
        uint64_t saved_step_budget   = (vm != NULL) ? vm->step_budget_remaining : 0;
        uint16_t saved_strand_budget = s->safepoint_budget_remaining;
        s->safepoint_budget_remaining = (uint16_t)URBI_SCRATCH_BUDGET_OPS;
        s->cleanup_body_done = 0U;

        (void)urbi_vm_dispatch_loop_until_yield(s, /*step_budget*/ URBI_SCRATCH_BUDGET_OPS);

        if (vm != NULL) vm->step_budget_remaining = saved_step_budget;
        s->safepoint_budget_remaining = saved_strand_budget;

        if (!s->cleanup_body_done && s->pending_unwind == UEXEC_OK) {
            if (USTRAND_GET_STATE(s) == USTRAND_DEAD) {
                /* C-1 absorbed-and-terminated: the body raised a replacement
                 * unwind, the recursive walker absorbed it at an OUTER handler
                 * (e.g. a catch above this finally), and the nested dispatch
                 * then ran the strand to completion.  All control flow was
                 * handled inside the nested dispatch; the original unwind is
                 * suppressed per C-1 and there is nothing left to restore.
                 * Pre-VM-02 this path restored the saved unwind onto the
                 * already-completed strand, marking it fatal and poisoning
                 * the session (vm->fatal_strand latched on the next
                 * urbi_step).  Callers check USTRAND_GET_STATE == DEAD after
                 * this helper returns and stop walking. */
                if (s->vm != NULL && s->vm->host_log_fn != NULL) {
                    s->vm->host_log_fn(s->vm, s->vm->host_log_ud, URBI_LOG_WARN,
                                       "URBI_WARN_SUPPRESSED_UNWIND: cleanup body "
                                       "raised; original unwind dropped");
                }
                ustrand_c_root_pop(s, &saved_value_root);
                s->cleanup_run_depth--;
                return 0;
            }
            /* v0.13.1-B: the body's replacement unwind was absorbed at an
             * OUTER handler (catch / tag-stop absorption with
             * cleanup_run_depth > 0 sets s->cleanup_absorbed) and the
             * strand continued as normal code — a subsequent yield/park is
             * legitimate control flow, not a mid-cleanup truncation.
             * Leave the strand exactly as dispatch left it; the original
             * unwind was suppressed per C-1.  The walker's call sites see
             * pending_unwind == UEXEC_OK and stop walking. */
            if (s->cleanup_absorbed) {
                s->cleanup_absorbed = 0U;
                ustrand_c_root_pop(s, &saved_value_root);
                s->cleanup_run_depth--;
                return 0;
            }
            /* The body exited dispatch without reaching its OP_RESUME
             * terminator and without raising: it yielded, blocked, or
             * exhausted the budget.  Cleanup bodies are atomic (REVIVAL
             * §14, 2026-06-10); a silent mid-body truncation enqueues or
             * parks the strand while the walker keeps unwinding it.
             * Unpark defensively, then escalate via the overflow path
             * (walker marks the strand fatal). */
            if (USTRAND_IS_WAITING(s)) {
                /* Unpark covers ALL
                 * WAITING reasons — sleep queue, event waiter chain, JOIN
                 * (child->joiners_head), WATCHER (waituntil back-pointer) —
                 * not just the SLEEP/EVENT pair the pre-fix code handled.
                 * SCHED-01 (closes v0.13.1-C): unpark(s, 0) owns the
                 * waiting-count exit; the strand then re-enters the counted
                 * set as RUNNING (about to be stamped fatal-DEAD by the
                 * overflow arm, whose driver fatal path owns the DEAD
                 * decrement) — correct by construction under the
                 * single-writer scheme. */
                urbi_sched_strand_unpark(s, /*enqueue=*/0);
                s->state = USTRAND_STATE_RUNNING;
                urbi_sched_runnable_inc(s->vm, s);
            } else if ((s->state & USTRAND_STATE_MASK) == USTRAND_READY) {
                /* Yielded: still counted; splice off the queue (unbind
                 * decrements) and re-enter the counted set as RUNNING —
                 * net zero, queue links clean. */
                urbi_sched_strand_unbind_from_ready_queue(s);
                s->state = USTRAND_STATE_RUNNING;
                urbi_sched_runnable_inc(s->vm, s);
            }
            if (s->vm != NULL && s->vm->host_log_fn != NULL) {
                s->vm->host_log_fn(s->vm, s->vm->host_log_ud, URBI_LOG_ERROR,
                    "URBI_ERR_CLEANUP_OVERFLOW: cleanup body yielded or blocked; "
                    "finally/onleave bodies execute atomically");
            }
            ustrand_c_root_pop(s, &saved_value_root);
            s->cleanup_run_depth--;
            return URBI_ERR_CLEANUP_OVERFLOW;
        }
        s->cleanup_body_done = 0U;
    }

    if (s->pending_unwind == UEXEC_OK) {
        /* Cleanup body completed normally: restore the original unwind state.
         * The restore happens while saved_value is still pinned on the root
         * chain; from here the value lives in s->unwind_value, which the
         * strand walker roots directly. */
        s->pending_unwind = saved_unwind;
        s->unwind_value   = saved_value;
    } else {
        /* C-1: cleanup body raised a new unwind; new state wins.
         * Original saved values are silently dropped.  Emit a diagnostic so
         * embedders can detect inadvertent unwind-loss in their tag-leave
         * handlers.  host_log_fn is NULL-guarded. */
        if (s->vm != NULL && s->vm->host_log_fn != NULL) {
            s->vm->host_log_fn(s->vm, s->vm->host_log_ud, URBI_LOG_WARN,
                               "URBI_WARN_SUPPRESSED_UNWIND: cleanup body raised; "
                               "original unwind dropped");
        }
    }

    ustrand_c_root_pop(s, &saved_value_root);
    s->cleanup_run_depth--;
    return 0;
}

/* ===== urbi_unwind: main 3-kind walker =====
   Called from the safepoint in urbi_vm_dispatch_loop_until_yield when
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

    /* RETURN absorption (bridging stub).
     *
     * Two paths:
     *   (a) forward path: a CALL_FRAME cleanup entry exists on the stack.
     *       Let the walker run — it absorbs RETURN at the first CALL_FRAME
     *       and runs the register-zeroing pass (Inv-5, row 7 §7.1).
     *   (b) deferred path (today's production reality — no CALL_FRAME entries
     *       are pushed by bytecode): use the backward-compat direct-pop
     *       path REGARDLESS of cleanup_depth.  Earlier code gated direct-pop
     *       on cleanup_depth == 0, so any RETURN through an at-body (which
     *       carries a TAG_SCOPE entry) entered the walker, popped the
     *       TAG_SCOPE without absorbing, and fell through to fatal: —
     *       converting a legitimate return into a fatal_status (task #23,
     *       surfaced 2026-05-16 when an at-body invoked a scripted method).
     *
     * RETURN crosses exactly one call-frame boundary; intervening tag /
     * try scopes belonging to the CALLER must stay intact.
     *
     * Cleanup entries belonging to the
     * RETURNING frame (frame_depth >= frame_count — the push-time
     * stamps make this exact) must be processed BEFORE the frame pops.
     * Before this fix the direct pop skipped them entirely: a finally inside the
     * returning function was silently SKIPPED (REVIVAL §S5a violation),
     * and TAG_SCOPE entries leaked one per call — at URBI_CLEANUP_MAX
     * accumulated leaks the next OP_PUSH_TAG killed the strand silently,
     * and a later tag.stop() could absorb at a stale entry (time-travel
     * resume at a dead handler_pc). */
    if (s->pending_unwind == UEXEC_RETURN && s->frame_count > 0) {
        int has_call_frame = 0;
        for (uint16_t i = 0; i < s->cleanup_depth; i++) {
            if (s->cleanup_base[i].kind == (uint8_t)UCLEANUP_CALL_FRAME) {
                has_call_frame = 1;
                break;
            }
        }
        if (!has_call_frame) {
            int replaced = 0;
            while (s->cleanup_depth > 0) {
                UCleanupEntry *e = &s->cleanup_base[s->cleanup_depth - 1];
                if (e->frame_depth < s->frame_count)
                    break;  /* caller's entry — leave intact */
                /* Inv-5: same register-zeroing contract as the walker. */
                if (e->register_count > 0)
                    zero_registers(s, e->register_base, e->register_count);
                if ((UCleanupKind)e->kind == UCLEANUP_TRY_FRAME &&
                    (e->flags & FLAG_HAS_FINALLY)) {
                    /* Finally runs on the return path too (REVIVAL §S5a).
                     * C-1 replace-on-raise applies: a clean body restores
                     * the saved RETURN+value; a raising body wins and the
                     * RETURN is suppressed — hand the new unwind to the
                     * general walker below.  A RETURN→RETURN replacement is
                     * also correct: C-1's value swap inside
                     * run_cleanup_with_replace precedes the
                     * pending != RETURN test below, so the loop continues
                     * with the replacement value already in unwind_value. */
                    uint16_t handler_pc = e->handler_pc;
                    int      rc;
                    urbi_sched_strand_cleanup_pop(s, UCLEANUP_TRY_FRAME);
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
                    /* Strand may have terminated inside
                     * the nested dispatch; stop immediately. */
                    if (USTRAND_GET_STATE(s) == USTRAND_DEAD)
                        return;
                    /* v0.13.1-B: pending == OK after a 0-rc return means the
                     * body's replacement unwind was absorbed at an OUTER
                     * handler and the strand continues as normal code (the
                     * RETURN was suppressed per C-1) — stop walking; the
                     * strand is parked/yielded/running at the handler.
                     * (A normally-completed body restores the saved
                     * non-OK unwind, so OK is unambiguous here.) */
                    if (s->pending_unwind == UEXEC_OK)
                        return;
                    if (s->pending_unwind != UEXEC_RETURN) {
                        replaced = 1;
                        break;
                    }
                } else if ((UCleanupKind)e->kind == UCLEANUP_TAG_SCOPE) {
                    /* Plain teardown — exactly what OP_POP_TAG runs (leave
                     * event, watcher cascade, member unlink, anonymous-tag
                     * destroy).  NOT the stop-absorb arm: the RETURN exits
                     * the scope; nothing resumes inside it. */
                    urbi_vm_tag_scope_teardown(s, e);
                } else {
                    /* TRY_FRAME without finally (a catch never matches a
                     * RETURN): pop and continue.  The explicit kind keeps
                     * urbi_sched_strand_cleanup_pop's kind assert meaningful — TRY_FRAME
                     * is the only kind left after the arms above. */
                    urbi_sched_strand_cleanup_pop(s, UCLEANUP_TRY_FRAME);
                }
            }
            if (!replaced) {
                UValue retval = s->unwind_value;
                int result_reg = s->frames[s->frame_count - 1].result_dest_reg;
                pop_call_frame(s);
                deliver_return_value(s, retval, result_reg);
                s->pending_unwind = UEXEC_OK;
                s->unwind_value   = nil;
                return;
            }
            /* C-1 replaced the RETURN: fall through to the general walker
             * with the new pending unwind. */
        }
        /* else fall through to walker — forward path absorbs at CALL_FRAME. */
    }

    if (s->cleanup_depth == 0) {
        /* Any other unwind with empty cleanup stack → fatal escalation. */
        goto fatal;
    }

    /* Walk cleanup-stack entries from innermost to outermost. */
    while (s->cleanup_depth > 0) {
        UCleanupEntry *e = &s->cleanup_base[s->cleanup_depth - 1];
        UCleanupKind kind = (UCleanupKind)e->kind;

        /* The unwind may have crossed call frames
         * entered after this entry was pushed.  Tear them down FIRST:
         * handler_pc is relative to the proto that pushed the entry,
         * register_base/count index that frame's window, and
         * run_cleanup_with_replace resolves handler PCs via s->pc_base.
         * pop_call_frame restores R / pc / pc_base / cur_consts and closes
         * upvalues — exactly what the RETURN path does.  CALL_FRAME entries
         * (OP_PUSH_FRAME_GUARD, currently never emitted) manage their own
         * frame teardown in their absorb arm; skip them here. */
        if (kind != UCLEANUP_CALL_FRAME) {
            while (s->frame_count > e->frame_depth)
                pop_call_frame(s);
        }

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
            urbi_sched_strand_cleanup_pop(s, UCLEANUP_CALL_FRAME);

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

            /* pattern_matches is a documented stub that
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
                urbi_sched_strand_cleanup_pop(s, UCLEANUP_TRY_FRAME);
                s->pc = s->pc_base + handler_pc;
                s->pending_unwind = UEXEC_OK;
                s->unwind_value   = nil;
                /* v0.13.1-B: absorbed while a cleanup body is on the C
                 * stack (this walker was entered from the nested dispatch
                 * inside run_cleanup_with_replace) — the strand continues
                 * as normal code at the handler; flag it so a subsequent
                 * yield/park is not misread as a cleanup truncation. */
                if (s->cleanup_run_depth > 0U)
                    s->cleanup_absorbed = 1U;
                return;  /* absorbed */
            }

            if (e->flags & FLAG_HAS_FINALLY) {
                /* Finally execution: run body, then continue unwinding.
                 * C-1 replace-on-raise: if body raises, new state wins.
                 * The helper returns URBI_ERR_CLEANUP_OVERFLOW
                 * when recursion depth would exceed URBI_CLEANUP_MAX. */
                uint16_t handler_pc = e->handler_pc;
                int      rc;
                urbi_sched_strand_cleanup_pop(s, UCLEANUP_TRY_FRAME);
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
                /* The body's replacement unwind may have
                 * been fully handled inside the nested dispatch (absorbed at
                 * an outer handler, strand ran to completion) or escalated to
                 * a fatal there.  Either way the strand has terminated; stop
                 * walking (fatal_status, if any, is already latched). */
                if (USTRAND_GET_STATE(s) == USTRAND_DEAD)
                    return;
                /* v0.13.1-B: absorbed-and-continued — see the RETURN
                 * direct-pop arm.  Stop walking; the strand owns control. */
                if (s->pending_unwind == UEXEC_OK)
                    return;
                /* After run_cleanup_with_replace, s->pending_unwind is either
                 * the original (body OK) or a new one (C-1 replace).
                 * Either way, continue the walker loop. */
                continue;
            }

            /* No matching catch and no finally: pop and propagate. */
            urbi_sched_strand_cleanup_pop(s, UCLEANUP_TRY_FRAME);
            continue;
        }

        case UCLEANUP_TAG_SCOPE: {
            /* v0.10.15-B: tag.stop() absorption / resume-after-scope.
             * If the pending unwind is a TAG_STOP targeting THIS scope's tag,
             * terminate the tagged block and resume execution AFTER it — legacy
             * urbi semantics: tag.stop ends the tagged block; enclosing code
             * continues.  The scope's handler_pc is the post-scope continuation
             * PC (OP_PUSH_TAG Bx == the empty-onleave/past-scope target, which
             * is where normal OP_POP_TAG completion lands), so jumping there
             * resumes exactly as a normal scope exit would.  Run the same
             * teardown OP_POP_TAG runs (leave event, watcher cascade, anonymous-
             * tag destroy) via the shared helper so the two paths can't drift.
             * A user-owned tag (FLAG_TAG_USER_OWNED) is left alive by the helper.
             * Capture resume_pc before teardown — it pops the entry, after which
             * e->handler_pc may be overwritten by a later push. */
            if (s->pending_unwind == UEXEC_TAG_STOP &&
                e->owning_tag != NULL &&
                e->owning_tag == s->unwind_target) {
                /* Carried-over pass-through fix: synthetic
                 * ambient entries (FLAG_TAG_AMBIENT — realm tag / inherited
                 * fork-chain tag) must NOT absorb the TAG_STOP: their
                 * handler_pc == 0 (would restart the thunk at pc_base) and
                 * urbi_vm_tag_scope_teardown would fire leave events and destroy
                 * the shared tag per-strand.  Bare-pop and continue walking
                 * so the unwind propagates and eventually terminates the
                 * strand via USTRAND_STATE_DEAD. */
                if (e->flags & FLAG_TAG_AMBIENT) {
                    urbi_sched_strand_cleanup_pop(s, UCLEANUP_TAG_SCOPE);
                    continue;
                }
                uint16_t resume_pc = e->handler_pc;
                urbi_vm_tag_scope_teardown(s, e);
                s->pc             = s->pc_base + resume_pc;
                s->pending_unwind = UEXEC_OK;
                s->unwind_value   = nil;
                s->unwind_target  = NULL;
                /* v0.13.1-B: same post-absorption-continuation flag as the
                 * catch arm — a tag-stop raised inside a cleanup body and
                 * absorbed at an outer scope continues as normal code. */
                if (s->cleanup_run_depth > 0U)
                    s->cleanup_absorbed = 1U;
                return;  /* absorbed — strand resumes after the tagged block */
            }

            /* TAG_SCOPE pass-through (non-matching tag, or non-TAG_STOP unwind).
             *
             * v0.13.3 (design-risks v0.13.1-M): a real scope (opened by
             * OP_PUSH_TAG) gets the SAME teardown OP_POP_TAG runs —
             * urbi_vm_tag_scope_teardown fires the tier-2 leave event, cascades
             * member watchers to the pending-onleave queue, unlinks the
             * member entry, pops, and destroys an anonymous per-scope tag
             * (user-owned tags survive).  Pre-fix the bare pop skipped all
             * of that: the leave event never fired and the scope's member
             * watchers LEAKED past the scope.
             *
             * Synthetic ambient entries (FLAG_TAG_AMBIENT — realm tag /
             * inherited fork chain) keep the bare pop: their tag is SHARED
             * across strands, so a per-strand scope teardown (leave event,
             * cascade, utag_destroy with other members still linked) would
             * be wrong; ustrand_destroy's strand_unlink_from_tags owns
             * their member unlink, as before.
             *
             * Re-entrancy: the leave-event sync bodies run on the transient
             * scratch strand (run_event_body_on_scratch), never on s, so
             * this walker cannot be re-entered for s; a leave handler that
             * deposits a new cross-strand unwind on s mid-walk overwrites
             * pending_unwind under C-1 priority and the loop continues with
             * the replacement. */

            if (e->flags & FLAG_HAS_ONLEAVE) {
                /* onleave handler: run under C-1 replace-on-raise AFTER the
                 * scope teardown, mirroring OP_POP_TAG's order (teardown at
                 * pop; the OP_POP_TAG onleave arm itself is emit-dead at
                 * v1.0 — urbi_emit_tag_prefix_arm never sets FLAG_HAS_ONLEAVE).
                 * Handle URBI_ERR_CLEANUP_OVERFLOW. */
                uint16_t handler_pc = e->handler_pc;
                int      rc;
                if (e->flags & FLAG_TAG_AMBIENT)
                    urbi_sched_strand_cleanup_pop(s, UCLEANUP_TAG_SCOPE);
                else
                    urbi_vm_tag_scope_teardown(s, e);
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
                /* See the TRY_FRAME finally arm — strand
                 * terminated inside the nested dispatch; stop walking. */
                if (USTRAND_GET_STATE(s) == USTRAND_DEAD)
                    return;
                /* v0.13.1-B: absorbed-and-continued — stop walking. */
                if (s->pending_unwind == UEXEC_OK)
                    return;
                continue;
            }

            if (e->flags & FLAG_TAG_AMBIENT)
                urbi_sched_strand_cleanup_pop(s, UCLEANUP_TAG_SCOPE);
            else
                urbi_vm_tag_scope_teardown(s, e);
            continue;
        }

        default: {
            /* Unknown entry kind — safety net; pop and continue to avoid
             * an infinite loop on corrupted cleanup stack. */
            urbi_sched_strand_cleanup_pop(s, (UCleanupKind)kind);
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

/* ===== Row 7 public C API =====
 *
 * These functions expose strand control-transfer to host C code.
 * Thread safety: not ISR-safe.  The ISR-safe path was added later.
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
 * ustrand_destroy so urbi_sched_quiescent eventually converges).
 *
 * WAITING strands are woken via urbi_sched_strand_unpark (unlink the
 * reason-specific third-party links, then the make_runnable
 * funnel) so they run and process the unwind before the scheduler reaches
 * quiescence.  SUSPENDED strands are resumed with their block/freeze gates
 * cleared (stop overrides suspension) for the same
 * reason — pre-fix the deposit landed but the member never ran it.
 *
 * Watcher cascade: urbi_tag_stop walks tag->member_watchers_head and pushes
 * each registered watcher to the pending-onleave queue (see step (2) below).
 *
 * NOT ISR-safe.  Returns URBI_ERR_INVALID_ARG for NULL vm or tag. */
int
urbi_tag_stop(struct UVM *vm, struct UTag *tag, UValue value)
{
    UCleanupEntry *e;
    UCleanupEntry *next;

    if (!vm || !tag) return URBI_ERR_INVALID_ARG;
    URBI_ASSERT_NOT_ISR(vm);
    URBI_TP(vm, URBI_TRACE_TAG, URBI_LOG_INFO, URBI_TP_TAG_OP, 0u,
            (uint32_t)(uintptr_t)tag);

    /* (1) Deposit pending TAG_STOP unwind on every member strand.
     * FOREACH_SAFE snapshots next_member before each body so entries may
     * unlink themselves during the walk (they do not today, but being safe
     * costs nothing). */
    URBI_SLIST_FOREACH_SAFE(e, next, tag->member_strands_head, next_member) {
        UStrand *s;
        bool fresh_deposit;

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

        /* Wake any blocked strand so it can consume the unwind.
         * urbi_sched_strand_unpark removes the strand's
         * reason-specific third-party links (sleep queue / event waiter
         * chain / child->joiners_head / waituntil waiter_strand) BEFORE
         * routing through the make_runnable wake funnel, so no later waker
         * (urbi_vm_fork_wake_joiners at child death, urbi_vm_watcher_eval_dirty on a rising
         * edge) can touch a strand that already moved on or was freed.
         *
         * SCHED-08: stop overrides suspension — the deposit must be
         * consumable.  Clear the gates first (the tag is stopping; leaving
         * a gate set would strand the unwind forever): on a WAITING member
         * a pending gate would route the unpark wake into SUSPENDED; on a
         * SUSPENDED member the gates would block the resume outright
         * (pre-fix: deposit without resume — the member never ran its
         * unwind/onleave and leaked, pinning strand_suspended_count). */
        if (USTRAND_IS_WAITING(s)) {
            s->suspend_gates = 0U;
            urbi_sched_strand_unpark(s, /*enqueue=*/1);
        } else if (USTRAND_IS_SUSPENDED(s)) {
            s->suspend_gates = 0U;
            /* URBI_TP_SCHED_RESUME intentionally not emitted here: this is a
             * forced override (stop wins over suspension), not an ungated-gate
             * resume.  The tracepoint in urbi_strand_resume_if_ungated covers
             * the normal gate-clear path; emitting here would mislabel the
             * stop-override event as a voluntary gate resume. */
            urbi_sched_strand_make_runnable(s);
        }
    }

    /* (1b) Mark the tag as stopped so urbi_tag_info can report URBI_TAG_STOPPED.
     * UTAG_FLAG_STOPPED was declared at v0.5.x as RESERVED; v0.7.1 activates it
     * here (Gap M) so the public urbi_tag_state_t surface reflects real state. */
    tag->flags |= UTAG_FLAG_STOPPED;

    /* (2) Watcher cascade: push each watcher registered on this tag to the
     * pending-onleave queue.  FOREACH_SAFE captures next_in_tag before each
     * push, which unlinks the watcher from member_watchers_head as it goes. */
    {
        UWatcher *ww, *ww_next;
        URBI_SLIST_FOREACH_SAFE(ww, ww_next, tag->member_watchers_head,
                                next_in_tag) {
            urbi_watcher_pending_onleave_queue_push(vm, ww);
        }
    }

    /* (3) Periodic cascade (B5/SCHED-N2): mark every periodic owned by this tag
     * for unregistration.  The next urbi_periodic_pump Phase 2 pass frees them
     * once their current_strand reaches NULL.
     *
     * Kept in temporal.c (urbi_periodics_stop_owned_by) so this TU never touches
     * UPeriodic internals. */
    urbi_periodics_stop_owned_by(vm, tag);

    /* (4) Return synchronously — all deposits are complete. */
    return URBI_OK;
}

/* === W3b (v0.10.9): urbi_tag_block / urbi_tag_unblock ===
 *
 * Block is the second of three cancellation modes (§S6 ratified at the
 * Cat. E re-audit on 2026-05-27): stop / block / freeze.  All three
 * walk tag->member_strands_head; block and freeze suspend rather than
 * deposit an unwind, and they are independent gates (unblock only
 * resumes BLOCK-suspended strands; unfreeze only FREEZE-suspended).
 *
 * urbi_tag_block sets UTAG_FLAG_BLOCKED and calls urbi_strand_suspend
 * with REASON_BLOCK on every member strand.  resume_value is stashed
 * on each strand's unblock_value so a future result-register handoff
 * (W3f, deferred) can deliver it on resume.  SCHED-08 (v0.13.3): each
 * member's USTRAND_GATE_BLOCK bit is set — WAITING members stay parked
 * with the gate armed (the make_runnable funnel routes their eventual
 * wake to SUSPENDED); already-SUSPENDED members stack the gate.
 *
 * urbi_tag_unblock clears UTAG_FLAG_BLOCKED and clears each member's
 * BLOCK gate; a member resumes only when its FREEZE gate is also clear
 * (urbi_strand_resume_if_ungated).  FREEZE-gated members stay suspended.
 *
 * Not ISR-safe.  Returns URBI_ERR_INVALID_ARG on NULL vm or tag. */
int
urbi_tag_block(struct UVM *vm, struct UTag *tag, UValue resume_value)
{
    if (vm == NULL || tag == NULL) return URBI_ERR_INVALID_ARG;
    URBI_ASSERT_NOT_ISR(vm);

    URBI_TP(vm, URBI_TRACE_TAG, URBI_LOG_INFO, URBI_TP_TAG_OP, 1u,
            (uint32_t)(uintptr_t)tag);
    tag->flags |= (uint8_t)UTAG_FLAG_BLOCKED;

    /* FOREACH_SAFE snapshots next_member before each body — urbi_strand_suspend
     * does not unlink the entry today, but being safe costs nothing. */
    UCleanupEntry *e, *next;
    URBI_SLIST_FOREACH_SAFE(e, next, tag->member_strands_head, next_member) {
        UStrand *s = e->strand_back;
        if (s != NULL) {
            /* Stash the value first so a races-against-resume can deliver
             * the right value to a strand that gets immediately unblocked.
             * urbi_strand_suspend itself does not write unblock_value. */
            s->unblock_value = resume_value;
            urbi_strand_suspend(s, USTRAND_REASON_BLOCK, tag);
        }
    }
    return URBI_OK;
}

int
urbi_tag_unblock(struct UVM *vm, struct UTag *tag)
{
    if (vm == NULL || tag == NULL) return URBI_ERR_INVALID_ARG;
    URBI_ASSERT_NOT_ISR(vm);

    URBI_TP(vm, URBI_TRACE_TAG, URBI_LOG_INFO, URBI_TP_TAG_OP, 2u,
            (uint32_t)(uintptr_t)tag);
    tag->flags &= (uint8_t)~UTAG_FLAG_BLOCKED;

    UCleanupEntry *e, *next;
    URBI_SLIST_FOREACH_SAFE(e, next, tag->member_strands_head, next_member) {
        UStrand *s = e->strand_back;
        if (s != NULL && (s->suspend_gates & USTRAND_GATE_BLOCK) != 0U) {
            /* SCHED-08: clear this mode's gate wherever the member is —
             * SUSPENDED (resume below iff the FREEZE gate is clear too,
             * delivering the unblock_value stashed by urbi_tag_block) or
             * still parked WAITING (the pending wake just loses its gate;
             * the strand stays on its sleep/event/join/watcher park and
             * urbi_strand_resume_if_ungated no-ops). */
            s->suspend_gates &= (uint8_t)~USTRAND_GATE_BLOCK;
            urbi_strand_resume_if_ungated(s);
        }
    }
    return URBI_OK;
}

/* === W3c (v0.10.9): urbi_tag_freeze / urbi_tag_unfreeze ===
 *
 * Third cancellation mode after stop (urbi_tag_stop) and block
 * (urbi_tag_block).  Structurally identical to W3b but with REASON_FREEZE
 * and UTAG_FLAG_FROZEN.
 *
 * Replaces the flag-only stub previously installed by tag_freeze_native /
 * tag_unfreeze_native at v0.10.2.  Those native methods now forward
 * through this C API so the strand-suspension semantic actually fires.
 *
 * SCHED-08 (v0.13.3): unfreeze clears each member's FREEZE gate; a member
 * resumes only when its BLOCK gate is also clear (urbi_strand_resume_if_ungated).
 * BLOCK-gated strands (W3b) stay suspended.  Block and freeze are
 * independent gates per workspace ledger §S6.
 *
 * Not ISR-safe.  Returns URBI_ERR_INVALID_ARG for NULL vm/tag. */
int
urbi_tag_freeze(struct UVM *vm, struct UTag *tag)
{
    if (vm == NULL || tag == NULL) return URBI_ERR_INVALID_ARG;
    URBI_ASSERT_NOT_ISR(vm);

    URBI_TP(vm, URBI_TRACE_TAG, URBI_LOG_INFO, URBI_TP_TAG_OP, 3u,
            (uint32_t)(uintptr_t)tag);
    tag->flags |= (uint8_t)UTAG_FLAG_FROZEN;

    UCleanupEntry *e, *next;
    URBI_SLIST_FOREACH_SAFE(e, next, tag->member_strands_head, next_member) {
        UStrand *s = e->strand_back;
        if (s != NULL) {
            urbi_strand_suspend(s, USTRAND_REASON_FREEZE, tag);
        }
    }
    return URBI_OK;
}

int
urbi_tag_unfreeze(struct UVM *vm, struct UTag *tag)
{
    if (vm == NULL || tag == NULL) return URBI_ERR_INVALID_ARG;
    URBI_ASSERT_NOT_ISR(vm);

    URBI_TP(vm, URBI_TRACE_TAG, URBI_LOG_INFO, URBI_TP_TAG_OP, 4u,
            (uint32_t)(uintptr_t)tag);
    tag->flags &= (uint8_t)~UTAG_FLAG_FROZEN;

    UCleanupEntry *e, *next;
    URBI_SLIST_FOREACH_SAFE(e, next, tag->member_strands_head, next_member) {
        UStrand *s = e->strand_back;
        if (s != NULL && (s->suspend_gates & USTRAND_GATE_FREEZE) != 0U) {
            /* SCHED-08: mirror of the unblock walk — clear the FREEZE gate
             * wherever the member is; resume only if ungated.  freeze has
             * no resume-value semantic of its own: the staged
             * unblock_value (stamped by a prior urbi_tag_block; never
             * cleared on resume, so may carry a stale value after the
             * first block/resume cycle — see STALENESS NOTE in
             * urbi_strand_resume_if_ungated) is what a resume here delivers. */
            s->suspend_gates &= (uint8_t)~USTRAND_GATE_FREEZE;
            urbi_strand_resume_if_ungated(s);
        }
    }
    return URBI_OK;
}

/* urbi_strand_cancel — deposit CANCEL (fatal, no catch) on a strand. */
int
urbi_strand_cancel(struct UVM *vm, struct UStrand *strand, UValue cancel_reason)
{
    (void)vm;  /* vm mirrors strand->vm; accepted for API convention */
    if (!strand) return URBI_ERR_INVALID_ARG;
    if (strand->vm) { URBI_ASSERT_NOT_ISR(strand->vm); }
    if (USTRAND_GET_STATE(strand) == USTRAND_DEAD) return URBI_ERR_STRAND_FATAL;
    strand->pending_unwind = UEXEC_CANCEL;
    strand->unwind_value   = cancel_reason;
    /* If the strand is sleeping/waiting, unblock it so it can process the
     * unwind.  Route through the scheduler
     * helpers — stamping READY in place left a sleeping strand linked on
     * the sleep queue and never enqueued it (zombie strand).
     * urbi_sched_strand_unpark additionally removes the reason-
     * specific third-party links (event waiter chain / child->joiners_head
     * / waituntil waiter_strand) before the wake.  Mirrors the
     * urbi_tag_stop wake block.
     *
     * SCHED-08: cancel overrides suspension exactly like tag-stop — clear
     * the gates (a pending gate would route the unpark wake into SUSPENDED;
     * a set gate on a SUSPENDED target would block the resume) and wake a
     * SUSPENDED target so the CANCEL deposit is consumable (pre-fix: same
     * deposit-without-resume leak shape as SCHED-08's tag-stop arm). */
    if (USTRAND_IS_WAITING(strand)) {
        strand->suspend_gates = 0U;
        urbi_sched_strand_unpark(strand, /*enqueue=*/1);
    } else if (USTRAND_IS_SUSPENDED(strand)) {
        strand->suspend_gates = 0U;
        /* URBI_TP_SCHED_RESUME intentionally not emitted: cancel override,
         * not a voluntary gate-clear resume (same rationale as tag-stop arm). */
        urbi_sched_strand_make_runnable(strand);
    }
    return URBI_OK;
}

/* urbi_strand_panic — skip walker, mark strand DEAD immediately.
 * No cleanup runs.  The msg parameter is for diagnostic context; it
 * is not stored (no string heap) — diagnostic infra will wire it.
 * The fatal_value is set to nil; a future pass may upgrade to a string UValue. */
int
urbi_strand_panic(struct UVM *vm, struct UStrand *strand, const char *msg)
{
    UValue nil;
    nil.kind  = UVAL_NIL;
    nil.v.i   = 0;

    (void)vm;  /* vm mirrors strand->vm; accepted for API convention */
    if (!strand) return URBI_ERR_INVALID_ARG;
    if (strand->vm) { URBI_ASSERT_NOT_ISR(strand->vm); }
    /* Route diagnostic msg through host_log_fn before marking the
     * strand dead so embedders can correlate panic causes with their own
     * logging pipeline.  URBI_LOG_FATAL is not defined; use ERROR (highest
     * level we have).  NULL-guarded — many tests wire vm without a log
     * callback. */
    if (msg != NULL && strand->vm != NULL && strand->vm->host_log_fn != NULL) {
        strand->vm->host_log_fn(strand->vm, strand->vm->host_log_ud, (int)URBI_LOG_ERROR, "%s", msg);
    }
    /* v0.13.3 (SCHED-05 carried finding): the DEAD stamp below bypasses
     * both the make_runnable wake funnel and ustrand_destroy's
     * death-from-parked arm (the state is no longer WAITING/SUSPENDED by
     * then), so the parked-strand counter exit and the third-party link
     * scrub (sleep queue / event waiter chain / child->joiners_head /
     * waituntil waiter_strand) must happen HERE.  Pre-fix only the event
     * waiter chain was unlinked, and the waiting/suspended counters leaked
     * (no-saturation decrement asserts would fire on a later transition). */
    if (USTRAND_IS_WAITING(strand)) {
        urbi_sched_strand_unpark(strand, /*enqueue=*/0);
    } else if (USTRAND_IS_SUSPENDED(strand)) {
        urbi_sched_suspended_dec(strand->vm, strand);
    }
    strand->fatal_status = UEXEC_CANCEL;
    strand->fatal_value  = nil;
    strand->state        = USTRAND_STATE_DEAD;
    /* pending_unwind stays as-is; the strand is immediately dead.
     * No cleanup runs — panic is the "kill unconditionally" path. */
    return URBI_OK;
}

/* urbi_strand_unwind_status — read pending unwind state (non-destructive).
 * Returns UStrandUnwind (public mirror of UExecStatus; numeric values
 * are identical so the cast is safe). */
UStrandUnwind
urbi_strand_unwind_status(struct UVM *vm, const struct UStrand *strand)
{
    (void)vm;
    if (strand && strand->vm) { URBI_ASSERT_NOT_ISR(strand->vm); }
    return (UStrandUnwind)(strand ? strand->pending_unwind : UEXEC_OK);
}

/* urbi_strand_is_fatal — query whether the strand has hit a fatal unwind.
 * Returns true and populates out_status / out_value (both nullable) if fatal.
 * out_status receives a UStrandUnwind value (numerically == UExecStatus). */
bool
urbi_strand_is_fatal(struct UVM *vm, const struct UStrand *strand,
                     UStrandUnwind *out_status, UValue *out_value)
{
    (void)vm;
    if (strand && strand->vm) { URBI_ASSERT_NOT_ISR(strand->vm); }
    if (!strand || strand->fatal_status == UEXEC_OK) return false;
    if (out_status) *out_status = (UStrandUnwind)strand->fatal_status;
    if (out_value)  *out_value  = strand->fatal_value;
    return true;
}

/* urbi_strand_reset — REPL session restart: clear all unwind / fatal state,
 * reset cleanup-stack depth, return strand to DORMANT.
 * Does not free or reallocate memory.  The register window (stack/R) is
 * left intact; callers are expected to re-initialise it per their session
 * semantics before the next dispatch. */
int
urbi_strand_reset(struct UVM *vm, struct UStrand *strand)
{
    UValue nil;
    nil.kind = UVAL_NIL;
    nil.v.i  = 0;

    (void)vm;
    if (!strand) return URBI_ERR_INVALID_ARG;
    if (strand->vm) { URBI_ASSERT_NOT_ISR(strand->vm); }

    strand->pending_unwind    = UEXEC_OK;
    strand->unwind_value      = nil;
    strand->unwind_target     = NULL;
    strand->fatal_status      = UEXEC_OK;
    strand->fatal_value       = nil;
    strand->cleanup_depth     = 0;
    strand->cleanup_run_depth = 0;        /* clear recursion counter */
    strand->cleanup_absorbed  = 0;        /* v0.13.1-B: no stale absorption flag */
    strand->cleanup_top       = NULL;
    strand->suspend_gates     = 0U;       /* SCHED-08: no stale block/freeze gate */
    strand->unblock_value     = nil;      /* SCHED-08: no stale resume value (W3f staleness) */
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
 * v0.10.3: gains vm as first arg and changes void → int so NULL
 * vm/strand can return URBI_ERR_INVALID_ARG (api-ergonomics F8). */
int
urbi_throw(struct UVM *vm, struct UStrand *strand, UValue value)
{
    (void)vm;
    if (!strand || !strand->vm) return URBI_ERR_INVALID_ARG;
    URBI_ASSERT_NOT_ISR(strand->vm);
    strand->pending_unwind = UEXEC_THROW;
    strand->unwind_value   = value;
    return URBI_OK;
}

/* urbi_return_val — deposit RETURN unwind (equiv to bytecode OP_RETURN).
 * Named urbi_return_val (not urbi_return) to avoid conflict with the C
 * keyword `return` in macro expansion contexts and to be unambiguous.
 * v0.10.3: gains vm as first arg and changes void → int. */
int
urbi_return_val(struct UVM *vm, struct UStrand *strand, UValue value)
{
    (void)vm;
    if (!strand || !strand->vm) return URBI_ERR_INVALID_ARG;
    URBI_ASSERT_NOT_ISR(strand->vm);
    strand->pending_unwind = UEXEC_RETURN;
    strand->unwind_value   = value;
    return URBI_OK;
}

/* urbi_tag_stop_local — deposit TAG_STOP from within the same strand.
 * v0.10.3: gains vm as first arg (api-ergonomics F3).
 * API-002: NULL strand or NULL strand->vm is a no-op. */
void
urbi_tag_stop_local(struct UVM *vm, struct UStrand *strand,
                    struct UTag *tag, UValue value)
{
    (void)vm;
    if (!strand || !strand->vm) return;
    URBI_ASSERT_NOT_ISR(strand->vm);
    strand->pending_unwind  = UEXEC_TAG_STOP;
    strand->unwind_target   = tag;
    strand->unwind_value    = value;
}
