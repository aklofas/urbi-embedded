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
#include "ugc_capi.h" /* UCell, UType, UGcRootCallback/ProviderFn, inline barriers */

#ifdef __cplusplus
extern "C" {
#endif

/* --- M3 forward declarations (rows 8, 9, 10, 11) ---
   Types referenced in the UVM struct but defined in later tasks.
   Forward-decl only: all uses are pointer-typed.
   Note: UCell, UType, UGcRootCallback, UGcRootProviderFn are now defined in ugc.h
   (pulled in via ugc_capi.h above). */
struct UStrand;
struct UEvent;
struct URealm;
struct UWatcher;
struct UEventRing;   /* T18 lands the definition; event_ring is a pointer */

/* --- M3 capacity macros --- */
/* URBI_GC_INITIAL_THRESHOLD: canonical definition in ugc_incremental.h (T22).
   Retained here with a #ifndef guard for any TU that includes uvm.h without
   ugc_capi.h in scope, though that should not occur in practice. */
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

/* --- VM state --- */

#define UVM_ERRMSG_CAP 128

typedef struct UVM {
    UVMAllocFn alloc_fn;
    void      *alloc_ud;
    UVMError   last_error;
    char       last_errmsg[UVM_ERRMSG_CAP];

    /* M2 additions — per pre-m2-multi-vm-audit-design.md */
    void      *intern_table;     /* opaque; owned by uintern.c (T3) */
    uint32_t   topology_gen;     /* shape-tree generation; bumped at M4
                                    on any slot-topology mutation. Zero-
                                    init; never bumped at M2. */

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
    uint8_t  pad_in_eval[3];           /* padding; zeroed */
    void    *watcher_scratch_frame;    /* UScratchFrame ~280 B; T34 allocates */

    /* --- Row 11 pending on-leave queue --- */
    struct UWatcher *pending_onleave_head;
    struct UWatcher *pending_onleave_tail;

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

#ifdef __cplusplus
}
#endif

#endif /* UVM_H */
