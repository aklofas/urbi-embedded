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
#include "chunk/uchunk.h"
#include "object/uchunk_instance.h"
#include "runtime/uframe.h"
#include "event/uevent_emit.h"  /* uevent_waiter_unregister (scheduler F1) */
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
 * (e.g., test teardown, OOM recovery, urbi_strand_panic).
 *
 * CHSTR-051: scan the FULL allocated range [0..cleanup_cap) rather than just
 * [0..cleanup_depth).  strand_cleanup_pop decrements cleanup_depth but does
 * NOT unlink the entry from owning_tag->member_strands_head; entries below
 * the current depth may therefore still be linked.  This arises in the fatal
 * unwind path for "try { throw 1 } finally { throw 2 }": the realm-tag
 * TAG_SCOPE at cleanup_base[0] is popped during unwind (depth → 0) but its
 * member_strands_head link is never cleared, so utag_destroy's invariant
 * assertion fires.  Scanning the full cap is safe: never-pushed slots are
 * zero-initialised (kind == 0, not UCLEANUP_TAG_SCOPE == 2) and are skipped;
 * already-unlinked popped entries harmlessly produce a no-op list walk. */
/* strand_unlink_member_entry
 *
 * Unlink a single UCleanupEntry from its owning_tag->member_strands_head
 * singly-linked list.  No-op if entry->owning_tag is NULL or entry is
 * not on the list (defensive — should always be on it when called from
 * a strand that opened the scope normally).
 *
 * v0.10.10: extracted from strand_unlink_from_tags' inline loop so the
 * disown helper can call it on a single entry without walking the whole
 * cleanup stack. */
void
strand_unlink_member_entry(UCleanupEntry *e)
{
    UTag *tag;
    UCleanupEntry **cur;

    if (e == NULL || e->kind != UCLEANUP_TAG_SCOPE) return;
    tag = e->owning_tag;
    if (tag == NULL) return;

    cur = &tag->member_strands_head;
    while (*cur != NULL) {
        if (*cur == e) {
            *cur = e->next_member;
            e->next_member = NULL;
            return;
        }
        cur = &(*cur)->next_member;
    }
}

static void
strand_unlink_from_tags(UStrand *s)
{
    uint16_t i;

    if (s->cleanup_base == NULL || s->cleanup_cap == 0) return;

    for (i = 0; i < s->cleanup_cap; i++) {
        strand_unlink_member_entry(&s->cleanup_base[i]);
    }
}

/* === CHSTR-029: release_strand_resource_chain (v0.8.4 Option B Step C-3) ===
 *
 * Clear open_upvals head pointer on the strand.  UClosure and UUpvalCell are
 * GC-managed since Step C-2; the GC sweep reclaims them when they become
 * unreachable from any root.  closure_list + closed_cells were deleted at
 * Step C-3.  open_upvals is cleared here so the GC root walk for this strand
 * stops following the chain (cells are still reachable via closures' upvals[]
 * arrays if live). */
static void
release_strand_resource_chain(UVM *vm, UStrand *s)
{
    (void)vm;
    s->open_upvals = NULL;
}

/* strand_cleanup_observers
 *
 * Scheduler F1: single point of truth for "this strand will never run again —
 * scrub all external references to it."  Called by ustrand_destroy so that
 * EVERY teardown path (urbi_strand_destroy public API, urbi_realm_destroy
 * sweep, OOM early-exit in urbi_strand_create) performs the same scrub.
 *
 * Two classes of external reference:
 *
 *   1. Event-waiter chain: if s is parked on a UEvent.waiters_head chain,
 *      splice it out now.  Without this, the next urbi_event_emit call walks
 *      the chain and dereferences freed memory.  uevent_waiter_unregister is
 *      idempotent (returns immediately if s->wait_event_target == NULL) so
 *      calling it even when the strand was not waiting is safe.
 *
 *   2. Join-blocked parents: any parent strand threaded onto s->joiners_head
 *      via OP_JOIN_WAIT would block forever because nothing else will ever
 *      wake it.  The walk here clears joiners_head before walking so
 *      re-entrant calls are no-ops (idempotent).  The walk also guards against
 *      DEAD joiners (which can appear in adversarial teardown sequences) so the
 *      teardown path is safe regardless of joiner state.
 *
 * exit_strand in uvm.c (the normal dispatch-loop teardown path) also calls
 * fork_wake_joiners.  That call is kept as an "eager wake while we still have
 * CPU" optimisation; it becomes a no-op here because joiners_head is cleared
 * on the first call.  Similarly, uevent_waiter_unregister in the unwind/cancel
 * paths is idempotent, so double-calls are safe. */
