/* SPDX-License-Identifier: BSD-3-Clause */
/* src/vm/uvm_tag_scope.h — OP_PUSH_TAG / OP_POP_TAG dispatch helpers
 * (v0.10.15-vm-decomp-2, W1 stage 1).
 *
 * Mirrors the UVmSlotResult extraction pattern (uvm_slot.h, v0.10.4): each arm
 * body moves into a helper taking (UVM *vm, UStrand *s); the dispatch-loop
 * control transfers (goto exit_strand / HALT() / the fall-through NEXT()) become
 * return codes the arm switches on, since a helper cannot goto a caller label.
 *
 * Consumed only by uvm.c and uvm_tag_scope.c.  NOT part of the public API;
 * no versioning obligation. */

#ifndef UVM_TAG_SCOPE_H
#define UVM_TAG_SCOPE_H

#include "vm/uvm.h"
#include "sched/ustrand.h"

/* UVmTagScopeResult — return codes from vm_push_tag_scope / vm_pop_tag_scope.
 *
 * NEXT  — arm continues with NEXT().
 * FATAL — arm does `goto exit_strand` (helper already set s->fatal_status =
 *         UEXEC_THROW and s->state = USTRAND_STATE_DEAD).
 * HALT  — arm does HALT() (helper already set vm->last_error + a diagnostic). */
typedef enum {
    UVM_TAG_SCOPE_NEXT = 0,
    UVM_TAG_SCOPE_FATAL,
    UVM_TAG_SCOPE_HALT
} UVmTagScopeResult;

/* vm_push_tag_scope: execute the OP_PUSH_TAG body for the instruction at s->pc.
 *
 * Stage 1 (v0.10.15): byte-for-byte the v0.10.14 anonymous-tag behavior — each
 * tag scope gets its own fresh anonymous UTag from the cleanup stack; the
 * A[3:0] tag_reg nibble is ignored.  W2 (v0.10.9-B) extends this to honor the
 * nibble and bind the scope to the user tag.
 *
 * Returns UVM_TAG_SCOPE_NEXT on success, UVM_TAG_SCOPE_FATAL on a utag_create
 * or cleanup-stack-overflow allocation failure. */
UVmTagScopeResult vm_push_tag_scope(UVM *vm, UStrand *s);

/* vm_pop_tag_scope: execute the OP_POP_TAG body for the instruction at s->pc.
 *
 * Pops the top UCLEANUP_TAG_SCOPE entry: unlinks it from the tag's member
 * list, fires the tier-2 leave event, cascades member watchers to the
 * pending-onleave queue, pops the cleanup entry, and destroys the anonymous
 * UTag.  Returns UVM_TAG_SCOPE_NEXT normally, UVM_TAG_SCOPE_HALT on the
 * (v1.0-dead) FLAG_HAS_ONLEAVE defensive branch. */
UVmTagScopeResult vm_pop_tag_scope(UVM *vm, UStrand *s);

#endif /* UVM_TAG_SCOPE_H */
