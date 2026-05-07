/* SPDX-License-Identifier: BSD-3-Clause */
/* UStrand state byte encoding + lifecycle declarations.
   Full lifecycle operations land across T20 (create/start/spawn) and T29 (tag fields). */

#ifndef USTRAND_H
#define USTRAND_H

#include <stdint.h>
#include <stddef.h>   /* size_t */
#include "value/uvalue.h"   /* pulls in umodule.h which defines UValue — must come before uframe.h */
#include "runtime/uframe.h"   /* UCallFrame, UUpvalCell, UVM_MAX_FRAMES, UVM_STACK_CAP */

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

/* WAITING reason sub-codes (lower nibble). */
#define USTRAND_REASON_NONE    0x00U
#define USTRAND_REASON_SLEEP   0x01U
#define USTRAND_REASON_EVENT   0x02U
#define USTRAND_REASON_JOIN    0x03U
#define USTRAND_REASON_HOST    0x04U  /* RESERVED v1.x/v2 */
#define USTRAND_REASON_WATCHER 0x02U  /* same sub-code as EVENT; context disambiguates */

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

/* spec #2 §7.7 — waituntil(cond) strand parked awaiting edge fire.
   0x32 = USTRAND_WAITING (0x30) | USTRAND_REASON_WATCHER (0x02). */
#define USTRAND_WAIT_WATCHER          0x32U

/* spec #3 §3.3 — waituntil(e?) strand parked awaiting Event emit.
   0x33 = USTRAND_WAITING (0x30) | USTRAND_REASON_EVENT (0x03). */
#define USTRAND_WAIT_EVENT            0x33U

/* Helper macros — take a pointer to UStrand. */
#define USTRAND_IS_WAITING(s)  (((s)->state & USTRAND_STATE_MASK) == USTRAND_WAITING)
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
struct UModule;          /* umodule.h — forward-decl for strand execution context */
struct UClosure;         /* umodule.h — forward-decl for closure list threading */
struct UModuleInstance;  /* object/umodule_instance.h — M4 follow-up: per-(vm,module) IC tier */
struct UWatcher;         /* watcher/uwatcher.h — spec #1 §4.2 back-pointer */

/* === UStrand struct (M3 baseline) ===
   T20 and T29 add lifecycle operations; T9 wires the unwind walker;
   T3 initialises the cleanup-stack array. */

typedef struct UStrand UStrand;
struct UStrand {
    /* M2 fields for frame stack, registers, lex env, etc. are added by T20
       when the strand becomes a full execution context. */

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
    struct UCleanupEntry   *cleanup_base;
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
    uint8_t                 state_pad[1];               /* was [3], then [2] — shrunk by 1 again */
    uint16_t                instruction_budget_remaining;
    uint16_t                budget_pad;

    /* --- Cooperative scheduler intrusive list (T5 wires) ---
       Included unconditionally at T2; T5 will revisit whether to gate
       these fields behind URBI_SCHED == URBI_SCHED_COOPERATIVE if the
       scheduler dispatch becomes plural or multi-strategy.
       TODO(T5): conditionally compile under URBI_SCHED == URBI_SCHED_COOPERATIVE
       if scheduler dispatch becomes plural. */
    UStrand                *ready_next;
    UStrand                *ready_prev;

    /* --- WAITING-related queue fields --- */
    UStrand                *wait_next;
    union {
        uint64_t            wake_us;
        struct UEvent      *event;
        UStrand            *join_parent;   /* set by OP_JOIN_WAIT: child we are waiting on */
    } wait_payload;

    /* --- Watcher body ownership (spec #1 §4.2) ---
     * Non-NULL iff this strand was spawned as a watcher body strand.
     * The scheduler's strand-completion path calls urbi_watcher_body_completed
     * with O(1) lookup via this back-pointer. NULL for all other strands. */
    struct UWatcher        *watcher_body_owner;