static void
strand_cleanup_observers(UStrand *s)
{
    /* Splice out of event waiter chain if parked on waituntil(). */
    uevent_waiter_unregister(s);

    /* Wake any JOIN-blocked parents so they do not stall forever.
     * Inline the walk from fork_wake_joiners with a DEAD-joiner guard:
     * in adversarial teardown sequences a joiner may itself have been
     * forcibly DEAD before the child is destroyed (e.g., in test teardown
     * or multi-realm destroy ordering).  Calling sched_strand_make_runnable
     * on a DEAD strand trips an assertion in hosted builds; guard here so the
     * teardown path is safe regardless of joiner state.  Joiners in WAITING
     * or any other live state are woken normally. */
    if (s->joiners_head != NULL) {
        UStrand *joiner = s->joiners_head;
        s->joiners_head = NULL;  /* clear first — idempotent on re-entry */
        while (joiner != NULL) {
            UStrand *next_joiner = joiner->wait_next;
            joiner->wait_next = NULL;
            if (USTRAND_GET_STATE(joiner) != USTRAND_DEAD)
                sched_strand_make_runnable(joiner);
            joiner = next_joiner;
        }
    }
}

void
ustrand_destroy(UStrand *s, struct UVM *vm) {
    /* Scheduler F1: unregister from event-waiter chains and wake any
     * join-blocked parents BEFORE tearing down the strand's own resources.
     * This ensures every teardown path (urbi_strand_destroy, realm-destroy
     * sweep, OOM early-exit) has identical external-reference cleanup, not
     * just the normal dispatch-loop path through exit_strand. */
    if (vm != NULL)
        strand_cleanup_observers(s);

    /* v0.8.1 Phase 2 (Variant B fusion): drop strand-bind ref on root_proto.
     * Pairs with the bump in urbi_strand_create_for_module (below), uvm_run.c
     * (transient path), and uop_fork.c (child spawn).
     * v0.9.2: s->module deleted; use s->root_proto directly.
     * Null after — prevents double-dec on pool recycle paths. */
    if (s->root_proto != NULL) {
        uproto_strand_refcount_dec(s->root_proto, vm);
        s->root_proto = NULL;
    }

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
     * ustrand_destroy, so double-free is not a risk there (stack is NULL).
     *
     * CHSTR-004: explicit s->stack != NULL guard pins the contract at the
     * call site rather than relying on urbi_strand_register_stack_free's
     * internal NULL-check.  Two-layer guard: future maintainers reading
     * ustrand_destroy can see locally that the helper is safe to call on a
     * pre-freed strand without having to chase the helper's body. */
    if (vm != NULL && s->stack != NULL)
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
urbi_strand_create(struct UVM *vm, struct URealm *realm, struct UClosure *entry)
{
    if (!vm) vm = realm ? realm->vm : NULL;
    if (!vm) return NULL;
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
urbi_strand_start(struct UVM *vm, UStrand *s)
{
    (void)vm;  /* mirrors s->vm; accepted for API convention */
    URBI_ASSERT_NOT_ISR(s->vm);
    /* CHSTR-033 (T104): the precondition is "DORMANT only — no double-start".
     * sched_strand_make_runnable unconditionally tail-inserts into the
     * cooperative ready queue and bumps strand_runnable_count++; calling
     * urbi_strand_start twice on the same strand would re-enqueue it
     * (creating a circular ready_next/ready_prev chain because the strand
     * is already a list member) and double-count the runnable counter so
     * sched_quiescent never converges.  The URBI_INTERNAL_ASSERT below
     * catches this in -DURBI_DEBUG builds, BUT it is a no-op in freestanding
     * production builds (umacros.h defaults to (void)0 when assert.h is
     * unavailable).  TODO(v1.x): consider promoting this to urbi_panic so
     * the violation is fatal in production rather than silently corrupting
     * the queue accounting.  Current callers (urbi_strand_spawn,
     * application code) all transition DORMANT → READY exactly once. */
    URBI_INTERNAL_ASSERT(USTRAND_GET_STATE(s) == USTRAND_DORMANT);
    URBI_TP(vm, URBI_TRACE_SCHED, URBI_LOG_DEBUG, URBI_TP_SCHED_START,
            (uint32_t)(uintptr_t)s, 0);
    sched_strand_make_runnable(s);
}

UStrand *
urbi_strand_spawn(struct UVM *vm, struct URealm *realm, struct UClosure *entry)
{
    if (!vm) vm = realm ? realm->vm : NULL;
    URBI_ASSERT_NOT_ISR(vm);
    UStrand *s = urbi_strand_create(vm, realm, entry);
    if (s) urbi_strand_start(vm, s);
    return s;
}

int
urbi_strand_destroy(struct UVM *vm, UStrand *s)
{
    int rc = URBI_OK;
    if (!s) return URBI_OK;
    if (!vm) vm = s->vm;
    if (vm) URBI_ASSERT_NOT_ISR(vm);

#if URBI_DEBUG
    /* api-ergonomics F8: warn in debug builds if the strand is in an active
     * state.  Log the problem and record the error code, but always proceed
     * with teardown — realm-destroy calls this on strands in any state and
     * must complete cleanup (e.g. strand_unlink_from_tags) regardless.
     * Release builds skip the check (pre-v1.0 permissive posture). */
    {
        uint8_t st = USTRAND_GET_STATE(s);
        if (st != USTRAND_DORMANT && st != USTRAND_DEAD) {
            if (vm != NULL && vm->host_log_fn != NULL) {
                vm->host_log_fn(vm, vm->host_log_ud, (int)URBI_LOG_ERROR,
                    "urbi_strand_destroy: strand in active state 0x%02x", st);
            }
            rc = URBI_ERR_INVALID_STATE;
            /* fall through — cleanup must complete to maintain tag invariants */
        }
    }
#endif

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
    return rc;
}

/* urbi_strand_state — query observable strand lifecycle state.
 * Maps the packed internal state byte to the public UStrandState enum.
 * NULL strand returns URBI_STRAND_DEAD (safe to destroy). */
UStrandState
urbi_strand_state(struct UVM *vm, const struct UStrand *s)
{
    uint8_t raw;
    (void)vm;
    if (!s) return URBI_STRAND_DEAD;
    raw = USTRAND_GET_STATE(s);
    switch (raw) {
        case USTRAND_DORMANT: return URBI_STRAND_DORMANT;
        case USTRAND_READY:   return URBI_STRAND_READY;
        case USTRAND_RUNNING: return URBI_STRAND_RUNNING;
        case USTRAND_WAITING: return URBI_STRAND_WAITING;
        case USTRAND_DEAD:    /* fall through */
        default:              return URBI_STRAND_DEAD;
    }
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

        /* Zero the entry up front so any future UCleanupEntry field gains
         * a defined initial value without a per-call-site touch (CHSTR-032).
         * Then assign the live fields. strand_cleanup_push hands back a slot
         * inside the pre-zeroed cleanup_base, but slots are reused across
         * push/pop cycles so a fresh zero per push is the safe contract. */
        urbi_zero(e, sizeof(*e));
        /* frame_depth: 0 via urbi_zero — fresh strand (VM-01) */
        e->kind           = (uint8_t)UCLEANUP_TAG_SCOPE;
        e->owning_tag     = chain[i];
        e->strand_back    = new_s;
        e->next_member    = chain[i]->member_strands_head;

        /* Head-insert this entry into the tag's member_strands_head list. */
        chain[i]->member_strands_head = e;
    }
}

/* === v0.8.0: urbi_strand_create_for_module ===
 *
 * Allocates a non-transient strand bound to a module's root chunk.
 * Mirrors the setup in uvm_run.c::urbi_vm_run, minus the transient flag
 * and stack-local allocation:
 *   - urbi_strand_create handles: pool alloc, ustrand_init (cleanup stack),
 *     realm link (next_in_realm + strands_head insert), ambient-tag attach,
 *     sched_strand_init.  Leaves strand DORMANT.
 *   - This function adds: module bind + refcount, register-stack arm,
 *     execution-state wiring, UChunkInstance creation.
 *   - urbi_strand_start transitions DORMANT → READY.
 *
 * OOM recovery: any allocation failure after urbi_strand_create succeeds
 * tears down the strand via urbi_strand_destroy (which unlinks from realm,
 * drops refcount if already bumped, and frees all resources). */
UStrand *
urbi_strand_create_for_module(struct UVM *vm, struct URealm *realm,
                              struct UProto *root)
{
    if (!vm || !root) return NULL;
    if (root->instr_count == 0) return NULL;

    if (realm == NULL) {
        realm = urbi_realm_global(vm);
        if (realm == NULL) return NULL;
    }

    /* Pool-allocate a non-transient strand (is_transient_strand stays 0).
     * entry_closure = NULL: chunk-top has no closure; root instructions come
     * from root->instructions directly.  urbi_strand_create handles:
     * ustrand_init (cleanup stack alloc), realm link, ambient-tag attach,
     * sched_strand_init.  Leaves strand DORMANT. */
    UStrand *s = urbi_strand_create(vm, realm, NULL);
    if (!s) return NULL;

    /* Bind root and bump refcount before any teardown path so
     * urbi_strand_destroy (which calls ustrand_destroy) correctly decrements
     * the count on error.
     * v0.9.2: s->module deleted; root_proto IS the sole chunk identity. */
    s->root_proto = root;
    /* v0.10.1 W4: typed-handle acquire for diagnostics (F3 strand-bind site). */
    urbi_proto_strand_ref_acquire(s->root_proto, URBI_PROTO_REF_OWNER_STRAND);

    /* Allocate and zero the per-strand register stack.
     * On failure: urbi_strand_destroy drops the refcount and frees the strand. */
    if (urbi_strand_arm_init(s) != 0) {
        urbi_strand_destroy(vm, s);
        return NULL;
    }

    /* Wire frame-0 execution state from the root chunk.
     * Mirrors uvm_run.c lines 102-129 (without the transient-specific fields
     * is_transient_strand and out_slot, which callers set if needed).
     * v0.9.2: s->root_proto IS the root — no intermediate module. */
    s->R          = s->stack;
    s->pc         = s->root_proto->instructions;
    s->pc_base    = s->root_proto->instructions;
    s->cur_consts = s->root_proto->constants;
    s->frame_count  = 0;
    s->open_upvals  = NULL;
    s->out_slot     = NULL;  /* caller may set before first urbi_step */

    /* Create a UChunkInstance for IC wiring (per-(vm, root) cache tier).
     * urbi_chunk_instance_create always allocates fresh — safe here because
     * this is a new strand owning its own IC entry. */
    s->module_instance = urbi_chunk_instance_create(vm, root);
    if (!s->module_instance) {
        urbi_strand_destroy(vm, s);
        return NULL;
    }

    /* Transition DORMANT → READY so the host's urbi_step loop picks it up.
     * urbi_strand_start calls sched_strand_make_runnable internally. */
    urbi_strand_start(vm, s);

    return s;
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
 * Callers that need s->root_proto set (e.g. fork_spawn_child) must do so
 * explicitly after this call returns 0. */
int
urbi_strand_arm_from_closure(UStrand *s, struct UClosure *entry)
{
    if (urbi_strand_arm_init(s) != 0) return -1;

    s->pc         = entry->proto->instructions;
    s->pc_base    = entry->proto->instructions;
    /* CHSTR-019: unconditionally adopt the new proto's constant pool (which
     * may itself be NULL).  The earlier conditional preserve-on-NULL clobbered
     * cur_consts only when entry->proto->constants was non-NULL, leaving a
     * stale pointer from a prior arm if the strand was recycled via the legal
     * free → arm sequence (CHSTR-005).  Always reset; callers that need a
     * non-NULL pool must supply one in the closure. */
    s->cur_consts = entry->proto->constants;
    s->frame_count  = 0;
    s->open_upvals  = NULL;
    s->out_slot     = NULL;
    return 0;
}

/* === W3a (v0.10.9): SUSPENDED ↔ READY transitions ===
 *
 * Tag.block / Tag.freeze cross-strand suspension primitives.  The header
 * comment in ustrand.h documents the contract; this is the implementation. */

void
urbi_strand_suspend(struct UStrand *strand, uint8_t reason, struct UTag *tag)
{
    if (strand == NULL || tag == NULL) return;
    URBI_ASSERT_NOT_ISR(strand->vm);

    /* Only READY and RUNNING strands transition cleanly into SUSPENDED.
     *
     * READY: splice out of the cooperative scheduler's ready queue so the
     *   dispatch loop doesn't pick the strand up.  sched_strand_unbind_from_
     *   ready_queue idempotently decrements strand_runnable_count when the
     *   strand was actually on the queue.
     *
     * RUNNING: the strand is currently dispatching (called urbi_strand_suspend
     *   indirectly from within a host callback, or set up for valued-block).
     *   Not on the queue; just stamp the state.  The dispatch loop's run-step
     *   sees the SUSPENDED state byte on the next NEXT() and exits at the
     *   appropriate yield boundary.  At v0.10.9 the only callers are
     *   urbi_tag_block / urbi_tag_freeze, both invoked from outside dispatch
     *   (from a host callback) so the strand here is typically READY; the
     *   RUNNING arm is forward-defensive for in-dispatch use.
     *
     * Any other state (DORMANT, WAITING, SUSPENDED, DEAD): silent no-op.
     * Suspending a WAITING strand would corrupt sleep/event waiter chains;
     * suspending a DEAD strand would un-reap it.  Callers that legitimately
     * want to cancel a wait must transition through READY first via the
     * appropriate unblock path. */
    const uint8_t cur_state = strand->state & USTRAND_STATE_MASK;
    if (cur_state != USTRAND_READY && cur_state != USTRAND_RUNNING) return;

    if (cur_state == USTRAND_READY) {
        sched_strand_unbind_from_ready_queue(strand);
    }

    strand->state = (uint8_t)(USTRAND_SUSPENDED |
                              (reason & USTRAND_REASON_MASK));
    strand->wait_payload.suspend_tag = tag;
}

void
urbi_strand_resume(struct UStrand *strand, UValue resume_value)
{
    if (strand == NULL) return;
    URBI_ASSERT_NOT_ISR(strand->vm);

    if (!USTRAND_IS_SUSPENDED(strand)) return;
    URBI_TP(strand->vm, URBI_TRACE_SCHED, URBI_LOG_DEBUG, URBI_TP_SCHED_RESUME,
            (uint32_t)(uintptr_t)strand, 0);

    /* Write resume_value to unblock_value so a future opcode-level handoff
     * (W3f, deferred at v0.10.9) can deliver it into the strand's result
     * register.  At v0.10.9 the value is stored but not delivered. */
    strand->unblock_value = resume_value;
    strand->wait_payload.suspend_tag = NULL;

    /* SUSPENDED → READY via sched_strand_make_runnable so queue accounting
     * (ready_head/tail + strand_runnable_count) stays consistent.  The
     * helper sets state = USTRAND_STATE_READY itself. */
    sched_strand_make_runnable(strand);
}
