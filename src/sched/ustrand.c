/* SPDX-License-Identifier: BSD-3-Clause */
/* UStrand lifecycle.
   T20 adds urbi_strand_create/start/spawn/destroy (full lifecycle C API).
   T29 adds urbi_strand_attach_ambient_tags + urbi_strand_capture_ambient_chain.
   Cleanup-stack wiring: T3 (this file). */

#include "sched/ustrand.h"
#include "runtime/ucleanup.h"
#include "runtime/uclosure.h"
#include "tag/utag.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "sched/usched_cooperative.h"
#include "urbi/urbi.h"
#include "runtime/umacros.h"
#include "module/umodule.h"
#include "runtime/uframe.h"
#include <stddef.h>
#include <stdint.h>

int
ustrand_init(UStrand *s, struct UVM *vm) {
    urbi_zero(s, sizeof(*s));
    s->vm    = vm;
    s->state = USTRAND_STATE_DORMANT;
    /* Pre-allocate the cleanup stack using the VM's pluggable allocator.
     * CHSTR-010: propagate OOM (return -1) instead of silently discarding.
     * Frame-stack / register-window / lex-env init is deferred to urbi_step
     * or urbi_strand_arm_from_closure; the strand is a valid DORMANT without it. */
    if (vm != NULL) {
        if (strand_cleanup_stack_init(s, vm, URBI_CLEANUP_MAX) != 0) {
            return -1;
        }
    }
    /* When vm is NULL the strand has no cleanup stack; callers that omit vm
       must call strand_cleanup_stack_init explicitly before use. */
    return 0;
}

/* strand_unlink_from_tags
 *
 * Walk all TAG_SCOPE entries on s's cleanup-stack and unlink s from each
 * owning_tag's member_strands_head list.  Called by ustrand_destroy to maintain
 * the §3.4 membership invariant before the cleanup-stack array is freed.
 *
 * The normal production path runs this via the row 7 walker during unwind;
 * this ensures correctness when strands are torn down without executing bytecode
 * (e.g., test teardown, OOM recovery, urbi_strand_panic). */
static void
strand_unlink_from_tags(UStrand *s)
{
    uint16_t i;

    if (s->cleanup_base == NULL || s->cleanup_depth == 0) return;

    for (i = 0; i < s->cleanup_depth; i++) {
        UCleanupEntry *e = &s->cleanup_base[i];
        UTag *tag;
        UCleanupEntry **cur;

        if (e->kind != UCLEANUP_TAG_SCOPE) continue;
        tag = e->owning_tag;
        if (tag == NULL) continue;

        /* Unlink e from tag->member_strands_head singly-linked list. */
        cur = &tag->member_strands_head;
        while (*cur != NULL) {
            if (*cur == e) {
                *cur = e->next_member;
                e->next_member = NULL;
                break;
            }
            cur = &(*cur)->next_member;
        }
    }
}

/* === CHSTR-029: release_strand_resource_chain ===
 *
 * Free the three allocation chains owned by a strand: closure_list,
 * closed_cells, and open_upvals.  Centralises the near-identical free loops
 * that previously appeared in ustrand_destroy.
 *
 * closure_list: skips any closure that equals vm->last_return_closure (the
 * caller-owned return value kept alive between urbi_vm_run calls).
 *
 * urbi_vm_run pre-frees these chains itself (before calling ustrand_destroy) to
 * avoid the skip-logic for last_return_closure; by the time ustrand_destroy
 * is called from that path the three pointers are NULL and these loops are
 * no-ops. */
static void
release_strand_resource_chain(UVM *vm, UStrand *s)
{
    UClosure  *cl;
    UUpvalCell *cell;

    /* Closure list (pre-GC closure bookkeeping). */
    cl = s->closure_list;
    s->closure_list = NULL;
    while (cl != NULL) {
        UClosure *next = cl->next_alloc;
        if (cl != vm->last_return_closure)
            vm->alloc_fn(cl, 0, vm->alloc_ud);
        cl = next;
    }

    /* Heapified upvalue cells. */
    cell = s->closed_cells;
    s->closed_cells = NULL;
    while (cell != NULL) {
        UUpvalCell *next = cell->next;
        vm->alloc_fn(cell, 0, vm->alloc_ud);
        cell = next;
    }

    /* Open upvalue cells (not closed before strand death). */
    cell = s->open_upvals;
    s->open_upvals = NULL;
    while (cell != NULL) {
        UUpvalCell *next = cell->next;
        vm->alloc_fn(cell, 0, vm->alloc_ud);
        cell = next;
    }
}

