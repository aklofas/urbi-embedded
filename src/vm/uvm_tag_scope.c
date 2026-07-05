/* SPDX-License-Identifier: BSD-3-Clause */
/* src/vm/uvm_tag_scope.c — OP_PUSH_TAG / OP_POP_TAG dispatch helpers,
 * extracted from the uvm.c dispatch loop (v0.10.15-vm-decomp-2, W1 stage 1).
 *
 * Behavior-preserving move: the arm bodies are byte-for-byte the v0.10.14
 * versions, with the dispatch-loop control transfers rewritten as return
 * codes (a helper cannot goto a caller label).  See uvm_tag_scope.h. */

#include "vm/uvm_tag_scope.h"

#include "vm/uvm.h"
#include "vm/uvm_internal.h"          /* vm_format_type_error_msg */
#include "urbi/urbi.h"                /* UVM_TYPE_ERROR */
#include "sched/ustrand.h"            /* UStrand, UEXEC_THROW, USTRAND_STATE_DEAD */
#include "runtime/ucleanup.h"         /* UCleanupEntry, UCLEANUP_TAG_SCOPE, FLAG_HAS_ONLEAVE, strand_cleanup_push/pop */
#include "runtime/umacros.h"          /* URBI_INTERNAL_ASSERT */
#include "runtime/ulist.h"            /* URBI_SLIST_FOREACH_SAFE */
#include "tag/utag.h"                 /* UTag, utag_create/destroy */
#include "watcher/uwatcher.h"         /* UWatcher, pending_onleave_queue_push */
#include "event/uevent.h"             /* UEvent */
#include "event/uevent_emit.h"        /* c_event_emit_sync */
#include "value/uvalue.h"             /* UValue, UVAL_NIL */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

UVmTagScopeResult
vm_push_tag_scope(UVM *vm, UStrand *s)
{
    /* OP_PUSH_TAG ABx:
     *   A[7:4] = flags nibble (0 at M3 — no FLAG_HAS_ONLEAVE)
     *   A[3:0] = tag_reg nibble (v0.10.9-B: READ at runtime — the code
     *            below checks R[tag_reg] and, when it holds a UVAL_TAG,
     *            binds the scope to the user's tag instead of creating
     *            an anonymous one; the anonymous UTag is the fallback
     *            for non-UVAL_TAG values and pre-v0.10.15 bytecode)
     *   Bx     = onleave_pc (handler PC; 0 at M3 since no onleave body)
     *
     * T30: allocate a per-scope UTag (no UVAL_TAG / register binding at M3).
     * Each tag-scope gets its own anonymous UTag; the tag's lifetime is
     * bounded by the corresponding OP_POP_TAG.
     * Walker-pop (urbi_unwind via OP_THROW etc.) will leak the UTag at M3 —
     * deferred for T31/walker integration when full tag lifecycle wires through.
     * strand_back = s for future tag.stop() walk (T31 uses). */
    (void)vm;  /* the VM is reached via s->vm; vm kept for call-site symmetry */
    uint8_t  a          = uinstr_a(*s->pc);
    uint8_t  flags      = (uint8_t)((a >> 4) & 0xFU);
    uint8_t  tag_reg    = (uint8_t)(a & 0x0FU);  /* v0.10.9-B: user-tag register */
    uint16_t handler_pc = uinstr_bx(*s->pc);
    UTag *tag;
    bool  tag_is_user_owned;
    if (s->R[tag_reg].kind == (uint8_t)UVAL_TAG && s->R[tag_reg].v.p != NULL) {
        /* v0.10.9-B: bind the scope to the user's tag (live in R[tag_reg], packed
         * there by emit_tag_prefix_arm) instead of creating an anonymous one, so
         * tag.stop() / membership / scopeTag observe this scope as part of t. */
        tag               = (UTag *)s->R[tag_reg].v.p;
        tag_is_user_owned = true;
    } else {
        /* Legacy / defensive fallback: pre-v0.10.15 bytecode, or a tag-prefix
         * expression that did not evaluate to a UVAL_TAG — keep the anonymous
         * per-scope tag. */
        tag = utag_create(s->vm);
        if (tag == NULL) {
            s->fatal_status = UEXEC_THROW;
            s->state        = USTRAND_STATE_DEAD;
            return UVM_TAG_SCOPE_FATAL;
        }
        tag_is_user_owned = false;
    }
    UCleanupEntry *entry = strand_cleanup_push(s);
    if (entry == NULL) {
        if (!tag_is_user_owned)
            utag_destroy(s->vm, tag);  /* roll back only an anonymous alloc */
        s->fatal_status = UEXEC_THROW;
        s->state        = USTRAND_STATE_DEAD;
        return UVM_TAG_SCOPE_FATAL;
    }
    entry->kind           = (uint8_t)UCLEANUP_TAG_SCOPE;
    entry->flags          = (uint8_t)(flags |
                            (tag_is_user_owned ? FLAG_TAG_USER_OWNED : 0U));
    entry->handler_pc     = handler_pc;
    entry->frame_depth    = (uint16_t)s->frame_count;  /* VM-01 */
    entry->register_base  = 0U;
    entry->register_count = 0U;
    entry->owning_tag     = tag;
    entry->catch_pattern  = NULL;
    entry->strand_back    = s;
    URBI_SLIST_PUSH(tag->member_strands_head, entry, next_member);
    /* VM-015: enter_event is unconditionally NULL on a fresh utag_create
     * (utag.c zero-fills enter_event/leave_event at allocation; only the
     * tag.enter native getter — invoked through a Tag.enter property
     * read — lazy-allocates the UEvent later in tag_enter_getter).  For an
     * anonymous scope the tag was just created above, so enter_event MUST be
     * NULL; assert that contract.  A USER-owned tag (v0.10.9-B) may legitimately
     * carry an enter_event from a prior Tag.enter read, so it is exempt. */
    if (!tag_is_user_owned) {
        URBI_INTERNAL_ASSERT(tag->enter_event == NULL);
    }
    return UVM_TAG_SCOPE_NEXT;
}

