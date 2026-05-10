/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode interpreter. Freestanding. */

#ifndef UVM_H
#define UVM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "module/umodule.h"  /* UModule, UValue, UValKind, UOpcode */
#include "value/uvalue.h"   /* UValue — needed for handle_table field */
#include "runtime/uframe.h"   /* UCallFrame, UUpvalCell, UVM_MAX_FRAMES, UVM_STACK_CAP */
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
struct UModuleInstance;   /* M4 T30 — defined in src/object/umodule_instance.h */

/* --- M3 capacity macros --- */
/* Dead path — uvm.h always pulls urbi/gc.h.  Guard retained only to prevent
 * double-definition warnings if ugc_incremental.h is included standalone. */
#ifndef URBI_GC_INITIAL_THRESHOLD
#  define URBI_GC_INITIAL_THRESHOLD (16 * 1024)
#endif

#ifndef URBI_MAX_ROOT_PROVIDERS
#  define URBI_MAX_ROOT_PROVIDERS 8U        /* row 10 §5.1; T26 may move to ugc.h */
#endif

/* --- errors + pluggable allocator ---
 *
 * UVMError and UVMAllocFn moved to <urbi/types.h> at v0.5.5 (T17) so
 * include/urbi/urbi.h can declare urbi_vm_run / urbi_vm_init without
 * pulling internal headers.  See <urbi/types.h> for the canonical
 * definitions and the realloc-semantics docstring. */
#include "urbi/types.h"

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
 * The ring is heap-allocated (one calloc at urbi_vm_init), not GC-managed. */
#ifndef URBI_DEFERRED_SLOT_CHANGE_RING_SIZE
#  define URBI_DEFERRED_SLOT_CHANGE_RING_SIZE  64
#endif

/* Entry in the deferred slot-change ring (spec #4 §3.5).
 *
 * GC rooting contract (closes GC-004 — doc-only at v1.0):
 *   The (parent, key, new_value) triple is NOT walked by any GC root
 *   provider.  parent (UObject *) and any heap-bearing UValue inside
 *   new_value are weak references across a GC slice.  Correctness at v1.0
 *   relies on a strict safepoint-ordered invariant maintained by the
 *   cooperative scheduler:
 *
 *     defer-site (slot-write barrier inside a sync slot-change body)
 *       --> next safepoint
 *       --> urbi_drain_deferred_slot_changes (clears head..tail)
 *       --> watcher_eval_dirty
 *
 *   No GC slice runs between defer-site and drain because the cooperative
 *   scheduler only steps GC at the dispatch-loop safepoint, and the drain
 *   happens at that same safepoint *before* any potential GC trigger.
 *   This invariant is implicitly verified by the full 148-fixture .chk
 *   corpus passing under URBI_GC_INCREMENTAL with stress-mode allocation.
 *
 *   v1.x preemption upgrade path (filed; NOT addressed here):
 *     A preemptive scheduler that can step GC asynchronously between
 *     defer-site and drain MUST upgrade these to strong refs by visiting
 *     vm->deferred_slot_changes[head..tail] from the GC root walker (e.g.
 *     a new urbi_deferred_ring_walk_roots root provider registered with
 *     urbi_gc_register_root_provider).  Until then, runtime safety is
 *     contractual on the safepoint ordering.  See the workspace-root
 *     design-risks register entry "v1.x: deferred slot-change ring
 *     becomes weak under preemption" for the detailed upgrade plan.
 *
 * Storage: heap-allocated ring buffer, one urbi_vm-init calloc, freed in
 * urbi_vm_destroy.  NOT GC-managed at any tier — the cell entries are
 * transient and the drain logic zeroes each slot after firing. */
typedef struct UDeferredSlotChange {
    struct UObject *parent;
    struct USymbol *key;
    UValue          new_value;
} UDeferredSlotChange;

/* --- VM state --- */

#define UVM_ERRMSG_CAP 128

