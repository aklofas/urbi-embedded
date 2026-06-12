/* SPDX-License-Identifier: BSD-3-Clause */
/* UStrand state byte encoding + lifecycle declarations. */

/* === Strand walker contract (REALM-026) ===
 *
 * URealm.strands_head MUST contain every live strand whose register window
 * may hold GC-managed UValues.  Scheduler implementations are responsible
 * for maintaining this invariant — the GC walker visits every strand on
 * this list (with the DEAD-state filter applied inside strand_walk_roots).
 * This decouples GC correctness from any single scheduler's internal
 * queues (cooperative ready/sleep, future priority bands, mutex/event
 * wait queues, ...).
 *
 * The list is threaded via UStrand.next_in_realm; strands are head-inserted
 * at urbi_strand_create and unlinked at urbi_realm_destroy.
 * See docs/internals/scheduler-design.md for the full contract. */

#ifndef USTRAND_H
#define USTRAND_H

#include <stdint.h>
#include <stddef.h>   /* size_t */
#include "value/uvalue.h"   /* pulls in umodule.h which defines UValue — must come before uframe.h */
#include "runtime/uframe.h"   /* UCallFrame, UUpvalCell, UVM_MAX_FRAMES, UVM_STACK_CAP */
#include "runtime/umacros.h"  /* URBI_INTERNAL_ASSERT (ustrand_c_root_pop LIFO check) */

