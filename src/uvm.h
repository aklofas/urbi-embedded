/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode interpreter. Freestanding. */

#ifndef UVM_H
#define UVM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "umodule.h"  /* UModule, UValue, UValKind, UOpcode */
#include "uvalue.h"   /* UValue — needed for handle_table field */
#include "uframe.h"   /* UCallFrame, UUpvalCell, UVM_MAX_FRAMES, UVM_STACK_CAP */
#include "urbi/gc.h" /* UCell, UType, UGcRootCallback/ProviderFn, inline barriers */

#ifdef __cplusplus
extern "C" {
#endif

/* --- M3 forward declarations (rows 8, 9, 10, 11) ---
   Types referenced in the UVM struct but defined in later tasks.
   Forward-decl only: all uses are pointer-typed.
   Note: UCell, UType, UGcRootCallback, UGcRootProviderFn are now defined in ugc.h
   (pulled in via urbi/gc.h above). */
struct UStrand;
struct UEvent;
struct URealm;
struct UWatcher;
struct UEventRing;   /* T18 lands the definition; event_ring is a pointer */
struct UShape;       /* M4 — defined in src/object/ushape.h */
struct UModuleInstance;   /* M4 T30 — defined in src/object/umoduleinstance.h */

/* --- M3 capacity macros --- */
/* Dead path — uvm.h always pulls urbi/gc.h.  Guard retained only to prevent
 * double-definition warnings if ugc_incremental.h is included standalone. */
#ifndef URBI_GC_INITIAL_THRESHOLD
#  define URBI_GC_INITIAL_THRESHOLD (16 * 1024)
#endif

#ifndef URBI_MAX_ROOT_PROVIDERS
#  define URBI_MAX_ROOT_PROVIDERS 8u        /* row 10 §5.1; T26 may move to ugc.h */
#endif

/* --- errors --- */

typedef enum {
    UVM_OK = 0,
    UVM_TYPE_ERROR,
    UVM_OOM,
} UVMError;

/* --- pluggable allocator (matches umodule pattern) --- */

typedef void *(*UVMAllocFn)(void *ptr, size_t nbytes, void *ud);
/* Standard realloc semantics:
 *   ptr == NULL && nbytes > 0  : allocate fresh buffer; return non-NULL or NULL on OOM.
 *   ptr != NULL && nbytes == 0 : free ptr; return NULL.
 *   ptr != NULL && nbytes > 0  : reallocate ptr to nbytes (may move); return non-NULL or NULL on OOM.
 *   ptr == NULL && nbytes == 0 : no-op; return NULL. */

/* UCallFrame, UUpvalCell, UVM_MAX_FRAMES, UVM_STACK_CAP are in uframe.h (included above). */

/* --- spec #2 §5.2: per-VM install-time trace state ---
 * URBI_WATCHER_READSET_MAX sets the trace_read_set[] capacity.
 * Default 16; footprint preset (cross-arm/cross-riscv Makefile) overrides to 4.
 * uwatcher.h carries the same guard — when both headers are included the first
 * definition wins; both use identical defaults so either order is safe. */
#ifndef URBI_WATCHER_READSET_MAX
#  define URBI_WATCHER_READSET_MAX  16
#endif

/* --- spec #4 §3.5: deferred slot-change ring ---
 * URBI_DEFERRED_SLOT_CHANGE_RING_SIZE sets the capacity of the per-VM
 * deferred-emit ring used when a slot-write barrier fires inside a sync
 * slot-change body (re-entrancy).  Default 64; footprint preset → 16.
 * The ring is heap-allocated (one calloc at uvm_init), not GC-managed. */
#ifndef URBI_DEFERRED_SLOT_CHANGE_RING_SIZE
#  define URBI_DEFERRED_SLOT_CHANGE_RING_SIZE  64
#endif

/* Entry in the deferred slot-change ring (spec #4 §3.5).
 * Holds a strong reference to parent/key/new_value only while the entry
 * is live (head != tail).  Drain logic (R6) clears each slot after firing.
 * NOT GC-managed — entries are transient. */
typedef struct UDeferredSlotChange {
    struct UObject *parent;
    struct USymbol *key;
    UValue          new_value;
} UDeferredSlotChange;

/* --- Scratch frame for watcher condition + onleave evaluation (T34) ---
 *
 * One per VM, allocated at uvm_init and freed at uvm_destroy.
 * Used by watcher_eval_dirty (§6.2) and drain_pending_onleave_queue (§6.5):
 * both run at safepoints which are serialized, so the single frame is safe.
 *
 * M5 owns the real urbi_run_closure_on_scratch that executes bytecode here.
 * At M3, invoke_condition_closure uses the test_watcher_condition_hook instead.
 * The struct is allocated now so M5 can wire real execution without a layout change.
 *
 * Override URBI_SCRATCH_FRAME_REGS for footprint-constrained builds:
 *   default 16 registers → ~280 B per VM (16 × 16 B UValue + 8+2+16 header). */
#ifndef URBI_SCRATCH_FRAME_REGS
#  define URBI_SCRATCH_FRAME_REGS  16
#endif

typedef struct UScratchFrame {
    struct UClosure *closure;                      /* current condition/body being evaluated */
    UValue           registers[URBI_SCRATCH_FRAME_REGS];
    uint16_t         register_top;
    UValue           result;                       /* return value after M5 execution */
} UScratchFrame;   /* ~280 B at default URBI_SCRATCH_FRAME_REGS=16 */

/* --- VM state --- */

#define UVM_ERRMSG_CAP 128

typedef struct UVM {
    UVMAllocFn alloc_fn;
    void      *alloc_ud;
    UVMError   last_error;
    char       last_errmsg[UVM_ERRMSG_CAP];

    /* M2 additions — per pre-m2-multi-vm-audit-design.md */
    void      *intern_table;     /* opaque; owned by uintern.c (T3) */
    uint64_t   topology_gen;     /* shape-tree generation; bumped on §4.1 mutations.
                                    Per pre-M4 topology-generation spec §3.1: monotonic
                                    only, init=1, reserves 0 as IC unfilled sentinel. */

    /* === M4 additions (per pre-M4 prototype-chain spec §7.1, §8.1) === */
    uint64_t   lookup_id;        /* per-VM monotonic counter; bumped at each top-level
                                    lookup; truncated to u32 when stamping UObject.lookup_stamp.
                                    Mark phase clears stamps + resets to 1 on low-32 wrap.
                                    Init=1. */
    uint32_t   next_object_id;   /* per-VM monotonic UObject identity counter; populated
                                    at urbi_object_alloc. 32-bit wrap aborts the VM with
                                    URBI_FATAL_OBJECT_ID_EXHAUSTED. Init=0 (first object → 1). */
    struct UShape *root_shape;   /* lazy-allocated root hidden class
                                    (per pre-M2 §7.1).  NULL until first
                                    urbi_shape_root() call. */

    /* === M4 atom-family singletons (T8) ===
     * Lazy-allocated per-VM atom prototypes; pinned via urbi_pin so they
     * survive early GC cycles before T36's root provider lands.  Each is
     * NULL until first urbi_object_root / urbi_object_atom call.  Slot
     * order mirrors URBIAtomFamily values 0..8 (object/integer/float/
     * string/list/dict/tag/event/symbol). */
    struct UObject *atom_object;     /* root Object — atom of all atoms; protos = empty */
    struct UObject *atom_integer;
    struct UObject *atom_float;
    struct UObject *atom_string;
    struct UObject *atom_list;
    struct UObject *atom_dict;
    struct UObject *atom_tag;
    struct UObject *atom_event;
    struct UObject *atom_symbol;

    /* === M4 T30 — UModuleInstance registry ===
     * Linked list head of every live UModuleInstance threaded via
     * UModuleInstance.next_in_vm.  Created at urbi_module_instance_create
     * time (no removal at v1.0 — the GC reaps both the cell and any chain
     * dangling references when the instance becomes unreachable; this
     * registry is consulted only by the determinism checksum which itself
     * runs at quiescent points where instance removal isn't observed). */
    struct UModuleInstance *module_instances_head;

    /* Pre-GC closure ownership: the closure (if any) returned by the most
     * recent uvm_run() call.  Freed at the start of the next uvm_run() or
     * on uvm_destroy().  Allows callers to inspect *out without immediately
     * freeing it, while preventing leaks across multi-run sessions (REPL). */
    UClosure   *last_return_closure;

    /* ================================================================
     * M3 additions (rows 8, 9, 10, 11 of the pre-M3 design bundle)
     * All pointer fields zero-init to NULL; uint fields zero-init to 0.
     * Non-zero defaults set explicitly in uvm_init().
     * ================================================================ */

    /* --- Row 8 + Rule X: 5-flag liveness counters ---
     * Quiescence is defined as all five counters being zero simultaneously.
     * strand_suspended_count is excluded from the quiescence check at M3
     * (always 0; included here for completeness per row 9 §2.6). */
    uint32_t strand_runnable_count;    /* row 8 §3 + row 9 §2.6 */
    uint32_t strand_suspended_count;   /* row 9 §2.6; always 0 at M3 */
    uint32_t watcher_active_count;     /* row 8 §3; M5 maintains */
    uint32_t event_queue_count;        /* row 8 §3; M5+ shape */
    uint32_t wakeup_pending_count;     /* row 8 §3; scheduler timer heap */
    uint32_t host_call_pending_count;  /* row 8 §3 + row 9; cross-strand stop injection */

    /* --- Row 8 realm/fatal-strand pointers and counters --- */
    struct URealm  *realms_head;       /* linked list of all realms; T14 maintains */
    struct URealm  *global_realm;      /* lazy-created on first urbi_realm_global() */
    struct UStrand *fatal_strand;      /* set by urbi_step on FATAL; NULL otherwise */
    uint32_t        realm_id_seq;      /* per-VM monotonic Realm ID counter; starts at 0, incremented to 1 on first create */

    /* --- Row 9 scheduler queues --- */
    struct UStrand *ready_head;        /* run-queue head (FIFO); NULL = empty */
    struct UStrand *ready_tail;        /* run-queue tail for O(1) enqueue */
    struct UStrand *sleep_q_head;      /* sleep queue head, sorted by wake_us */

    /* --- Row 9 step driver state --- */
    uint64_t step_budget_remaining;    /* opcode budget for current urbi_step() call */

    /* --- Row 9 dispatcher hooks --- */
    uint16_t gc_pending;               /* non-zero → gc_slice() at next safepoint */
    uint32_t watcher_dirty_count;      /* non-zero → watcher_eval_dirty() at scheduler turn */

    /* --- Row 9 v2 reservation --- */
    uint8_t  flag_preemption;          /* RESERVED — always 0 at M3 */
    uint8_t  flag_reserved[3];         /* padding; zeroed */

    /* --- Row 9 ISR-safe event ring ---
     * Stored as a pointer (definition lands at T18).  Zero-init = NULL. */
    struct UEventRing *event_ring;     /* T18: uevent_ring_init(vm) allocates */

    /* --- Row 10 GC state machine --- */
    uint8_t  gc_phase;                 /* 0 = IDLE per row 10 §6.2; named constant lands at T22 */
    uint8_t  current_white;            /* current white color for tri-color marking */
    uint8_t  gc_paused;                /* non-zero → GC slices suppressed */
    uint8_t  in_destroy_callback;      /* debug-build assertion guard (T22/T27 use) */
    int64_t  gc_debt;                  /* negative = credit; positive = GC work owed */
    size_t   gc_threshold;             /* debt threshold; default URBI_GC_INITIAL_THRESHOLD */
    size_t   gc_live_bytes;            /* live bytes after last sweep cycle */
    size_t   gc_total_allocated;       /* monotonically increasing allocation counter */
    struct UCell *all_cells_head;      /* intrusive list of all GC-managed cells */
    struct UCell *gray_work_head;      /* mark-phase gray worklist */
    struct UCell *sweep_cursor;        /* incremental sweep position */
    struct UCell *sweep_cursor_prev;   /* previous cell (for list surgery) */

    /* --- Row 10 GC root provider registry --- */
    UGcRootProviderFn root_providers[URBI_MAX_ROOT_PROVIDERS];
    uint8_t           root_provider_count;

    /* --- Row 10 type table --- */
    struct UType *type_table[256];     /* indexed by type_tag byte; T22/T27 populate */
    uint8_t       host_type_count;     /* host-registered types since UTYPE_HOST_BASE */

    /* --- Row 10 host-handle table (T27 allocates) --- */
    UValue   *handle_table;            /* flat array of pinned host handles */
    uint32_t  handle_table_cap;
    uint32_t  handle_table_next_id;

    /* --- Row 11 watcher pool (T32 allocates) --- */
    struct UWatcher *watcher_pool_base;      /* base of pre-allocated pool */
    struct UWatcher *watcher_pool_freelist;  /* freelist head */
    struct UWatcher *active_watchers_head;   /* linked list of live watchers */
    uint16_t         watcher_pool_in_use;
    uint16_t         watcher_pool_high_water;

    /* --- Row 11 watcher dirty-set + scratch frame --- */
    uint8_t  in_watcher_eval;          /* reentrancy guard */
    uint8_t  in_watcher_scratch;       /* spec #3 §5.4: set while running event body
                                          inline on scratch frame; guards re-entrancy
                                          in c_event_emit_sync / c_event_waituntil. */
    uint8_t  pad_in_eval[2];           /* padding; zeroed */
    void    *watcher_scratch_frame;    /* UScratchFrame ~280 B; T34 allocates */

    /* --- spec #2 §5.2 install-time trace state ---
     * in_watcher_install: set while evaluating cond during watcher install
     *   to enable OP_GETSLOT read-set tracing.  Mutually exclusive with
     *   in_watcher_eval (never both set at once; URBI_DEBUG asserts land in R4).
     * trace_overflow: set when trace_read_set[] is full and a new cell would
     *   have been recorded.  Install treats overflow as "untrackable — skip IC".
     * trace_read_set_count: number of valid UCell* entries written into
     *   trace_read_set[].  Reset to 0 at the start of each install evaluation.
     * trace_read_set[]: ring buffer of UCell pointers touched during install
     *   cond evaluation; written by OP_GETSLOT when in_watcher_install is set.
     *   Array is uninitialized storage; only indices [0, trace_read_set_count)
     *   are valid.  Sized by URBI_WATCHER_READSET_MAX. */
    uint8_t   in_watcher_install;
    uint8_t   trace_overflow;
    uint16_t  trace_read_set_count;
    struct UCell *trace_read_set[URBI_WATCHER_READSET_MAX];

    /* M3-only test hooks for watcher eval/fire (M5 replaces with real
     * urbi_run_closure_on_scratch and spawn_body_coroutine).
     *
     * test_watcher_condition_hook: replaces invoke_condition_closure when non-NULL.
     *   Tests install this to feed deterministic condition values for edge/level
     *   firing tests.  NULL → invoke_condition_closure returns UVAL_NIL.
     *   See spec §6.4 + §6.8 for the M3 stub rationale.
     *
     * test_watcher_fire_hook: invoked by spawn_body_coroutine when non-NULL.
     *   Tests install this to observe watcher body fires.  NULL → no-op at M3. */
    UValue (*test_watcher_condition_hook)(struct UVM *vm, struct UWatcher *w);
    void   (*test_watcher_fire_hook)(struct UVM *vm, struct UWatcher *w);

    /* M3-only test hook for run_watcher_onleave (M5 replaces with real
     * urbi_run_closure_on_scratch).  NULL → run_watcher_onleave is no-op. */
    void   (*test_watcher_onleave_hook)(struct UVM *vm, struct UWatcher *w);

    /* T37 stub test hook for run_closure_on_scratch_frame_with_result.
     * M5 will replace this with real bytecode dispatch on vm->watcher_scratch_frame.
     * When non-NULL, install_watcher_runtime calls this instead of the real runner.
     *   Signature: hook(vm, cond, out_result, out_threw)
     *   - out_result receives the simulated return value.
     *   - *out_threw is set to 1 to simulate a cond-throw (URBI_INSTALL_TRACE_FAULT).
     * NULL → run_closure_on_scratch_frame_with_result returns UVAL_NIL, no throw. */
    void   (*test_install_cond_hook)(struct UVM *vm, struct UClosure *cond,
                                     UValue *out_result, int *out_threw);

    /* --- Row 11 pending on-leave queue --- */
    struct UWatcher *pending_onleave_head;
    struct UWatcher *pending_onleave_tail;

    /* --- spec #4 §3.5 slot-change re-entrancy + deferred-emit ring ---
     * slot_change_reentrancy_warned: one-shot flag; set on first re-entrant
     *   slot-write during a sync slot-change body; gates URBI_LOG_WARN.
     * deferred_slot_changes: heap-allocated ring buffer (cap entries),
     *   freed in uvm_destroy.  NOT GC-managed — entries live only while
     *   head != tail; drain logic (R6) clears each slot after firing.
     * head/tail: SPSC ring indices (mod cap).  head == tail → empty.
     * cap: URBI_DEFERRED_SLOT_CHANGE_RING_SIZE at init. */
    uint8_t                 slot_change_reentrancy_warned;
    UDeferredSlotChange    *deferred_slot_changes;
    uint16_t                deferred_slot_changes_head;
    uint16_t                deferred_slot_changes_tail;
    uint16_t                deferred_slot_changes_cap;

    /* --- Row 9 host time hook --- */
    uint64_t (*host_time_us)(void);    /* returns monotonic microseconds; default set at init */

    /* --- T19 ISR-check + debug watchdog hooks ---
     * isr_check_fn: returns true when called from ISR context; NULL = no check.
     *   In URBI_DEBUG builds, every non-ISR-safe function asserts isr_check_fn() == false.
     * host_log_fn: structured log callback; NULL = silent.  Called by debug-build
     *   watchdog when a host callback exceeds callback_warn_us.
     * callback_warn_us: watchdog threshold in microseconds (default URBI_CALLBACK_WARN_US).
     * callback_watchdog_mode: URBI_WATCHDOG_WARN (0) or URBI_WATCHDOG_ASSERT (1). */
    bool     (*isr_check_fn)(void);
    void     (*host_log_fn)(struct UVM *vm, int level, const char *fmt, ...);
    uint32_t   callback_warn_us;
    uint8_t    callback_watchdog_mode;
    uint8_t    pad_watchdog[3];        /* padding; zeroed */
} UVM;

/* --- API --- */

/* Initialize vm. On hosted builds, passing alloc_fn == NULL wires up a
   stdlib-realloc shim internally. On freestanding builds the caller MUST
   supply alloc_fn; if NULL is passed, uvm_init still returns (cannot fail
   at M1), but any subsequent uvm_run will NULL-deref in the frame
   allocation path — caller's bug. Zero-initializes last_error and
   last_errmsg. */
void uvm_init(UVM *vm, UVMAllocFn alloc_fn, void *alloc_ud);

/* Strand-driven dispatch loop (T6).  Runs s's bytecode until one of:
   - strand reaches DEAD (top-level OP_RET or halt_error)
   - strand voluntarily yields via OP_YIELD (state → READY)
   - step_budget_in opcodes have been consumed (state remains RUNNING)
   Returns the number of opcodes consumed.  s->vm must be non-NULL.
   Caller must have initialised s->stack, s->R, s->pc, s->pc_base,
   s->cur_consts, s->module, and s->state = USTRAND_STATE_RUNNING. */
uint64_t dispatch_loop_until_yield(struct UStrand *s, uint64_t step_budget_in);

/* Run module to completion. On UVM_OK, *out receives the RET value. On
   error, vm->last_error and vm->last_errmsg are populated and *out is
   set to UVAL_NIL (kind = UVAL_NIL, value payload zeroed).
   last_error and last_errmsg are reset at entry — a caller may inspect
   them after each uvm_run call without stale state from prior runs. */
UVMError uvm_run(UVM *vm, const UModule *module, UValue *out);

/* Free any VM-owned resources. Safe to call on a zero-initialized UVM. */
void uvm_destroy(UVM *vm);

/* Return a static string such as "UVM_TYPE_ERROR" for debug. */
const char *uvm_error_name(UVMError code);

/* --- Internal cross-module declarations ---
 * Originally in uvm_internal.h (consolidated post-M3). All src/ headers are
 * internal-by-definition after the include/urbi/ split — no separate friend
 * header needed. */

/* Heapify all open upvalue cells whose stack address is >= threshold.
 * Removed cells are appended to *closed_list.
 * Called by OP_CLOSE, OP_RET, and urbi_unwind. */
void vm_close_upvalues(struct UStrand *s, UValue *threshold,
                       UUpvalCell **closed_list);

#ifdef __cplusplus
}
#endif

#endif /* UVM_H */