void
ustrand_destroy(UStrand *s, struct UVM *vm) {
    /* CHSTR-031: cross-strand stop counter management moved to scheduler.
     * sched_strand_account_destroy handles the host_call_pending_count
     * bookkeeping for strands that had a cross-strand stop deposited. */
    if (vm != NULL)
        sched_strand_account_destroy(vm, s);

    /* Unlink all TAG_SCOPE entries from their owning tags before freeing
       the cleanup stack.  This maintains the §3.4 membership invariant
       and allows utag_destroy to assert an empty member list. */
    strand_unlink_from_tags(s);

    /* Free the pre-allocated cleanup stack if vm is available. */
    if (vm != NULL && s->cleanup_base != NULL) {
        strand_cleanup_stack_destroy(s, vm);
    }

    /* CHSTR-044: register-stack free via urbi_strand_register_stack_free.
     * urbi_vm_run frees its own transient strand's stack before calling
     * ustrand_destroy, so double-free is not a risk there (stack is NULL). */
    if (vm != NULL)
        urbi_strand_register_stack_free(s, vm);

    /* CHSTR-029: three resource chains consolidated into one helper. */
    if (vm != NULL)
        release_strand_resource_chain(vm, s);
}

/* === T20: Strand C API (create / start / spawn / destroy) ===
 *
 * These functions implement the strand lifecycle as specified in row 9 §9.1.
 * Separate _create (DORMANT alloc) from _start (DORMANT → READY enqueue) so
 * callers can pre-attach tags (T29), set scheduler attrs (v1.x), or pool/recycle
 * strands before making them runnable.  _spawn is the convenience composite. */

UStrand *
urbi_strand_create(struct URealm *realm, struct UClosure *entry)
{
    struct UVM *vm = realm->vm;
    URBI_ASSERT_NOT_ISR(vm);

    /* Allocate via VM pluggable allocator (no stdlib calloc — freestanding). */
    UStrand *s = (UStrand *)vm->alloc_fn(NULL, sizeof(UStrand), vm->alloc_ud);
    if (!s) return NULL;

    /* CHSTR-010: check OOM from ustrand_init (cleanup-stack alloc failure). */
    if (ustrand_init(s, vm) != 0) {
        vm->alloc_fn(s, 0, vm->alloc_ud);
        return NULL;
    }
    s->realm         = realm;
    s->entry_closure = entry;

    /* T38: head-insert into realm's strand ownership list so that
     * urbi_realm_destroy can free all heap-allocated child strands. */
    s->next_in_realm   = realm->strands_head;
    realm->strands_head = s;

    /* Row 11 §4.1: chunk-start ambient = [realm->tag].
       Attach synthetic TAG_SCOPE entries so this strand appears in the
       realm tag's member_strands_head list.  On overflow, attach sets
       s->fatal_status = UEXEC_CANCEL and state = DEAD (detectable by caller). */
    if (realm->tag != NULL) {
        UTag *chain[1];
        chain[0] = realm->tag;
        urbi_strand_attach_ambient_tags(s, chain, 1);

        /* CHSTR-018 + CHSTR-040: enforce the "fully-functional strand or NULL"
         * contract documented at urbi.h:194.  attach_ambient_tags transitions
         * the strand to DEAD on cleanup-stack overflow; without this guard
         * urbi_strand_create would return a DEAD-but-allocated strand that
         * is already linked into realm->strands_head, leaking the partial
         * allocation onto callers that only check the return value for NULL.
         * Unlink from realm, tear the strand down, and return NULL so the
         * single OOM/overflow path mirrors the other failure modes. */
        if (USTRAND_GET_STATE(s) == USTRAND_DEAD) {
            URBI_INTERNAL_ASSERT(realm->strands_head == s);
            realm->strands_head = s->next_in_realm;
            s->next_in_realm    = NULL;
            ustrand_destroy(s, vm);
            vm->alloc_fn(s, 0, vm->alloc_ud);
            return NULL;
        }
    }

    sched_strand_init(s, NULL);

    /* Execution state (stack, R, pc, etc.) is zero-init; subsequent
       activation by urbi_step or a future urbi_strand_arm helper sets up
       frame-0 from entry_closure. */

    /* state stays USTRAND_DORMANT — sched_strand_init does not change state. */
    return s;
}

void
urbi_strand_start(UStrand *s)
{
    struct UVM *vm = s->vm;
    URBI_ASSERT_NOT_ISR(vm);
    (void)vm;  /* suppress -Wunused-variable in non-debug builds */
    URBI_INTERNAL_ASSERT(USTRAND_GET_STATE(s) == USTRAND_DORMANT);
    sched_strand_make_runnable(s);
}