#ifdef __cplusplus
extern "C" {
#endif

/* === State byte encoding (row 9 §3.1) ===
   Upper nibble: logical state class.
   Lower nibble: reason sub-code (meaningful only in WAITING). */

#define USTRAND_STATE_MASK     0xF0U
#define USTRAND_REASON_MASK    0x0FU

/* Logical state classes (pre-shifted into upper nibble). */
#define USTRAND_DORMANT        0x00U  /* allocated, not yet enqueued */
#define USTRAND_READY          0x10U  /* on run-queue */
#define USTRAND_RUNNING        0x20U  /* currently dispatching */
#define USTRAND_WAITING        0x30U  /* blocked, see reason sub-code */
#define USTRAND_DEAD           0x40U  /* terminated, awaiting GC */
#define USTRAND_SUSPENDED      0x50U  /* RESERVED — Tag.freeze (M5/M6) */

/* WAITING reason sub-codes (lower nibble).
 *
 * Each reason has a distinct value (CHSTR-016, v0.5.5).  Earlier baselines
 * reused 0x02 for both EVENT and WATCHER and disambiguated by call-site
 * context; the WAIT_EVENT composite was documented as 0x33 (= 0x30 | 0x03)
 * even though REASON_EVENT was 0x02 — a real comment-vs-macro divergence
 * (CHSTR-017).  Renumbering pushes EVENT to 0x03 / JOIN to 0x04 / HOST to
 * 0x05 so every composite is reconstructible from its constituents.
 *
 * The state byte is RUNTIME-ONLY (never serialized to bytecode), so the
 * numeric values are not part of any external contract. */
#define USTRAND_REASON_NONE    0x00U
#define USTRAND_REASON_SLEEP   0x01U
#define USTRAND_REASON_WATCHER 0x02U
#define USTRAND_REASON_EVENT   0x03U
#define USTRAND_REASON_JOIN    0x04U
#define USTRAND_REASON_HOST    0x05U  /* RESERVED v1.x/v2 */
#define USTRAND_REASON_BLOCK   0x06U  /* SUSPENDED via tag.block / urbi_tag_block (W3a) */
#define USTRAND_REASON_FREEZE  0x07U  /* SUSPENDED via tag.freeze / urbi_tag_freeze (W3a) */

/* === SCHED-08 (v0.13.3): per-strand suspension gates ===
 *
 * block and freeze are INDEPENDENT gates (workspace ledger §S6): each sets
 * its own bit at suspend time and clears it at unblock/unfreeze; the strand
 * resumes only when BOTH bits are clear (strand_resume_if_ungated).  The
 * reason sub-code on a SUSPENDED strand is the gate-priority decode
 * (ustrand_gates_reason below) — pre-fix, the single shared reason nibble
 * let unblock-order games resume a still-gated strand (block -> freeze ->
 * unblock RAN; the symmetric order too). */
#define USTRAND_GATE_BLOCK   0x01U   /* set by urbi_tag_block,  cleared by unblock */
#define USTRAND_GATE_FREEZE  0x02U   /* set by urbi_tag_freeze, cleared by unfreeze */

/* Reason-nibble decode for a NON-ZERO gate set: BLOCK outranks FREEZE.
 * Single source of truth for every SUSPENDED state stamp (urbi_strand_suspend,
 * strand_resume_if_ungated's still-gated re-stamp, and the gated-wake arm in
 * sched_strand_make_runnable). */
static inline uint8_t
ustrand_gates_reason(uint8_t gates)
{
    return (gates & USTRAND_GATE_BLOCK) ? USTRAND_REASON_BLOCK
                                        : USTRAND_REASON_FREEZE;
}

/* Composite values stored in strand->state. */
#define USTRAND_STATE_DORMANT         (USTRAND_DORMANT)
#define USTRAND_STATE_READY           (USTRAND_READY)
#define USTRAND_STATE_RUNNING         (USTRAND_RUNNING)
#define USTRAND_STATE_DEAD            (USTRAND_DEAD)
#define USTRAND_STATE_SUSPENDED       (USTRAND_SUSPENDED)
#define USTRAND_STATE_WAITING_SLEEP   (USTRAND_WAITING | USTRAND_REASON_SLEEP)
#define USTRAND_STATE_WAITING_EVENT   (USTRAND_WAITING | USTRAND_REASON_EVENT)
#define USTRAND_STATE_WAITING_JOIN    (USTRAND_WAITING | USTRAND_REASON_JOIN)
#define USTRAND_STATE_WAITING_HOST    (USTRAND_WAITING | USTRAND_REASON_HOST)

/* W3a: SUSPENDED composite states — distinguish block vs freeze via the
 * reason sub-code so tag.unblock can target only BLOCK-suspended strands
 * (and likewise tag.unfreeze for FREEZE).  block and freeze are
 * independent gates per workspace ledger §S6. */
#define USTRAND_STATE_SUSPENDED_BLOCK   (USTRAND_SUSPENDED | USTRAND_REASON_BLOCK)
#define USTRAND_STATE_SUSPENDED_FREEZE  (USTRAND_SUSPENDED | USTRAND_REASON_FREEZE)

/* spec #2 §7.7 — waituntil(cond) strand parked awaiting edge fire.
   0x32 = USTRAND_WAITING (0x30) | USTRAND_REASON_WATCHER (0x02). */
#define USTRAND_WAIT_WATCHER          (USTRAND_WAITING | USTRAND_REASON_WATCHER)

/* spec #3 §3.3 — waituntil(e?) strand parked awaiting Event emit.
   0x33 = USTRAND_WAITING (0x30) | USTRAND_REASON_EVENT (0x03). */
#define USTRAND_WAIT_EVENT            (USTRAND_WAITING | USTRAND_REASON_EVENT)

/* Helper macros — take a pointer to UStrand. */
#define USTRAND_IS_WAITING(s)  (((s)->state & USTRAND_STATE_MASK) == USTRAND_WAITING)
#define USTRAND_IS_SUSPENDED(s) (((s)->state & USTRAND_STATE_MASK) == USTRAND_SUSPENDED)
#define USTRAND_GET_STATE(s)   ((s)->state & USTRAND_STATE_MASK)
#define USTRAND_GET_REASON(s)  ((s)->state & USTRAND_REASON_MASK)

/* === UExecStatus moved to <urbi/types.h> at v0.5.5 (T17) === */
#include "urbi/types.h"

/* === Cleanup-stack type (T3) ===
   ucleanup.h defines UCleanupEntry and the stack init/destroy ops. */

#include "runtime/ucleanup.h"

/* === Forward declarations for types that land in later tasks. === */

struct UTag;             /* T29 */
struct UEvent;           /* reactive runtime */
struct UVM;              /* uvm.h — forward-decl to avoid circular include */
struct URealm;           /* urealm.h — forward-decl for strand lifecycle context */
struct UProto;           /* uproto.h — forward-decl for root_proto (v0.8.1+) */
struct UClosure;         /* umodule.h — forward-decl for closure list threading */
struct UChunkInstance;  /* object/uchunk_instance.h — M4 follow-up: per-(vm,module) IC tier */
struct UWatcher;         /* watcher/uwatcher.h — spec #1 §4.2 back-pointer */
struct UPeriodic;        /* stdlib/temporal.h — v0.9.4 every() back-pointer */

/* refactor-3 VM-06a / v0.13.1-L: C-stack root frame.  Runtime C code that
 * must hold a UValue live across a nested dispatch (which can run GC
 * slices) pushes one of these stack-allocated frames onto
 * s->c_roots_head; strand_walk_roots yields every chained slot.  LIFO
 * discipline is mandatory (assert-checked in pop) and every push must be
 * popped on every exit path: a frame leaked past its holding function
 * leaves the chain pointing at a dead C stack frame, and the next mark
 * phase reads it — silent corruption, not a crash.  Sound because the
 * holder (currently only run_cleanup_with_replace) cannot yield mid-body
 * — the C frame outlives every GC slice that can observe the chain. */
typedef struct UCRootFrame {
    UValue              *slot;
    struct UCRootFrame  *next;
} UCRootFrame;

/* === UStrand struct ===
   The strand is the unit of cooperative concurrency.  Each instance owns
   its own register stack, frame array, cleanup stack, and scheduler-list
   threading; the lifecycle helpers below (ustrand_init, ustrand_destroy,
   urbi_strand_arm_init, urbi_strand_arm_from_closure, etc.) are the live
   contract.  The execution-state fields at the bottom of the struct hold
   per-strand frame/PC/upvalue state and are valid while the strand is
   RUNNING or READY (paused mid-run). */

typedef struct UStrand UStrand;
struct UStrand {
    /* Field groups below: VM/realm context -> unwind/cleanup state ->
       state byte + budget -> scheduler list pointers -> wait-state ->
       watcher/event/join links -> realm strand list -> execution state. */

    /* --- VM back-pointer (T5; set by ustrand_init; required by scheduler ops) --- */
    struct UVM             *vm;

    /* --- T20 lifecycle fields: owning Realm + entry closure --- */
    struct URealm          *realm;           /* owning Realm; NULL for internal/stack strands */
    struct UClosure        *entry_closure;   /* closure to invoke at strand activation;
                                               zero-init; frame-0 setup deferred to urbi_step
                                               or a future urbi_strand_arm helper */

    /* --- Row 7 unwind/cleanup fields (T9 wires walker; T3 lands cleanup-stack) --- */
    UExecStatus             pending_unwind;
    uint8_t                 unwind_pad[3];
    UValue                  unwind_value;
    UValue                  catch_value;       /* T10: last caught exception value;
                                                  written by unwind walker on catch absorption;
                                                  read by OP_LOAD_CATCH_VALUE at handler entry */
    struct UTag            *unwind_target;
    void                   *suppressed_head;   /* RESERVED v1.x */
    struct UCleanupEntry   *cleanup_top;
    uint16_t                cleanup_depth;
    uint16_t                cleanup_cap;
    /* T29 / FOUND-009: recursion bound for run_cleanup_with_replace().
     * Distinct from cleanup_depth (cleanup-stack push/pop counter); this
     * tracks how deeply run_cleanup_with_replace has re-entered
     * dispatch_loop_until_yield via finally/onleave handlers.  Without
     * this guard, a misbehaving cleanup body that itself triggers a new
     * unwind could push past URBI_CLEANUP_MAX levels of recursion and
     * exhaust the C stack.  Lives in the natural alignment gap between
     * the two cleanup_* uint16_t fields and the cleanup_base pointer,
     * keeping UStrand size stable (CHSTR-041 layout pin holds). */
    uint16_t                cleanup_run_depth;
    /* v0.13.3 (design-risks v0.13.1-B): set by the unwind walker when a
     * cleanup body's replacement unwind is ABSORBED at an OUTER handler
     * while cleanup_run_depth > 0 (catch / tag-stop absorption arms).
     * run_cleanup_with_replace consumes it to recognise a post-absorption
     * yield/park as legitimate control flow rather than a mid-cleanup
     * truncation (pre-fix: spurious CLEANUP_OVERFLOW fatal).  Cleared at
     * every run_cleanup_with_replace ENTRY (each cleanup run starts
     * un-absorbed — a stale flag from a body that absorbed internally and
     * then completed normally must not mask a later genuine truncation;
     * spec-review hazard 2, pinned by
     * tests/chk/exceptions/finally_truncation_after_absorb.chk), at
     * consume, and at urbi_strand_reset.  Known limitations (both degrade
     * to a CLEANUP_OVERFLOW fatal, never to a masked truncation): with
     * NESTED cleanup runs (cleanup_run_depth > 1) the innermost consumer
     * clears the flag, so an enclosing run still misreads the park and
     * escalates — same behaviour as pre-fix; and an absorbed handler that
     * runs a COMPLETE nested cleanup and only then parks escalates too
     * (the nested run's entry-clear consumed the flag).  Single-level
     * absorption (the v0.13.1-B repro) is fully handled.  Carved from the
     * former uint16_t cleanup_run_pad — no UStrand size/offset change
     * (CHSTR-041 holds). */
    uint8_t                 cleanup_absorbed;
    uint8_t                 cleanup_run_pad;
    struct UCleanupEntry   *cleanup_base;
    /* refactor-3 VM-06a / v0.13.1-L: head of the C-stack root frame chain
     * (UCRootFrame above).  NULL when no runtime C code is pinning a value
     * across a nested dispatch; all strand constructions zero-init the
     * struct (ustrand_init via urbi_zero; the urbi_vm_run / scratch
     * transients via urbi_zero on the stack-local), so the chain starts
     * empty by construction. */
    struct UCRootFrame     *c_roots_head;
    UExecStatus             fatal_status;
    uint8_t                 fatal_pad[3];
    UValue                  fatal_value;

    /* --- Row 9 state machine + budget --- */
    uint8_t                 state;
    uint8_t                 cross_strand_stop_pending;  /* T31: set when urbi_tag_stop deposits
                                                           cross-strand; cleared + counter-
                                                           decremented at ustrand_destroy.
                                                           Once-per-lifetime: repeated deposits on
                                                           a strand that already processed its TAG_STOP
                                                           do not re-increment (counter tracks lifetime
                                                           cross-strand-affected strands). */
    uint8_t                 is_transient_strand;       /* Set on stack-local transient strands
                                                           (synthesized for in-process eval such as
                                                           urbi_vm_run and urbi_run_closure_on_scratch)
                                                           so OP_FORK_DETACH / OP_FORK_JOIN reject
                                                           forks even though realm now points at
                                                           vm->global_realm.  Always 0 for
                                                           urbi_strand_create-managed strands
                                                           (heap-allocated).  See pre-M4 GC
                                                           strand-walker §5.1. */
    uint8_t                 cleanup_body_done;          /* refactor-3 VM-02: set by OP_RESUME —
                                                           run_cleanup_with_replace's completion
                                                           marker (yield/budget exits leave it 0).
                                                           Absorbs the former state_pad[1] byte so
                                                           the CHSTR-041 size pin holds. */
    uint16_t                instruction_budget_remaining;
    /* SCHED-08 (v0.13.3): independent block/freeze suspension gates —
     * USTRAND_GATE_BLOCK / USTRAND_GATE_FREEZE bits (see the macro block
     * above for the resume contract).  Set by urbi_strand_suspend (any
     * live state, including gate-and-leave-parked on WAITING); cleared by
     * urbi_tag_unblock / urbi_tag_unfreeze (per-mode) and by the tag-stop /
     * cancel override (both bits — stop wins over suspension).
     *
     * KNOWN LIMITATION: two different tags blocking/freezing the SAME
     * strand share these per-strand bits (no per-tag refcount) — one tag's
     * unblock can resume a strand another tag still wants blocked.  Filed
     * as a design-risk at the v0.13.3 close-out (v1.x candidate: per-tag
     * gate refcount).
     *
     * Carved from the former uint16_t budget_pad — no UStrand size/offset
     * change (CHSTR-041 pin holds). */
    uint8_t                 suspend_gates;
    uint8_t                 budget_pad;

    /* --- Cooperative scheduler intrusive list (T5 wires) ---
       Included unconditionally at T2; T5 will revisit whether to gate
       these fields behind URBI_SCHED == URBI_SCHED_COOPERATIVE if the
       scheduler dispatch becomes plural or multi-strategy.
       TODO(T5): conditionally compile under URBI_SCHED == URBI_SCHED_COOPERATIVE
       if scheduler dispatch becomes plural. */
    UStrand                *ready_next;
    UStrand                *ready_prev;

    /* --- WAITING-related queue fields ---
     *
     * wait_payload (CHSTR-025): anonymous union discriminated by the strand's
     * USTRAND_GET_REASON(s) byte (lower nibble of s->state).  Each WAITING
     * sub-state owns exactly one arm; reading any other arm is undefined
     * behaviour because storing into one union member ends the lifetime of
     * the others (C11 6.2.6.1 §7).
     *
     *   USTRAND_REASON_SLEEP  (0x01) -> wait_payload.wake_us       (sleep queue)
     *   USTRAND_REASON_EVENT  (0x03) -> wait_payload.event         (event-wait)
     *   USTRAND_REASON_JOIN   (0x04) -> wait_payload.join_parent   (join-wait)
     *   USTRAND_REASON_BLOCK  (0x06) -> wait_payload.suspend_tag   (tag.block)  (W3a)
     *   USTRAND_REASON_FREEZE (0x07) -> wait_payload.suspend_tag   (tag.freeze) (W3a)
     *
     * USTRAND_REASON_WATCHER (0x02) does NOT use wait_payload (the strand
     * parks via UWatcher's own waiters list, not the union).  Read-site
     * contract: switch on USTRAND_GET_REASON(s) before touching an arm. */
    UStrand                *wait_next;
    union {
        uint64_t            wake_us;       /* USTRAND_REASON_SLEEP */
        struct UEvent      *event;         /* USTRAND_REASON_EVENT */
        UStrand            *join_parent;   /* USTRAND_REASON_JOIN: child we are waiting on */
        struct UTag        *suspend_tag;   /* USTRAND_REASON_BLOCK / _FREEZE (W3a) */
    } wait_payload;

    /* --- Watcher body ownership (spec #1 §4.2) ---
     * Non-NULL iff this strand was spawned as a watcher body strand.
     * The scheduler's strand-completion path calls urbi_watcher_body_completed
     * with O(1) lookup via this back-pointer. NULL for all other strands. */
    struct UWatcher        *watcher_body_owner;

    /* --- Periodic body ownership (v0.9.4 every() spec §11.4) ---
     * Non-NULL iff this strand was spawned as a UPeriodic body strand.
     * uvm.c::exit_strand calls urbi_periodic_body_completed when this
     * field is non-NULL to clear the back-pointer and either re-arm the
     * periodic or mark it for unregister.  Mirrors watcher_body_owner. */
    struct UPeriodic       *periodic_owner;

    /* --- Event-waiter fields (spec #3 §3.3) ---
     * Populated when strand is in USTRAND_WAIT_EVENT state.
     * next_event_waiter: intrusive singly-linked waiters_head chain on UEvent.
     * wait_event_target: back-pointer to the UEvent being waited on (for unregister).
     * last_event_payload: written by UEvent emit before unblocking; read by waituntil(). */
    struct UStrand         *next_event_waiter;
    struct UEvent          *wait_event_target;
    UValue                  last_event_payload;

    /* --- Suspend resume-value (W3a; workspace ledger §S5 valued-block) ---
     * Written by urbi_tag_block (and the C API) when SUSPENDED via REASON_BLOCK
     * so unblock can deliver the value back to the strand.  Cleared on resume.
     * Distinct from unwind_value (which is for stop/cancel/throw unwinding).
     * Mirroring last_event_payload's role for event-wait. */
    UValue                  unblock_value;

    /* --- Join-blocker list (OP_FORK_JOIN / OP_JOIN_WAIT) ---
     * Singly-linked list of strands that are JOIN-blocked on THIS strand.
     * Threaded via each joiner's wait_next field.
     * fork_wake_joiners() walks this list when the strand reaches DEAD. */
    UStrand                *joiners_head;

    /* --- Realm ownership list (T38) ---
     * Singly-linked list of all strands created under the same URealm.
     * Populated by urbi_strand_create; walked by urbi_realm_destroy to free
     * all realm-managed strands when the realm is torn down.
     * NULL for strands not created via urbi_strand_create (e.g. urbi_vm_run transient). */
    UStrand                *next_in_realm;

    /* --- M2-baseline execution state migrated from urbi_vm_run-locals + UVM at T6 ---
       These fields are valid only while the strand is RUNNING or READY (paused mid-run).
       urbi_vm_run's thin adapter initialises them before calling dispatch_loop_until_yield
       and tears them down after the strand transitions to DEAD.
       T20 will move strand creation here when the full Strand C API lands. */
    UValue                 *stack;          /* heap-alloc'd register array; UVM_STACK_CAP slots */
    UValue                 *R;              /* current frame register base (derived from stack) */
    const uint32_t         *pc;             /* current instruction pointer */
    const uint32_t         *pc_base;        /* base of current frame's instruction array */
    const UValue           *cur_consts;     /* current frame's constant pool */
    struct UProto          *root_proto;     /* root UProto of the chunk being executed.
                                             * Replaces the deleted s->module field (v0.9.2 Task 4.1).
                                             * Set at strand creation (strand_create_for_module) and
                                             * cleared in ustrand_destroy after refcount dec.
                                             * NULL for closure-based strands (set by arm_from_closure
                                             * callers via uproto_root_of on the closure's proto). */
    /* module_instance: per-(vm, module) IC RAM tier (M4 follow-up).
     * Bound by urbi_vm_run / urbi_run_chunk via
     * urbi_get_or_create_chunk_instance.  May be NULL if not yet wired
     * (defensive).
     *
     * CHSTR-043: GC-managed, NOT freed by ustrand_destroy.  The
     * UChunkInstance is shared across strands within a realm and has its
     * own GC lifecycle — vm->module_instances_head heads the live list and
     * walk_umoduleinstance (src/object/utypes_init.c) traces each instance
     * during the realm's strand walker.  ustrand_destroy clears the strand
     * via urbi_zero / release_strand_resource_chain for hygiene only; the
     * pointer is not free'd here. */
    struct UChunkInstance *module_instance;
    UCallFrame              frames[UVM_MAX_FRAMES];
    int                     frame_count;
    UUpvalCell             *open_upvals;    /* open upvalue cells still pointing into stack */
    /* closure_list + closed_cells deleted at v0.8.4 Step C-3: UClosure and
     * UUpvalCell are GC-managed; no per-strand free-lists needed. */
    UValue                 *out_slot;       /* adapter-set: OP_RET at top-frame writes here */
};

/* Layout pin (Wave-1 v0.5.3 audit CHSTR-041): the bulk of UStrand's size
 * is the embedded frames[UVM_MAX_FRAMES] call-frame array (64 × ~56 B);
 * any change to that or the surrounding fields must update this assert
 * deliberately.  Default + footprint presets share this size — preset
 * tunables change runtime budgets, not struct layout.
 * Guarded on pointer width to avoid a hard failure on 32-bit cross
 * targets, matching the UEvent / UObject pattern. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
URBI_STATIC_ASSERT(sizeof(struct UStrand) == 3920,
               "UStrand size pin (CHSTR-041) on 64-bit — update deliberately when UCallFrame or surrounding fields change"
               /* v0.9.2 Task 4.1: -8 B from deleting s->module pointer (3896 → 3888).
                * v0.9.4: +8 B for periodic_owner back-pointer (3888 → 3896).
                * v0.10.9 W3a: +16 B for unblock_value (UValue) supporting
                *              SUSPENDED↔READY tag.block/unblock plumbing (3896 → 3912).
                * v0.13.2 (refactor-3 VM-06a): +8 B for c_roots_head — the
                *              C-stack root frame chain (3912 → 3920). */);
