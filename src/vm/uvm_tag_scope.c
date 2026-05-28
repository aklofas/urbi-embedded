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
#include "tag/utag.h"                 /* UTag, utag_create/destroy */
#include "watcher/uwatcher.h"         /* UWatcher, pending_onleave_queue_push */
#include "event/uevent.h"             /* UEvent */
#include "event/uevent_emit.h"        /* c_event_emit_sync */
#include "value/uvalue.h"             /* UValue, UVAL_NIL */

#include <stddef.h>
#include <stdint.h>

UVmTagScopeResult
vm_push_tag_scope(UVM *vm, UStrand *s)
{
    /* OP_PUSH_TAG ABx:
     *   A[7:4] = flags nibble (0 at M3 — no FLAG_HAS_ONLEAVE)
     *   A[3:0] = reserved (currently unused at runtime; the emitter
     *            packs a tag_reg here per uemit_push_tag, but the
     *            dispatch path creates an anonymous UTag from the
     *            cleanup stack and never reads this nibble — the
     *            register binding is reserved for a future feature
     *            where the tag is exposed to a register slot)
     *   Bx     = onleave_pc (handler PC; 0 at M3 since no onleave body)
     *
     * T30: allocate a per-scope UTag (no UVAL_TAG / register binding at M3).
     * Each tag-scope gets its own anonymous UTag; the tag's lifetime is
     * bounded by the corresponding OP_POP_TAG.
     * Walker-pop (urbi_unwind via OP_THROW etc.) will leak the UTag at M3 —
     * deferred for T31/walker integration when full tag lifecycle wires through.
     * strand_back = s for future tag.stop() walk (T31 uses). */
    (void)vm;  /* this stage reads the VM via s->vm; vm kept for symmetry + W2 */
    uint8_t  a          = uinstr_a(*s->pc);
    uint8_t  flags      = (uint8_t)((a >> 4) & 0xFU);
    uint16_t handler_pc = uinstr_bx(*s->pc);
    UTag *tag = utag_create(s->vm);
    if (tag == NULL) {
        s->fatal_status = UEXEC_THROW;
        s->state        = USTRAND_STATE_DEAD;
        return UVM_TAG_SCOPE_FATAL;
    }
    UCleanupEntry *entry = strand_cleanup_push(s);
    if (entry == NULL) {
        utag_destroy(s->vm, tag);  /* roll back the tag alloc on overflow */
        s->fatal_status = UEXEC_THROW;
        s->state        = USTRAND_STATE_DEAD;
        return UVM_TAG_SCOPE_FATAL;
    }
    entry->kind           = (uint8_t)UCLEANUP_TAG_SCOPE;
    entry->flags          = flags;
    entry->handler_pc     = handler_pc;
    entry->register_base  = 0U;
    entry->register_count = 0U;
    entry->owning_tag     = tag;
    entry->catch_pattern  = NULL;
    entry->next_member    = tag->member_strands_head;  /* head-insert */
    entry->strand_back    = s;
    tag->member_strands_head = entry;
    /* VM-015: enter_event is unconditionally NULL on a fresh utag_create
     * (utag.c zero-fills enter_event/leave_event at allocation; only the
     * tag.enter native getter — invoked through a Tag.enter property
     * read — lazy-allocates the UEvent later in tag_enter_getter).  At
     * OP_PUSH_TAG the tag was just created on the line above and no
     * code has had access to it; therefore tag->enter_event MUST be
     * NULL here.  The original T55 "tier-2 enter event hook" branch
     * (load + null-check + at_watchers_head load) was dead at every
     * v1.0 dispatch and is removed; M6 wires Tag.enter through a
     * different path (subscribers register on the lazy-alloc'd event
     * after the tag escapes via a register binding, never during
     * OP_PUSH_TAG itself).  The assertion pins the contract. */
    URBI_INTERNAL_ASSERT(tag->enter_event == NULL);
    return UVM_TAG_SCOPE_NEXT;
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
    return UVM_TAG_SCOPE_NEXT;
}