UStrand *
urbi_strand_spawn(struct URealm *realm, struct UClosure *entry)
{
    struct UVM *vm = realm->vm;
    URBI_ASSERT_NOT_ISR(vm);
    (void)vm;  /* suppress -Wunused-variable in non-debug builds */
    UStrand *s = urbi_strand_create(realm, entry);
    if (s) urbi_strand_start(s);
    return s;
}

void
urbi_strand_destroy(UStrand *s)
{
    struct UVM *vm;
    if (!s) return;
    vm = s->vm;
    if (vm) URBI_ASSERT_NOT_ISR(vm);

    /* T38: unlink from realm->strands_head singly-linked list so that
     * urbi_realm_destroy (which walks strands_head) does not encounter a
     * freed strand if the caller already called urbi_strand_destroy directly.
     * Walk from head; O(n) but only called at strand teardown. */
    if (s->realm != NULL && s->realm->strands_head != NULL) {
        UStrand **pp = &s->realm->strands_head;
        while (*pp != NULL) {
            if (*pp == s) {
                *pp = s->next_in_realm;
                s->next_in_realm = NULL;
                break;
            }
            pp = &(*pp)->next_in_realm;
        }
    }

    /* CHSTR-015 (T103): unbind the strand from any scheduler queue BEFORE
     * sched_strand_destroy zeroes the local ready_next/ready_prev pointers
     * that sched_strand_unbind_from_ready_queue walks to fix up neighbours.
     * sched_strand_destroy is then a pure local-pointer wipe; the strand's
     * neighbours and the queue head/tail are already consistent.
     *
     * ustrand_destroy follows so that the cleanup-stack unwind / register-
     * stack free / resource-chain release run with a strand that is no
     * longer reachable from the scheduler — eliminates the race window
     * where a concurrent sched_walk_roots (M3 cooperative: not actually
     * concurrent, but spec-level "the GC sees the strand on the queue
     * after we started tearing it down") would walk freed memory. */
    if (vm != NULL) {
        sched_strand_unbind_from_ready_queue(s);
        sched_strand_unbind_from_sleep_queue(s);
    }
    sched_strand_destroy(s);
    ustrand_destroy(s, vm);
    if (vm) vm->alloc_fn(s, 0, vm->alloc_ud);
}

/* === T29: ambient-tag inheritance helpers ===
 *
 * Row 11 §4.1 / §4.3. */

/* urbi_strand_capture_ambient_chain
 *
 * Walk `parent`'s cleanup-stack bottom-up (index 0 → depth-1) collecting
 * the owning_tag from every TAG_SCOPE entry into out_chain[].
 * Returns the number of tags collected, or SIZE_MAX on truncation. */

size_t
urbi_strand_capture_ambient_chain(struct UStrand *parent,
                                  struct UTag   **out_chain,
                                  size_t          out_cap)
{
    size_t n = 0;
    uint16_t i;

    if (parent == NULL || parent->cleanup_base == NULL) return 0;

    for (i = 0; i < parent->cleanup_depth; i++) {
        UCleanupEntry *e = &parent->cleanup_base[i];
        if (e->kind != UCLEANUP_TAG_SCOPE) continue;
        if (n >= out_cap) return (size_t)-1;  /* SIZE_MAX */
        out_chain[n++] = e->owning_tag;
    }
    return n;
}

/* urbi_strand_attach_ambient_tags
 *
 * Push synthetic TAG_SCOPE cleanup-entries onto `new_s` for each tag in
 * chain[0..chain_count-1].  chain[0] is the bottommost tag (pushed first),
 * so it ends up at the bottom of the cleanup-stack and pops last.
 *
 * Each synthetic entry has:
 *   kind            = UCLEANUP_TAG_SCOPE
 *   flags           = 0 (no onleave — synthetic entries never own cleanup logic)
 *   register_base   = 0, register_count = 0
 *   handler_pc      = 0
 *   owning_tag      = chain[i]
 *   catch_pattern   = NULL
 *   strand_back     = new_s
 *   next_member     = chain[i]->member_strands_head (head-insert into tag's list)
 *
 * On cleanup-stack overflow: sets new_s->fatal_status = UEXEC_CANCEL,
 * new_s->fatal_value = NIL, new_s->state = USTRAND_STATE_DEAD, and returns.
 * The create caller can detect this via s->fatal_status. */