#endif

/* === C-stack root frame push/pop (refactor-3 VM-06a) ===
 *
 * Strict LIFO: every push must be matched by a pop of the SAME frame on
 * every exit path of the holding function.  The frame and the rooted
 * UValue must both be C-stack locals that outlive the nested dispatch. */

static inline void
ustrand_c_root_push(struct UStrand *s, UCRootFrame *f, UValue *slot)
{
    URBI_INTERNAL_ASSERT(slot != NULL);
    URBI_INTERNAL_ASSERT(s->c_roots_head != f);   /* double-push cycle guard */
    f->slot = slot;
    f->next = s->c_roots_head;
    s->c_roots_head = f;
}

static inline void
ustrand_c_root_pop(struct UStrand *s, UCRootFrame *f)
{
    URBI_INTERNAL_ASSERT(s->c_roots_head == f);
    s->c_roots_head = f->next;
}

/* === Lifecycle functions ===

   ustrand_init zeros the strand, sets DORMANT state, and pre-allocates the
   cleanup stack using vm->alloc_fn.  On allocation failure the strand is
   left in a detectable malformed-DORMANT state (cleanup_base == NULL);
   the return value distinguishes success (0) from OOM (-1) and callers
   must check it.

   ustrand_destroy walks the cleanup stack to unregister the strand from
   any tag.member_strands_head lists (strand_unlink_from_tags), routes
   cross-strand stop bookkeeping through sched_strand_account_destroy
   (which decrements vm->host_call_pending_count if a cross-strand stop was
   deposited on this strand), frees the cleanup stack and the register
   stack via vm->alloc_fn, and releases per-strand resource chains
   (closures, closed upvalues, and out-slot writes) via
   release_strand_resource_chain.  The module_instance pointer is GC-managed
   and is NOT freed here (see CHSTR-043 docstring).  The same vm pointer
   used for init must be passed to destroy. */