/* Field order is intentional and load-bearing:
 *  - The early fields (alloc_fn, alloc_ud, last_error, last_errmsg) form the
 *    init-by-zero error-handling prefix used by urbi_vm_init's failure paths.
 *  - The M2/M3/M4/M5 sections cluster fields by lifecycle (M2 intern table,
 *    M4 prototype/atom singletons, M5 reactive runtime) so reviewing each
 *    milestone's contribution stays local to one block.
 *  - Six _Static_assert layout pins (in src/vm/uvm.c, guarded on
 *    __SIZEOF_POINTER__ == 8) cement specific field offsets for cross-arch
 *    parity verification.
 * Reordering by clang-analyzer-optin.performance.Padding's optimal-pack
 * suggestion would shrink the struct by ~40 bytes (host) but break the
 * layout pins and scatter related fields across milestones, costing far
 * more in maintainability than the saved bytes are worth on a struct
 * allocated once per VM.  Suppress per-line. */
typedef struct UVM {  /* NOLINT(clang-analyzer-optin.performance.Padding) — field order is intentional, see comment above */
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

    /* === M4 atom-family singletons (T8) + M6 Phase 4 additions (Boolean /
     *     Nil / Void) ===
     * Lazy-allocated per-VM atom prototypes; pinned via the M4 T36 root
     * walker (object_roots_walker) which shades each non-NULL singleton
     * during MARK_ROOTS.  Each is NULL until first urbi_object_root /
     * urbi_object_atom call.  Slot order mirrors URBIAtomFamily values
     * 0..11 (object/integer/float/string/list/dict/tag/event/symbol —
     * Phase 4 adds boolean/nil/void). */
    struct UObject *atom_object;     /* root Object — atom of all atoms; protos = empty */
    struct UObject *atom_integer;
    struct UObject *atom_float;
    struct UObject *atom_string;
    struct UObject *atom_list;
    struct UObject *atom_dict;
    struct UObject *atom_tag;
    struct UObject *atom_event;
    struct UObject *atom_symbol;
    struct UObject *atom_boolean;    /* M6 Phase 4 — proto for UVAL_BOOL receivers */
    struct UObject *atom_nil;        /* M6 Phase 4 — proto for UVAL_NIL receivers */
    struct UObject *atom_void;       /* M6 Phase 4 — proto for UVAL_VOID receivers */

    /* === M5 T53/T54 — native proto objects ===
     * event_proto: UObject carrying native method slots (new/emit/syncEmit/waituntil).
     *   Allocated at urbi_vm_init by event_native_register.  NULL until then.
     *   Walked by urbi_object_register_gc_roots (added to atom-proto walk pass).
     * tag_proto: UObject carrying native getter slots (enter/leave).
     *   Allocated at urbi_vm_init by tag_native_register.  NULL until then.
     * Both protos have atom_event / atom_tag as their single prototype respectively,
     * mirroring the M4 atom hierarchy. */
    struct UObject *event_proto;
    struct UObject *tag_proto;

    /* === M4 T30 — UModuleInstance registry ===
     * Linked list head of every live UModuleInstance threaded via
     * UModuleInstance.next_in_vm.  Created at urbi_module_instance_create
     * time (no removal at v1.0 — the GC reaps both the cell and any chain
     * dangling references when the instance becomes unreachable; this
     * registry is consulted only by the determinism checksum which itself
     * runs at quiescent points where instance removal isn't observed). */
    struct UModuleInstance *module_instances_head;

    /* Pre-GC closure ownership: the closure (if any) returned by the most
     * recent urbi_vm_run() call.  Freed at the start of the next urbi_vm_run() or
     * on urbi_vm_destroy().  Allows callers to inspect *out without immediately
     * freeing it, while preventing leaks across multi-run sessions (REPL). */
    UClosure   *last_return_closure;

    /* ================================================================
     * M3 additions (rows 8, 9, 10, 11 of the pre-M3 design bundle)
     * All pointer fields zero-init to NULL; uint fields zero-init to 0.
     * Non-zero defaults set explicitly in urbi_vm_init().
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

    /* --- T57 ISR ring drain handler ---
     * Optional host callback installed via urbi_register_event_drain.
     * Called at safepoint (uevent_ring_drain) for each injected entry.
     * Handler maps event_id to a UEvent* and calls c_event_emit_async.
     * NULL = no drain handler (ring entries are discarded). */
    void (*event_drain_handler)(struct UVM *vm, uint32_t event_id, UValue payload);

    /* --- Row 10 GC state machine --- */
    uint8_t  gc_phase;                 /* 0 = IDLE per row 10 §6.2; named constant lands at T22 */
    uint8_t  current_white;            /* current white color for tri-color marking */
    uint8_t  gc_paused;                /* non-zero → GC slices suppressed */
    uint8_t  in_destroy_callback;      /* debug-build assertion guard (T22/T27 use) */
    int64_t  gc_debt;                  /* negative = credit; positive = GC work owed */
    size_t   gc_threshold;             /* debt threshold; default URBI_GC_INITIAL_THRESHOLD */
    size_t   gc_live_bytes;            /* live bytes after last sweep cycle */
    size_t   gc_surviving_bytes;       /* in-progress sweep accumulator (closes GC-015):
                                        * reset at SWEEP entry, incremented per cell
                                        * processed in each gc_sweep_step slice, and
                                        * written to gc_live_bytes at sweep complete.
                                        * Persisting across slices avoids re-walking
                                        * head→cursor each slice (which mis-counted
                                        * intra-slice allocations that prepended to
                                        * head between slices). */
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

    /* --- Row 11 watcher dirty-set --- */
    /* in_watcher_eval (WATCH-010): true while an at/whenever cond is being
     * evaluated.
     *
     * Drain dependency: urbi_emit_slot_change_slow re-routes through the
     * deferred ring when this flag is set; the at/whenever body wouldn't
     * see its own write-during-eval otherwise.  The flag is owner-set by
     * watcher_eval_dirty / drain_pending_onleave_queue and cleared on
     * cond return.
     *
     * Invariant: vm->in_watcher_eval implies that any urbi_emit_slot_change_slow
     * invocation routes the slot-change emit through the deferred ring, NOT the
     * immediate path. See src/emit/uchanged_emit.c for the routing. */
    uint8_t  in_watcher_eval;          /* reentrancy guard */
    /* in_watcher_scratch (WATCH-036): caller-owned re-entry guard.  Set
     * TRUE before calling urbi_run_closure_on_scratch[_with_payload];
     * clear after.  The helper itself does NOT manage this flag — see
     * WATCH-011 (uwatcher_scratch.c head comment on
     * urbi_run_closure_on_scratch) for the asymmetry rationale.
     *
     * spec #3 §5.4: also set while running event body inline on the
     * scratch frame; guards re-entrancy in c_event_emit_sync /
     * c_event_waituntil. */
    uint8_t  in_watcher_scratch;
    uint8_t  pad_in_eval[2];           /* padding; zeroed */

    /* --- spec #3 §7.1: currently-dispatching strand ---
     * Set to the running strand by urbi_step before dispatch_loop_until_yield,
     * cleared after.  Required by c_event_waituntil to locate the caller strand.
     * NULL when no strand is dispatching (between urbi_step slices). */
    struct UStrand *cur_strand;

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

    /* Test seams for the watcher fast path.  Originally introduced as M3
     * stubs and retained at v0.5.5 because all four are still load-bearing
     * for C-level unit tests; structural removal of these seams is deferred
     * to Wave 5 (v0.5.7-fixes) per WATCH-023.  Production code paths (real
     * watcher dispatch via urbi_run_closure_on_scratch) coexist with the
     * hooks: each consumer checks the hook pointer and falls through to the
     * real path when NULL.
     *
     * test_watcher_condition_hook: replaces invoke_condition_closure when non-NULL.
     *   Tests install this to feed deterministic condition values for edge/level
     *   firing tests.  NULL → invoke_condition_closure runs the real cond closure.
     *
     * test_watcher_fire_hook: invoked by spawn_body_coroutine when non-NULL.
     *   Tests install this to observe watcher body fires.  NULL → real body spawn. */
    UValue (*test_watcher_condition_hook)(struct UVM *vm, struct UWatcher *w);
    void   (*test_watcher_fire_hook)(struct UVM *vm, struct UWatcher *w);

    /* Test seam for run_watcher_onleave; same Wave-5 deferral as above.
     * NULL → run_watcher_onleave runs the real onleave path. */
    void   (*test_watcher_onleave_hook)(struct UVM *vm, struct UWatcher *w);

    /* Install-time cond-eval test seam.
     * When non-NULL, install_watcher_runtime calls this hook instead of the
     * real urbi_run_closure_on_scratch (uwatcher_scratch.c) — used by C-level
     * unit tests that inject specific cond results or simulate cond-throws.
     *   Signature: hook(vm, cond, out_result, out_threw)
     *   - out_result receives the simulated return value.
     *   - *out_threw is set to 1 to simulate a cond-throw (URBI_INSTALL_TRACE_FAULT).
     * NULL → install_watcher_runtime calls the real urbi_run_closure_on_scratch.
     * Same Wave-5 deferral as the three watcher hooks above. */
    void   (*test_install_cond_hook)(struct UVM *vm, struct UClosure *cond,
                                     UValue *out_result, int *out_threw);

    /* --- Row 11 pending on-leave queue --- */
    struct UWatcher *pending_onleave_head;
    struct UWatcher *pending_onleave_tail;

    /* --- spec #4 §3.5 slot-change re-entrancy + deferred-emit ring ---
     * slot_change_reentrancy_warned: one-shot flag; set on first re-entrant
     *   slot-write during a sync slot-change body; gates URBI_LOG_WARN.
     * slot_change_ring_full_warned: one-shot flag; set when the deferred ring
     *   is full and an entry is dropped; gates URBI_LOG_WARN (spec §5.3).
     * deferred_slot_changes: heap-allocated ring buffer (cap entries),
     *   freed in urbi_vm_destroy.  NOT GC-managed — entries live only while
     *   head != tail; drain logic (R6) clears each slot after firing.  See
     *   the contract on `UDeferredSlotChange` (above) for the full GC
     *   safepoint-ordering invariant that keeps the weak parent + value
     *   pointers safe at v1.0 (closes GC-004 — doc-only).
     * head/tail: SPSC ring indices (mod cap).  head == tail → empty.
     * cap: URBI_DEFERRED_SLOT_CHANGE_RING_SIZE at init. */
    uint8_t                 slot_change_reentrancy_warned;
    uint8_t                 slot_change_ring_full_warned;
    /* event_sync_degradation_warned: one-shot flag; set on first
     * c_event_emit_sync degradation to async (spec #3 §5.4 — call from
     * within a scratch / eval / install context).  Mirrors the
     * slot_change_reentrancy_warned shape so a tight loop that triggers
     * the degradation does not flood URBI_LOG_WARN.  Closes EMITR-005. */
    uint8_t                 event_sync_degradation_warned;
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

    /* === M6 Phase 3 stdlib state ===
     * stdlib_closures: linked list of native UClosures registered by
     *   urbi_object_root_register (and future stdlib boot phases) AND of
     *   user-script UClosures migrated from strand closure_lists at run
     *   exit (see uvm_run.c).  Both flavours share the same VM-lifetime
     *   ownership and are reclaimed via urbi_vm_destroy's single sweep.
     * stdlib_upvalues: linked list of heapified UUpvalCells migrated from
     *   strand closed_cells at run exit.  Closures that survive the run
     *   (now on stdlib_closures) reference these upvals; freeing them at
     *   run-end would dangle the closure's upvals[] array.  Threaded via
     *   UUpvalCell.next; freed at urbi_vm_destroy.
     * stdlib_module: heap-allocated UModule deserialized from the baked
     *   urbi_stdlib_bytecode blob during urbi_stdlib_boot.  NULL when the
     *   blob is empty (Phase 4 baseline) or boot has not run.  Owned by
     *   the VM; freed via umodule_destroy + alloc_fn at urbi_vm_destroy.
     * stdlib_booted: idempotency guard for urbi_stdlib_boot.  Set on first
     *   successful boot; subsequent calls are no-ops.
     * last_recv: receiver UValue from the most recent OP_GETSLOT load.
     *   Read by OP_CALL when invoking a closure with native_fn != NULL —
     *   that closure's `self` argument comes from here.  No bytecode
     *   change required: native methods are only stored as slot values and
     *   are therefore loaded via OP_GETSLOT, which writes here unconditionally.
     *   Stale on non-method calls (where native_fn is NULL); the OP_CALL
     *   arm reads this only on the native_fn != NULL branch. */
    UClosure   *stdlib_closures;
    UUpvalCell *stdlib_upvalues;
    struct UModule *stdlib_module;      /* M6 Phase 4 (Wave 2) — see field doc above */
    /* M6 Phase 6 (containers): VM-lifetime backing buffers for List/Dict
     * instances allocated via urbi_stdlib_register_containers.  Each
     * buffer begins with a (void *next) header that threads onto this
     * head pointer.  Freed in urbi_vm_destroy via
     * urbi_stdlib_containers_destroy.  See src/stdlib/containers.c. */
    void       *stdlib_containers;
    /* M6 Phase 6: Pair / Triplet / Tuple proto singletons.  Allocated
     * by urbi_stdlib_register_containers; bound to realm globals by
     * urbi_stdlib_register_container_globals after the registry loop.
     * NULL until first VM boot. */
    struct UObject *container_pair_proto;
    struct UObject *container_triplet_proto;
    struct UObject *container_tuple_proto;
    /* M6 Phase 7: Exception primitive proto.  Allocated by
     * urbi_stdlib_register_runtime_types; bound to "Exception" realm
     * global by urbi_stdlib_register_runtime_globals after the registry
     * loop.  NULL until first VM boot. */
    struct UObject *exception_proto;
    /* M6 Phase 8: namespace proto singletons.  T86 lands math_proto
     * (pi / e / nan / infinity); subsequent T87+T88+T90+T91 tasks add
     * system_proto / platform_proto / global_namespace_proto / call-
     * message_proto.  Allocated by urbi_stdlib_register_namespaces;
     * bound to realm globals by urbi_stdlib_register_namespace_globals
     * after the registry loop.  NULL until first VM boot.  GC
     * reachability via object_roots_walker. */
    struct UObject *math_proto;
    struct UObject *system_proto;
    struct UObject *platform_proto;
    struct UObject *global_namespace_proto;
    uint8_t     stdlib_booted;
    uint8_t     pad_stdlib[7];          /* padding; zeroed */
    UValue      last_recv;
} UVM;