void
urbi_strand_attach_ambient_tags(struct UStrand *new_s,
                                struct UTag   **chain,
                                size_t          chain_count)
{
    size_t i;

    if (new_s == NULL || chain_count == 0) return;
    URBI_ASSERT_NOT_ISR(new_s->vm);

    for (i = 0; i < chain_count; i++) {
        UCleanupEntry *e = strand_cleanup_push(new_s);
        if (e == NULL) {
            /* Cleanup-stack overflow — strand cannot start safely. */
            new_s->fatal_status = UEXEC_CANCEL;
            new_s->fatal_value.kind = UVAL_NIL;
            new_s->fatal_value.v.i  = 0;
            new_s->state            = USTRAND_STATE_DEAD;
            return;
        }

        /* Zero-init the entry (strand_cleanup_push returns a pointer into
           the pre-zeroed allocation, but be explicit for each used field). */
        e->kind           = (uint8_t)UCLEANUP_TAG_SCOPE;
        e->flags          = 0;
        e->register_base  = 0;
        e->register_count = 0;
        e->handler_pc     = 0;
        e->owning_tag     = chain[i];
        e->catch_pattern  = NULL;
        e->strand_back    = new_s;
        e->next_member    = chain[i]->member_strands_head;

        /* Head-insert this entry into the tag's member_strands_head list. */
        chain[i]->member_strands_head = e;
    }
}

/* === CHSTR-044: register-stack lifecycle triplet ===
 *
 * Three-stage lifecycle for the per-strand UValue register stack.
 * Centralises alloc / zero / free so each stage has one owner. */

int
urbi_strand_register_stack_alloc(UStrand *s, struct UVM *vm)
{
    const size_t stack_bytes = UVM_STACK_CAP * sizeof(UValue);
    UValue *stack = (UValue *)vm->alloc_fn(NULL, stack_bytes, vm->alloc_ud);
    if (!stack) return -1;
    s->stack = stack;
    s->R     = stack;
    return 0;
}

void
urbi_strand_register_stack_zero(UStrand *s)
{
    urbi_zero(s->stack, UVM_STACK_CAP * sizeof(UValue));
}

void
urbi_strand_register_stack_free(UStrand *s, struct UVM *vm)
{
    if (s->stack == NULL) return;
    vm->alloc_fn(s->stack, 0, vm->alloc_ud);
    s->stack = NULL;
    s->R     = NULL;
}

/* === CHSTR-022: urbi_strand_arm_init ===
 *
 * Convenience composite: alloc + zero the register stack and wire s->R.
 * Shared by urbi_strand_arm_from_closure and urbi_vm_run; each caller wires
 * pc/pc_base/cur_consts/out_slot/state afterward.
 *
 * Precondition (CHSTR-005): s->stack must be NULL on entry.  Re-arming a
 * strand that already owns a register stack would leak the prior allocation
 * because urbi_strand_register_stack_alloc unconditionally overwrites s->stack.
 * Re-use is supported only via the explicit free → arm sequence (e.g.
 * urbi_strand_register_stack_free followed by a fresh arm); enforced here
 * so violations surface in debug builds.
 *
 * Returns 0 on success, -1 on allocation failure (s->stack remains NULL). */
int
urbi_strand_arm_init(UStrand *s)
{
    URBI_INTERNAL_ASSERT(s->stack == NULL);
    if (urbi_strand_register_stack_alloc(s, s->vm) != 0) return -1;
    urbi_strand_register_stack_zero(s);
    return 0;
}

/* === spec #1 §5.5: urbi_strand_arm_from_closure ===
 *
 * Lifted from the inlined stack-alloc + pc-arming block in fork_spawn_child
 * (uop_fork.c) so the watcher body-spawn path (T24) can reuse it.
 * Delegates stack alloc+zero to urbi_strand_arm_init (CHSTR-022); wires
 * pc/pc_base/cur_consts and clears exec-state fields from entry.
 *
 * Callers that need s->module set (e.g. fork_spawn_child) must do so
 * explicitly after this call returns 0. */
int
urbi_strand_arm_from_closure(UStrand *s, struct UClosure *entry)
{
    if (urbi_strand_arm_init(s) != 0) return -1;

    s->pc         = entry->proto->instructions;
    s->pc_base    = entry->proto->instructions;
    s->cur_consts = entry->proto->constants
                  ? entry->proto->constants
                  : s->cur_consts;   /* keep existing pool if proto has none */
    s->frame_count  = 0;
    s->open_upvals  = NULL;
    s->closure_list = NULL;
    s->closed_cells = NULL;
    s->out_slot     = NULL;
    return 0;
}