/* CHSTR-010: returns 0 on success, -1 if the cleanup-stack allocation fails.
 * Existing callers that discard the return value are valid C; urbi_strand_create
 * checks it.  int is used rather than URBIError to avoid pulling urbi/urbi.h
 * into ustrand.h's include chain (circular dependency risk). */
int  ustrand_init(UStrand *s, struct UVM *vm);
void ustrand_destroy(UStrand *s, struct UVM *vm);

/* === W3a (v0.10.9) / SCHED-08 (v0.13.3): SUSPENDED ↔ READY transitions ===
 *
 * urbi_strand_suspend: set the suspension gate matching the supplied reason
 *   sub-code (USTRAND_REASON_BLOCK or USTRAND_REASON_FREEZE) and record the
 *   owning UTag.  Per entry state:
 *     READY/RUNNING — suspend in place (queue splice / runnable_dec per the
 *       SCHED-01 single-writer scheme), stamp SUSPENDED with the gate-
 *       priority reason decode.
 *     WAITING — gate-and-leave-parked (SCHED-08): the gate bit is set but
 *       the strand stays on its sleep/event/join/watcher park and stays in
 *       strand_waiting_count; the make_runnable wake funnel routes the
 *       eventual wake to SUSPENDED instead of READY.
 *     SUSPENDED — stack the new gate; re-stamp reason; refresh the tag
 *       back-pointer.
 *     DORMANT/DEAD — silent no-op.
 *   Not ISR-safe.
 *
 *   Internal contract — not part of the public C API.  The public surface is
 *   urbi_tag_block / urbi_tag_unblock / urbi_tag_freeze / urbi_tag_unfreeze
 *   in include/urbi/urbi.h which walk tag->member_strands_head, manage the
 *   gate bits, and call this helper / strand_resume_if_ungated per member.
 *
 * strand_resume_if_ungated: resume a SUSPENDED strand iff BOTH gates are
 *   clear (the caller — unblock/unfreeze — clears its own gate bit first).
 *   While still gated, re-stamps the reason nibble to the remaining-gate
 *   decode and stays SUSPENDED.  The resume value is strand->unblock_value
 *   (stamped by urbi_tag_block; nil otherwise) and stays staged there for
 *   the deferred opcode-level handoff (W3f, v0.10.9-C).  No-op if the strand
 *   is not SUSPENDED (e.g. a gate cleared while the strand is still parked
 *   WAITING).  Not ISR-safe.  Replaces the pre-SCHED-08 urbi_strand_resume
 *   (which resumed unconditionally off the shared reason nibble).
 *
 * Both functions keep the cooperative scheduler's bookkeeping correct:
 *   - Suspending a READY strand splices it out of vm->ready_head and
 *     decrements vm->strand_runnable_count (via the unbind helper).
 *   - Suspending a RUNNING strand decrements vm->strand_runnable_count via
 *     sched_runnable_dec (refactor-3 SCHED-01: a RUNNING strand is in the
 *     counted set — count == |READY| + |RUNNING non-transient| — and
 *     RUNNING -> SUSPENDED leaves it).
 *   - Resuming a SUSPENDED strand routes through sched_strand_make_runnable
 *     (re-enters the counted set as READY; the funnel owns suspended--). */
