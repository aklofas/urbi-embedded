/* SPDX-License-Identifier: BSD-3-Clause */
/* UStrand state byte encoding + lifecycle declarations.
   Full lifecycle operations land across T20 (create/start/spawn) and T29 (tag fields). */

#ifndef USTRAND_H
#define USTRAND_H

#include <stdint.h>
#include <stddef.h>   /* size_t */
#include "uvalue.h"   /* pulls in umodule.h which defines UValue — must come before uframe.h */
#include "uframe.h"   /* UCallFrame, UUpvalCell, UVM_MAX_FRAMES, UVM_STACK_CAP */

#ifdef __cplusplus
extern "C" {
#endif

/* === State byte encoding (row 9 §3.1) ===
   Upper nibble: logical state class.
   Lower nibble: reason sub-code (meaningful only in WAITING). */

#define USTRAND_STATE_MASK     0xF0u
#define USTRAND_REASON_MASK    0x0Fu

/* Logical state classes (pre-shifted into upper nibble). */
#define USTRAND_DORMANT        0x00u  /* allocated, not yet enqueued */
#define USTRAND_READY          0x10u  /* on run-queue */
#define USTRAND_RUNNING        0x20u  /* currently dispatching */
#define USTRAND_WAITING        0x30u  /* blocked, see reason sub-code */
#define USTRAND_DEAD           0x40u  /* terminated, awaiting GC */
#define USTRAND_SUSPENDED      0x50u  /* RESERVED — Tag.freeze (M5/M6) */

/* WAITING reason sub-codes (lower nibble). */
#define USTRAND_REASON_NONE    0x00u
#define USTRAND_REASON_SLEEP   0x01u
#define USTRAND_REASON_EVENT   0x02u
#define USTRAND_REASON_JOIN    0x03u
#define USTRAND_REASON_HOST    0x04u  /* RESERVED v1.x/v2 */

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

/* Helper macros — take a pointer to UStrand. */
#define USTRAND_IS_WAITING(s)  (((s)->state & USTRAND_STATE_MASK) == USTRAND_WAITING)
#define USTRAND_GET_STATE(s)   ((s)->state & USTRAND_STATE_MASK)
#define USTRAND_GET_REASON(s)  ((s)->state & USTRAND_REASON_MASK)

/* === UExecStatus (row 7 §2; full T8 lands the dispatch transition) === */

typedef enum {
    UEXEC_OK = 0,
    UEXEC_RETURN,
    UEXEC_THROW,
    UEXEC_TAG_STOP,
    UEXEC_CANCEL
} UExecStatus;

/* === Cleanup-stack type (T3) ===
   ucleanup.h defines UCleanupEntry and the stack init/destroy ops. */

#include "ucleanup.h"

/* === Forward declarations for types that land in later tasks. === */

struct UTag;             /* T29 */
struct UEvent;           /* reactive runtime */
struct UVM;              /* uvm.h — forward-decl to avoid circular include */
struct URealm;           /* urealm.h — forward-decl for strand lifecycle context */
struct UModule;          /* umodule.h — forward-decl for strand execution context */
struct UClosure;         /* umodule.h — forward-decl for closure list threading */
struct UModuleInstance;  /* object/umoduleinstance.h — M4 follow-up: per-(vm,module) IC tier */

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
    uint8_t                 is_uvm_run_transient;       /* T33 (pre-M4 GC strand-walker §5.1):
                                                           set by uvm_run's stack-local transient
                                                           strand so OP_FORK_DETACH / OP_FORK_JOIN
                                                           can still reject forks here even though
                                                           realm now points at vm->global_realm.
                                                           Always 0 for urbi_strand_create-managed
                                                           strands (heap-allocated). */
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

    /* --- Join-blocker list (OP_FORK_JOIN / OP_JOIN_WAIT) ---
     * Singly-linked list of strands that are JOIN-blocked on THIS strand.
     * Threaded via each joiner's wait_next field.
     * fork_wake_joiners() walks this list when the strand reaches DEAD. */
    UStrand                *joiners_head;

    /* --- Realm ownership list (T38) ---
     * Singly-linked list of all strands created under the same URealm.
     * Populated by urbi_strand_create; walked by urbi_realm_destroy to free
     * all realm-managed strands when the realm is torn down.
     * NULL for strands not created via urbi_strand_create (e.g. uvm_run transient). */
    UStrand                *next_in_realm;

    /* --- M2-baseline execution state migrated from uvm_run-locals + UVM at T6 ---
       These fields are valid only while the strand is RUNNING or READY (paused mid-run).
       uvm_run's thin adapter initialises them before calling dispatch_loop_until_yield
       and tears them down after the strand transitions to DEAD.
       T20 will move strand creation here when the full Strand C API lands. */
    UValue                 *stack;          /* heap-alloc'd register array; UVM_STACK_CAP slots */
    UValue                 *R;              /* current frame register base (derived from stack) */
    const uint32_t         *pc;             /* current instruction pointer */
    const uint32_t         *pc_base;        /* base of current frame's instruction array */
    const UValue           *cur_consts;     /* current frame's constant pool */
    const struct UModule   *module;         /* top-level module (diagnostics + nested protos) */
    struct UModuleInstance *module_instance; /* M4 follow-up: per-(vm,module) IC RAM tier;
                                               bound by uvm_run / urbi_run_chunk via
                                               urbi_get_or_create_module_instance.  May be
                                               NULL if not yet wired (defensive). */
    UCallFrame              frames[UVM_MAX_FRAMES];
    int                     frame_count;
    UUpvalCell             *open_upvals;    /* open upvalue cells still pointing into stack */
    struct UClosure        *closure_list;   /* pre-GC: all closures allocated this run */
    UUpvalCell             *closed_cells;   /* pre-GC: all heapified upvalue cells this run */
    UValue                 *out_slot;       /* adapter-set: OP_RET at top-frame writes here */
};

/* === Lifecycle functions (stubs; full impl across T20 + T29) ===

   ustrand_init zeros the strand, sets DORMANT state, and pre-allocates the
   cleanup stack using vm->alloc_fn.  On allocation failure the strand is
   left in a detectable malformed-DORMANT state (cleanup_base == NULL);
   callers must check.

   ustrand_destroy frees the cleanup stack using vm->alloc_fn.  The same vm
   pointer used for init must be passed to destroy. */

void ustrand_init(UStrand *s, struct UVM *vm);
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

#ifdef __cplusplus
}
#endif

#endif /* USTRAND_H */