/* --- API --- */

/* Initialize vm. On hosted builds, passing alloc_fn == NULL wires up a
   stdlib-realloc shim internally. On freestanding builds the caller MUST
   supply alloc_fn; if NULL is passed, urbi_vm_init still returns (cannot fail
   at M1), but any subsequent urbi_vm_run will NULL-deref in the frame
   allocation path — caller's bug. Zero-initializes last_error and
   last_errmsg. */
void urbi_vm_init(UVM *vm, UVMAllocFn alloc_fn, void *alloc_ud);

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
   them after each urbi_vm_run call without stale state from prior runs.

   API-004 (Wave 5): the `realm` argument selects which Realm the
   transient strand runs in.  realm == NULL falls back to the VM's
   global Realm (the pre-Wave-5 implicit behavior), preserving
   source-compat for existing callers via the matching update in the
   public header. */
UVMError urbi_vm_run(UVM *vm, struct URealm *realm,
                     const UModule *module, UValue *out);

/* Free any VM-owned resources. Safe to call on a zero-initialized UVM. */
void urbi_vm_destroy(UVM *vm);

/* Allocate vm->event_proto + vm->tag_proto and install their native slots.
 * Must be called after urbi_vm_init.  Separated from urbi_vm_init because unit tests
 * that check exact post-init cell / intern counts would break otherwise
 * (same lazy pattern as the atom-family singletons).
 * Safe to call multiple times — re-entrant calls are no-ops if protos already
 * allocated. */
void urbi_native_protos_init(UVM *vm);

/* Return a static string such as "UVM_TYPE_ERROR" for debug. */
const char *uvm_error_name(UVMError code);

/* --- Internal cross-module declarations ---
 * Originally in uvm_internal.h (consolidated post-M3). All src/ headers are
 * internal-by-definition after the include/urbi/ split — no separate friend
 * header needed. */

/* Heapify all open upvalue cells whose stack address is >= threshold.
 * Removed cells are appended to *closed_list.
 * Called by OP_CLOSE, OP_RET, and urbi_unwind. */
void vm_close_upvalues(struct UStrand *s, const UValue *threshold,
                       UUpvalCell **closed_list);

#ifdef __cplusplus
}
#endif

#endif /* UVM_H */