void urbi_strand_suspend(struct UStrand *strand, uint8_t reason,
                         struct UTag    *tag);
void strand_resume_if_ungated(struct UStrand *strand);

/* === T29: ambient-tag inheritance helpers ===
 *
 * urbi_strand_capture_ambient_chain: walk `parent`'s cleanup-stack bottom-up
 *   and collect the owning_tag pointer from every TAG_SCOPE entry into
 *   out_chain[].  Returns the count of tags collected.  Returns SIZE_MAX if
 *   the chain would exceed out_cap (caller bug — use URBI_CLEANUP_MAX as cap).
 *   Read-only; ISR-safe.
 *
 * urbi_strand_attach_ambient_tags: push synthetic TAG_SCOPE cleanup-entries
 *   onto `new_s` for each tag in `chain[0..chain_count-1]`.  chain[0] is the
 *   bottommost tag (pushed first).  On cleanup-stack overflow, sets
 *   new_s->fatal_status = UEXEC_CANCEL, fatal_value = NIL, state = DEAD and
 *   returns immediately.  Not ISR-safe. */

size_t urbi_strand_capture_ambient_chain(struct UStrand *parent,
                                         struct UTag   **out_chain,
                                         size_t          out_cap);

void   urbi_strand_attach_ambient_tags(struct UStrand *new_s,
                                       struct UTag   **chain,
                                       size_t          chain_count);