    /* --- Event-waiter fields (spec #3 §3.3) ---
     * Populated when strand is in USTRAND_WAIT_EVENT state.
     * next_event_waiter: intrusive singly-linked waiters_head chain on UEvent.
     * wait_event_target: back-pointer to the UEvent being waited on (for unregister).
     * last_event_payload: written by UEvent emit before unblocking; read by waituntil(). */
    struct UStrand         *next_event_waiter;
    struct UEvent          *wait_event_target;
    UValue                  last_event_payload;

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
    const struct UModule   *module;         /* top-level module (diagnostics + nested protos) */
    struct UModuleInstance *module_instance; /* M4 follow-up: per-(vm,module) IC RAM tier;
                                               bound by urbi_vm_run / urbi_run_chunk via
                                               urbi_get_or_create_module_instance.  May be
                                               NULL if not yet wired (defensive). */
    UCallFrame              frames[UVM_MAX_FRAMES];
    int                     frame_count;
    UUpvalCell             *open_upvals;    /* open upvalue cells still pointing into stack */
    struct UClosure        *closure_list;   /* pre-GC: all closures allocated this run */
    UUpvalCell             *closed_cells;   /* pre-GC: all heapified upvalue cells this run */
    UValue                 *out_slot;       /* adapter-set: OP_RET at top-frame writes here */
};

/* Layout pin (Wave-1 v0.5.3 audit CHSTR-041): the bulk of UStrand's size
 * is the embedded frames[UVM_MAX_FRAMES] call-frame array (64 × ~40 B);
 * any change to that or the surrounding fields must update this assert
 * deliberately.  Default + footprint presets share this size — preset
 * tunables change runtime budgets, not struct layout.
 * Guarded on pointer width to avoid a hard failure on 32-bit cross
 * targets, matching the UEvent / UObject pattern. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(struct UStrand) == 2880,
               "UStrand size pin (CHSTR-041) on 64-bit");
#endif

/* === Lifecycle functions (stubs; full impl across T20 + T29) ===

   ustrand_init zeros the strand, sets DORMANT state, and pre-allocates the
   cleanup stack using vm->alloc_fn.  On allocation failure the strand is
   left in a detectable malformed-DORMANT state (cleanup_base == NULL);
   callers must check.

   ustrand_destroy frees the cleanup stack using vm->alloc_fn.  The same vm
   pointer used for init must be passed to destroy. */

/* CHSTR-010: returns 0 on success, -1 if the cleanup-stack allocation fails.
 * Existing callers that discard the return value are valid C; urbi_strand_create
 * checks it.  int is used rather than URBIError to avoid pulling urbi/urbi.h
 * into ustrand.h's include chain (circular dependency risk). */
int  ustrand_init(UStrand *s, struct UVM *vm);
void ustrand_destroy(UStrand *s, struct UVM *vm);

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

struct UTag;   /* forward-decl; full struct in utag.h */

size_t urbi_strand_capture_ambient_chain(struct UStrand *parent,
                                         struct UTag   **out_chain,
                                         size_t          out_cap);

void   urbi_strand_attach_ambient_tags(struct UStrand *new_s,
                                       struct UTag   **chain,
                                       size_t          chain_count);

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
 * (R, pc, pc_base, cur_consts, frame_count, open_upvals, closure_list,
 * closed_cells, out_slot) from `entry` and the strand's own vm->alloc_fn.
 *
 * Called by fork_spawn_child (T38) and the watcher body-spawn path (T24)
 * so the stack-alloc + pc-arming sequence is not duplicated.
 *
 * Returns 0 on success, -1 on allocation failure (s is left unarmed; caller
 * is responsible for tearing down s).
 *
 * NOTE: does NOT set s->module — callers that need module for diagnostics or
 * nested-proto lookup must set it explicitly after this call returns 0. */
int urbi_strand_arm_from_closure(struct UStrand *s, struct UClosure *entry);

#ifdef __cplusplus
}
#endif

#endif /* USTRAND_H */