/* vm_tag_scope_teardown: the OP_POP_TAG teardown body, shared with the unwind
 * walker's tag.stop() absorption path (v0.10.15-B).  `top` MUST be the top
 * UCLEANUP_TAG_SCOPE entry.  Unlinks the entry from the tag's member list,
 * fires the tier-2 leave event, cascades member watchers to the pending-onleave
 * queue, pops the cleanup entry, and destroys an anonymous per-scope UTag (a
 * user-owned tag outlives the scope and is left alive).  Behaviour is identical
 * whether reached via the OP_POP_TAG opcode (normal exit) or the walker
 * (tag.stop() absorption) — a single implementation prevents drift. */
void
vm_tag_scope_teardown(UStrand *s, UCleanupEntry *top)
{
    /* T30: capture owning_tag before pop — the slot remains valid memory but
     * is below cleanup_depth after pop and may be reused by a later push.
     * Capture flags too (v0.10.9-B FLAG_TAG_USER_OWNED) for the same reason. */
    UTag *tag = top->owning_tag;
    uint8_t top_flags = top->flags;
    /* Unlink this entry from tag->member_strands_head (singly-linked
     * list removal via next_member). Only unlink when tag is non-NULL
     * — older bytecode emitted before T30 may have owning_tag == NULL. */
    if (tag != NULL) {
        URBI_SLIST_UNLINK(tag->member_strands_head, top, next_member,
                          UCleanupEntry);
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
     * list is empty — push empties it.
     * v0.13.5 (v0.13.4-A): user-owned tags (FLAG_TAG_USER_OWNED) skip
     * this cascade — their watchers persist until t.stop() or VM-destroy.
     * Mirrors the utag_destroy guard below (:163) exactly. */
    if (tag != NULL && (top_flags & FLAG_TAG_USER_OWNED) == 0U) {
        UWatcher *ww, *ww_next;
        URBI_SLIST_FOREACH_SAFE(ww, ww_next, tag->member_watchers_head,
                                next_in_tag) {
            pending_onleave_queue_push(s->vm, ww);
        }
    }
    strand_cleanup_pop(s, UCLEANUP_TAG_SCOPE);
    /* Destroy only an anonymous per-scope UTag.  A user-owned tag
     * (v0.10.9-B, FLAG_TAG_USER_OWNED) outlives the scope: it is still
     * reachable via the user's variable and may have other open member
     * scopes, so destroying it here would be a use-after-free.
     * Precondition for the anonymous case (checked by utag_destroy's
     * assertion): member lists must be empty — we unlinked the only
     * member above. */
    if (tag != NULL && (top_flags & FLAG_TAG_USER_OWNED) == 0U) {
        utag_destroy(s->vm, tag);
    }
}

UVmTagScopeResult
vm_pop_tag_scope(UVM *vm, UStrand *s)
{
    /* OP_POP_TAG ABC: A = tag_reg (unused at M3), B = C = 0.
     * Pop the top UCLEANUP_TAG_SCOPE entry.
     * If FLAG_HAS_ONLEAVE is set in the entry's flags, the onleave
     * handler would run via run_cleanup_with_replace — but at M3
     * flags is always 0 (no onleave body is emitted), so the handler
     * branch is dead code.  Include the check for forward-compatibility. */
    if (s->cleanup_depth > 0) {
        UCleanupEntry *top = &s->cleanup_base[s->cleanup_depth - 1];
        if ((top->flags & FLAG_HAS_ONLEAVE) != 0U) {
            /* onleave handler: not reachable at M3 (emit always sets flags=0).
             * If somehow reached (bytecode corruption), halt safely. */
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm, "POP_TAG: FLAG_HAS_ONLEAVE not wired at M3");
            return UVM_TAG_SCOPE_HALT;
        }
        vm_tag_scope_teardown(s, top);
    }
    return UVM_TAG_SCOPE_NEXT;
}