/* v0.10.10 / D7-C: single-entry tag-list unlink, exposed for the disown
 * helper so it can strip individual TAG_SCOPE entries from a child's
 * cleanup chain without walking the whole chain. */
void strand_unlink_member_entry(struct UCleanupEntry *e);

/* === CHSTR-044: register-stack lifecycle triplet ===
 *
 * Centralises the alloc / zero / free lifecycle for the per-strand
 * UValue register stack so each lifecycle stage has a single owner.
 *
 * urbi_strand_register_stack_alloc: allocate a UVM_STACK_CAP-slot stack
 *   using vm->alloc_fn; wire s->stack and s->R.
 *   Returns 0 on success, -1 on OOM (s->stack remains NULL).
 *
 * urbi_strand_register_stack_zero: zero the allocated stack contents.
 *   Must be called after alloc and before first dispatch.
 *
 * urbi_strand_register_stack_free: free and null the stack via vm->alloc_fn.
 *   No-op when s->stack is already NULL (idempotent). */
int  urbi_strand_register_stack_alloc(struct UStrand *s, struct UVM *vm);
void urbi_strand_register_stack_zero(struct UStrand *s);
void urbi_strand_register_stack_free(struct UStrand *s, struct UVM *vm);

/* === CHSTR-022: urbi_strand_arm_init ===
 *
 * Convenience composite: calls urbi_strand_register_stack_alloc then
 * urbi_strand_register_stack_zero.  Common foundation shared by
 * urbi_strand_arm_from_closure (closure-based arming) and urbi_vm_run
 * (module-level direct arming).  Each caller wires pc/pc_base/cur_consts/
 * out_slot/state afterward.
 *
 * Returns 0 on success, -1 on allocation failure (s->stack remains NULL). */
int urbi_strand_arm_init(struct UStrand *s);

/* === spec #1 §5.5: urbi_strand_arm_from_closure ===
 *
 * Allocate a register stack for `s` and wire up the execution-state fields
 * (R, pc, pc_base, cur_consts, frame_count, open_upvals, out_slot) from
 * `entry` and the strand's own vm->alloc_fn.
 *
 * Called by fork_spawn_child (T38) and the watcher body-spawn path (T24)
 * so the stack-alloc + pc-arming sequence is not duplicated.
 *
 * Returns 0 on success, -1 on allocation failure (s is left unarmed; caller
 * is responsible for tearing down s).
 *
 * Precondition (CHSTR-005): s->stack must be NULL on entry.  Re-arming a
 * strand that already owns a register stack would leak the prior allocation
 * because urbi_strand_register_stack_alloc unconditionally overwrites s->stack.
 * Asserted in -DURBI_DEBUG builds via URBI_INTERNAL_ASSERT inside the inner
 * urbi_strand_arm_init helper.
 *
 * NOTE: does NOT set s->root_proto — callers that need root_proto for
 * diagnostics or nested-proto lookup must set it explicitly after this call returns 0.
 *
 * NOTE (CHSTR-014, CHSTR-037 / T102 + T105): does NOT set s->module_instance
 * either.  The M4-follow-up per-(vm, module) IC RAM tier requires each spawn
 * site to wire module_instance differently:
 *   - fork_spawn_child inherits parent's s->module_instance (siblings share
 *     modules);
 *   - the watcher body-spawn path pointer-range-searches vm->module_instances_head
 *     to find the closure's owning UChunkInstance;
 *   - the scratch-frame path synthesizes a one-entry UProtoInstanceArr shell.
 * Without a post-arm assignment, OP_GETSLOT / OP_SETSLOT at frame_count == 0
 * would dereference NULL via s->module_instance->proto_instances. */
int urbi_strand_arm_from_closure(struct UStrand *s, struct UClosure *entry);

/* === v0.8.0: urbi_strand_create_for_module ===
 *
 * Allocates a non-transient scheduler-managed strand bound to a root UProto.
 * Bumps root->refcount.  Arms register stack (via urbi_strand_arm_init)
 * and wires instruction pointers, constant pool, root_proto, and
 * UChunkInstance.  Transitions DORMANT → READY via urbi_strand_start so the
 * host's main urbi_step loop picks it up.
 *
 * v0.9.2 Task 4.1: takes UProto* (root proto) instead of UModule*.
 *
 * Strand lifecycle: persists in realm->strands_head until it reaches DEAD
 * naturally (OP_RET / fatal); the host's urbi_step loop drives it.
 * urbi_strand_destroy unbinds the root proto (drops refcount) and frees
 * backing storage.
 *
 * Returns the READY strand on success, NULL on OOM (any allocation failure
 * during setup tears down the partially-armed strand before returning NULL).
 *
 * Preconditions: root->instr_count > 0; realm non-NULL (pass
 * urbi_realm_global(vm) if in doubt); vm non-NULL. */
struct UStrand *urbi_strand_create_for_module(struct UVM *vm,
                                              struct URealm *realm,
                                              struct UProto *root);

#ifdef __cplusplus
}
#endif

#endif /* USTRAND_H */
