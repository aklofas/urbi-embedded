/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode interpreter. Freestanding. */

#ifndef UVM_H
#define UVM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chunk/uchunk.h"  /* UProto (UModule deleted v0.9.2), UValue, UValKind, UOpcode */
#include "value/uvalue.h"   /* UValue — needed for handle_table field */
#include "runtime/uframe.h"   /* UCallFrame, UUpvalCell, UVM_MAX_FRAMES, UVM_STACK_CAP */
#include "urbi/gc.h" /* UCell (opaque), UGcRootCallback/ProviderFn, non-inline ops */
/* W2: urbi/gc.h no longer includes the strategy header; pull it here so
 * internal callers that reach gc state via uvm.h still get the inline
 * barrier helpers (urbi_gc_slot_store, urbi_gc_register_write, etc.) and
 * the UGC_* bit-flag macros without an additional explicit include. */
#include "gc/ugc_incremental.h" /* inline barriers + UGC_* flags + UCell full layout */

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
struct UPeriodic;    /* v0.9.4 — defined in src/stdlib/temporal.h */
struct UClosure;     /* v0.9.4 — defined in src/runtime/uclosure.h */
struct UEventRing;   /* T18 lands the definition; event_ring is a pointer */
struct UShape;       /* M4 — defined in src/object/ushape.h */
struct UChunkInstance;   /* M4 T30 — defined in src/object/uchunk_instance.h */

/* Gap B (v0.7.1): named-event registry — full type needed in UVM struct. */
#include "event/uevent_registry.h"

/* Gap J (v0.7.1): host-side watcher table — full type needed in UVM struct. */
#include "watcher/uwatcher_host.h"

/* === W2+W3/v0.10.4: substate struct headers (audit-1 F8) ===
 * Full types are required for the pointer fields in struct UVM below.
 * uwatcher_state.h is always compiled (src/watcher/).
 * urepl_state.h is in src/repl/ (compiled only with URBI_ENABLE_REPL=1);
 * the forward declaration below keeps uvm.h freestanding-safe for non-REPL
 * builds — callers that need the full UReplState type include it directly.
 * utest_hooks.h is in src/runtime/ (always compiled). */
#include "watcher/uwatcher_state.h"
struct UReplState;   /* forward-decl; full type in repl/urepl_state.h */
#include "runtime/utest_hooks.h"

/* --- M3 capacity macros --- */
/* Dead path — uvm.h always pulls urbi/gc.h.  Guard retained only to prevent
 * double-definition warnings if ugc_incremental.h is included standalone. */
#ifndef URBI_GC_INITIAL_THRESHOLD
#  define URBI_GC_INITIAL_THRESHOLD (16 * 1024)
#endif

#ifndef URBI_MAX_ROOT_PROVIDERS
#  define URBI_MAX_ROOT_PROVIDERS 12U       /* row 10 §5.1; bumped from 8→12 at Step C-1 */
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
 * GC rooting contract (W3/v0.10.2 — closes reactive audit F6):
 *   The (parent, key, new_value) triple IS walked by the GC root provider
 *   urbi_deferred_slot_changes_walk_roots, registered at urbi_vm_init.
 *   Under the cooperative scheduler this is correctness-preserving (no GC
 *   slice fires between defer-site and drain in well-formed runs); under
 *   future preemptive scheduling the walker becomes load-bearing and
 *   prevents UAF in the drain path.
 *
 *   Safepoint ordering (unchanged):
 *     defer-site (slot-write barrier inside a sync slot-change body)
 *       --> next safepoint
 *       --> urbi_drain_deferred_slot_changes (clears head..tail)
 *       --> watcher_eval_dirty
 *
 * Storage: heap-allocated ring buffer, one urbi_vm-init calloc, freed in
 * urbi_vm_destroy.  NOT GC-managed at any tier — the cell entries are
 * transient and the drain logic zeroes each slot after firing. */
typedef struct UDeferredSlotChange {
    struct UObject *parent;
    struct USymbol *key;
    UValue          new_value;
} UDeferredSlotChange;

/* --- Operator-overload IC (Gap #4, M6 Wave 3) ---
 *
 * Per-call-site inline cache for operator-method lookup.  When an arithmetic
 * or comparison opcode raises a type error and the lhs is a user object, the
 * fallback walks the proto chain to find the operator-named slot ("+", "-",
 * etc.).  This IC caches the result so the second and subsequent calls at the
 * same pc_offset skip the proto-chain walk.
 *
 * Sizing: URBI_OP_OVERLOAD_IC_SITES × URBI_OP_OVERLOAD_IC_ENTRIES_PER_SITE.
 * Defaults: 64 sites × 4 entries = 256 IC slots.  Embedded footprint preset
 * may halve these; they are independently tunable compile-time constants.
 * Allocated inline in the UVM struct (not heap-allocated) so the IC is always
 * available after urbi_vm_init without a separate alloc. */
#ifndef URBI_OP_OVERLOAD_IC_SITES
#define URBI_OP_OVERLOAD_IC_SITES          32U
#endif
#ifndef URBI_OP_OVERLOAD_IC_ENTRIES_PER_SITE
#define URBI_OP_OVERLOAD_IC_ENTRIES_PER_SITE 4U
#endif

typedef struct UOpOverloadICEntry {
    uint32_t         pc_offset;    /* call-site identifier (offset from proto base) */
    uint64_t         topology_gen; /* vm->topology_gen at fill time; stale on mismatch */
    struct USymbol  *op_name;      /* interned operator-name symbol */
    struct UObject  *holder;       /* proto-chain object owning the operator slot
                                      (refactor-3 GC-06: cache WHERE, not WHAT —
                                      the hit path re-reads holder->slots[slot_idx],
                                      so in-place overwrites and GC replacement of
                                      the closure are picked up without a gen bump;
                                      mirrors the slot UIC, see object/uic.h).
                                      Lifetime parity with UIC's recv_shapes/slots:
                                      proto-chain holders are realm-rooted for the
                                      life of the type — deliberate, see design-risks. */
    struct UShape   *holder_shape; /* holder->shape at fill; staleness key */
    uint16_t         slot_idx;     /* index into holder->slots[] */
    uint16_t         pad0;
    uint32_t         pad1;
} UOpOverloadICEntry;

typedef struct UOpOverloadIC {
    UOpOverloadICEntry entries[URBI_OP_OVERLOAD_IC_SITES]
                              [URBI_OP_OVERLOAD_IC_ENTRIES_PER_SITE];
    uint8_t  n[URBI_OP_OVERLOAD_IC_SITES];             /* live entries per site */
    uint8_t  cursor[URBI_OP_OVERLOAD_IC_SITES];        /* eviction cursor per site */
} UOpOverloadIC;
/* UVM holds a heap pointer to UOpOverloadIC to avoid inflating the per-UVM
 * stack footprint in tests that allocate `UVM vm;` on the C stack.  The IC
 * is heap-allocated at urbi_vm_init time and freed at urbi_vm_destroy. */

/* UNestedArrayNode: deleted at Task 11 (v0.8.1-uproto-root).
 * urbi_steal_repl_protos (which populated vm->stdlib_nested_arrays) was
 * deleted at Task 8a; vm->stdlib_protos and vm->stdlib_nested_arrays are
 * deleted at Task 11.  The whole-root_proto rescue path (vm->rescued_protos)
 * is the sole deferred-destroy mechanism from v0.8.1 onward. */

/* --- Gap P (v0.7.1): per-VM error ring buffer storage ---
 *
 * UErrorRingEntry: one ring slot — owns the string buffers so the
 *   urbi_error_info_t const char* pointers never dangle.
 * UErrorRing: capacity-4 ring.  Inline in UVM (~4 KB).
 *
 * Size note: 4 entries × (sizeof urbi_error_info_t + 3×256 B) ≈ 3.2 KB
 * on 64-bit hosts.  Tests that allocate `UVM vm;` on the stack add this
 * to the frame; safe on hosted (8 MB default stack) but FreeRTOS embedders
 * with tight task stacks should allocate UVM on the heap or a static region.
 *
 * urbi_error_info_t lives in <urbi/urbi.h> (public header); included
 * transitively through urbi/types.h → urbi/urbi.h when urbi/urbi.h is
 * included.  We forward-include it here to keep uvm.h self-contained. */
#include "urbi/urbi.h"  /* urbi_error_info_t (Gap P struct) */
#include "urbi/trace.h" /* UTraceState (URBI_TRACE-gated UVM field) */
#include "runtime/uperf.h" /* UPerfCounters (URBI_PERF_COUNTERS-gated UVM field) */
#include "runtime/umemdebug.h" /* UMemDebug (URBI_MEM_DEBUG-gated UVM field) */

#define URBI_ERROR_RING_DEPTH  4U
#define URBI_ERROR_STRING_BUF  256U

typedef struct {
    urbi_error_info_t info;
    char              message_buf    [URBI_ERROR_STRING_BUF];
    char              source_name_buf[URBI_ERROR_STRING_BUF];
    char              context_buf    [URBI_ERROR_STRING_BUF];
} UErrorRingEntry;

typedef struct {
    UErrorRingEntry entries[URBI_ERROR_RING_DEPTH];
    size_t          head;   /* next write slot index (mod URBI_ERROR_RING_DEPTH) */
    size_t          count;  /* 0..URBI_ERROR_RING_DEPTH; 0 == empty */
} UErrorRing;

/* --- Gap Q (v0.7.1): per-VM reference table storage ---
 *
 * URefSlot: one table slot — holds the pinned UValue, a generation counter
 *   (0..255; increments on each urbi_unref to invalidate old handles), and
 *   an in_use flag.
 * URefTable: growable heap-allocated array + free-list.  NULL slots == empty.
 *
 * Handle encoding: (slot_index << 8) | generation.  Slot 0 is permanently
 * reserved so URBI_REF_INVALID (== 0) can never be a valid handle.  Valid
 * handles always have (handle >> 8) >= 1.
 *
 * GC integration: ref_table_walk_roots is registered at urbi_vm_init via
 * urbi_gc_register_root_provider; it calls cb for every in_use slot value
 * so the GC marks those values live. */
#define URBI_REF_INDEX_BITS  24U
#define URBI_REF_GEN_BITS    8U
#define URBI_REF_INDEX_MASK  ((1U << URBI_REF_INDEX_BITS) - 1U)
#define URBI_REF_MAX_SLOTS   URBI_REF_INDEX_MASK

typedef struct {
    UValue  value;
    uint8_t generation;  /* 0..255; increments on each urbi_unref */
    uint8_t in_use;      /* 1 = slot is live; 0 = free */
    uint8_t pad[6];      /* free-list next-index in pad[0..2] (24-bit, 3 bytes);
                          * pad[3..5] reserved / alignment filler */
} URefSlot;

typedef struct {
    URefSlot *slots;          /* heap-allocated array; NULL until first urbi_ref */
    size_t    capacity;       /* number of allocated slots (includes slot 0 sentinel) */
    size_t    used;           /* number of live (in_use) slots */
    size_t    free_list_head; /* index of next free slot, or SIZE_MAX if none */
} URefTable;

/* --- VM state --- */

#define UVM_ERRMSG_CAP 128

/* Field order is intentional and load-bearing:
 *  - The early fields (alloc_fn, alloc_ud, last_error, last_errmsg) form the
 *    init-by-zero error-handling prefix used by urbi_vm_init's failure paths.
 *  - The M2/M3/M4/M5 sections cluster fields by lifecycle (M2 intern table,
 *    M4 prototype/atom singletons, M5 reactive runtime) so reviewing each
 *    milestone's contribution stays local to one block.
 *  - Six URBI_STATIC_ASSERT layout pins (in src/vm/uvm.c, guarded on
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
    /* v0.10.3 (W3): last_error is now plain int (was UVMError; UVMError is now
     * typedef int for source compat).  Values: URBI_OK (0), URBI_ERR_OOM (-3),
     * URBI_ERR_STRAND_FATAL (-2) — same as UVM_OK / UVM_OOM / UVM_TYPE_ERROR
     * shims in <urbi/types.h>. */
    int        last_error;
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

    /* === M4 T30 — UChunkInstance registry ===
     * Linked list head of every live UChunkInstance threaded via
     * UChunkInstance.next_in_vm.  Created at urbi_chunk_instance_create
     * time (no removal at v1.0 — the GC reaps both the cell and any chain
     * dangling references when the instance becomes unreachable; this
     * registry is consulted only by the determinism checksum which itself
     * runs at quiescent points where instance removal isn't observed). */
    struct UChunkInstance *module_instances_head;

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
    /* watcher_active_count moved to vm->watchers->active_count (W2/v0.10.4) */
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
    /* watcher_dirty_count moved to vm->watchers->dirty_count (W2/v0.10.4) */

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
     * NULL = no drain handler (ring entries are discarded).
     * v0.10.3 (W3): handler gains void *ud; event_drain_ud forwarded. */
    void (*event_drain_handler)(struct UVM *vm, void *ud, uint32_t event_id, UValue payload);
    void  *event_drain_ud;

    /* --- Row 10 GC state machine --- */
    uint8_t  gc_phase;                 /* 0 = IDLE per row 10 §6.2; named constant lands at T22 */
    uint8_t  current_white;            /* current white color for tri-color marking */
    uint8_t  gc_paused;                /* non-zero → GC slices suppressed */
    uint8_t  in_destroy_callback;      /* debug-build assertion guard (T22/T27 use) */
    uint8_t  gc_stress_armed;          /* URBI_GC_STRESS: 1 after urbi_vm_init completes;
                                        * urbi_gc_alloc force-collects BEFORE every
                                        * allocation while set (refactor-3 TEST-GAP-01).
                                        * Always present (1 B) so layout is identical
                                        * across stress/non-stress builds; only read
                                        * under #if URBI_GC_STRESS. */
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
    /* v0.11.1 always-on GC stats (cheap, O(GC-cycle); feed Debug.gc()). */
    size_t   gc_cycles;                /* completed collection cycles */
    size_t   gc_slices;                /* incremental slices run */
    uint64_t last_gc_us;               /* duration of the most-recent cycle (0 if no clock) */
    uint64_t total_gc_us;              /* cumulative cycle time (0 if no clock) */
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

    /* --- Row 11 watcher substate (T32 allocates pool slab) --- */
    /* === W2/v0.10.4: watcher substate (extracted per audit-1 F8) === */
    UWatcherState *watchers;           /* heap-allocated; NULL until urbi_vm_init */
    /* Linked list of live watchers — NOT in UWatcherState.
     * GC walker (watcher_table_walk_roots) and the pending-onleave drain
     * loop walk this on every safepoint; keeping it on UVM avoids one
     * pointer indirection per iteration.  W2/v0.10.4 deliberate retention,
     * audit-1 F8 partial. */
    struct UWatcher *active_watchers_head;

    /* --- spec #3 §7.1: currently-dispatching strand ---
     * Set to the running strand by urbi_step before dispatch_loop_until_yield,
     * cleared after.  Required by c_event_waituntil to locate the caller strand.
     * NULL when no strand is dispatching (between urbi_step slices). */
    struct UStrand *cur_strand;

    /* --- spec #2 §5.2 install-time trace state ---
     * in_watcher_install: moved to vm->watchers->in_install (W2/v0.10.4).
     * trace_overflow: set when trace_read_set[] is full and a new cell would
     *   have been recorded.  Install treats overflow as "untrackable — skip IC".
     * trace_read_set_count: number of valid UCell* entries written into
     *   trace_read_set[].  Reset to 0 at the start of each install evaluation.
     * trace_read_set[]: ring buffer of UCell pointers touched during install
     *   cond evaluation; written by OP_GETSLOT when in_watcher_install is set
     *   (vm->watchers->in_install).  Array is uninitialized storage; only
     *   indices [0, trace_read_set_count) are valid.  Sized by
     *   URBI_WATCHER_READSET_MAX. */
    uint8_t   trace_overflow;
    uint8_t   _pad_trace[3];          /* padding to 4-byte boundary so the
                                         following uint16_t trace_read_set_count
                                         is naturally aligned (and the
                                         subsequent pointer array starts at a
                                         pointer-aligned offset). */
    uint16_t  trace_read_set_count;
    struct UCell *trace_read_set[URBI_WATCHER_READSET_MAX];

    /* === W3/v0.10.4: substate pointers (extracted per audit-1 F8) ===
     * Watcher/install test seams previously lived as four inline function
     * pointer fields; they are now bundled in UTestHooks (runtime/utest_hooks.h).
     * Production callers NULL-check vm->test_hooks before dereferencing, matching
     * the pre-W3 pattern of checking each hook pointer individually.
     *
     * vm->test_hooks is allocated by utest_hooks_create at urbi_vm_init and is
     * non-NULL in all hosted builds.  Callers in src/watcher/ check
     * vm->test_hooks != NULL before any field access so freestanding builds
     * (where alloc_fn may be NULL) remain safe.
     *
     * vm->repl is allocated by urepl_state_create (src/repl/) when a REPL
     * server is started; it is NULL until then.  Callers check vm->repl != NULL
     * before dereferencing (matching the former vm->repl_server != NULL check). */
    struct UReplState  *repl;        /* was vm->repl_server (1 field) */
    struct UTestHooks  *test_hooks;  /* was vm->test_watcher_* + test_install_cond_hook */

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

    /* --- Row 9 host time hook ---
     * v0.10.3 (W3): host_time_us gains a ud parameter; host_time_ud forwarded. */
    uint64_t (*host_time_us)(void *ud);  /* returns monotonic microseconds; default set at init */
    void      *host_time_ud;

    /* --- Gap E pluggable I/O writer (v0.7.1) ---
     * writer_fn: channel-multiplexed write callback.  NULL = default writer
     *   (hosted: cout/clog→stdout, cerr→stderr, others discarded;
     *    freestanding: silent sink).
     * writer_ud: opaque user-data pointer forwarded to every writer_fn call.
     * Thread safety: MAIN. */
    void   (*writer_fn)(void *ud, const char *channel, size_t channel_len,
                        const char *msg, size_t msg_len, uint64_t ts_us);
    void    *writer_ud;

    /* --- Gap S wake notification hook (v0.7.1) ---
     * wake_fn: called after each successful urbi_inject_event ring deposit.
     *   May run in ISR context.  MUST be O(1), non-blocking, non-allocating.
     *   NULL = no wake signal (embedder polls urbi_step directly).
     * wake_ud: opaque user-data pointer forwarded to every wake_fn call.
     * Thread safety: ISR or MAIN. */
    void   (*wake_fn)(void *ud);
    void    *wake_ud;

    /* --- T19 ISR-check + debug watchdog hooks ---
     * isr_check_fn: returns true when called from ISR context; NULL = no check.
     *   In URBI_DEBUG builds, every non-ISR-safe function asserts isr_check_fn() == false.
     * host_log_fn: structured log callback; NULL = silent.  Called by debug-build
     *   watchdog when a host callback exceeds callback_warn_us.
     * callback_warn_us: watchdog threshold in microseconds (default URBI_CALLBACK_WARN_US).
     * callback_watchdog_mode: URBI_WATCHDOG_WARN (0) or URBI_WATCHDOG_ASSERT (1).
     * v0.10.3 (W3): isr_check_fn + host_log_fn gain void *ud; *_ud fields forwarded. */
    bool     (*isr_check_fn)(void *ud);
    void      *isr_check_ud;
    void     (*host_log_fn)(struct UVM *vm, void *ud, int level, const char *fmt, ...);
    void      *host_log_ud;
    uint32_t   callback_warn_us;
    uint8_t    callback_watchdog_mode;
    uint8_t    pad_watchdog[3];        /* padding; zeroed */

    /* === M6 Phase 3 stdlib state ===
     * stdlib_closures + stdlib_upvalues: deleted at v0.8.4 Step C-3.
     *   UClosure and UUpvalCell are GC-managed since Step C-2; no
     *   VM-level linked lists needed for lifetime.
     * stdlib_module: heap-allocated UModule deserialized from the baked
     *   urbi_stdlib_bytecode blob during urbi_stdlib_boot.  NULL when the
     *   blob is empty (Phase 4 baseline) or boot has not run.  Owned by
     *   the VM; freed via uchunk_destroy + alloc_fn at urbi_vm_destroy.
     * stdlib_booted: idempotency guard for urbi_stdlib_boot.  Set on first
     *   successful boot; subsequent calls are no-ops.
     * (last_recv removed at v1.6 S42 — method receivers are now passed
     *  via R[A+1] under OP_CALL's method flag, not a global side channel.) */
    /* stdlib_protos and stdlib_nested_arrays deleted at Task 11 (v0.8.1-uproto-root).
     * The whole-root_proto rescue path (vm->rescued_protos) is the sole mechanism. */
    /* rescued_protos: intrusive list (via UProto.next_alloc) of whole root_proto
     * objects rescued from uchunk_destroy when root_proto->refcount > 0 at
     * destroy time (Phase 2 Task 9 of v0.8.1-uproto-root).
     *
     * When a module is destroyed while a strand still holds a reference to its
     * root_proto, uchunk_destroy detaches the root_proto (with all nested[]
     * and chunk-top buffer ownership) and threads it onto this list.  The
     * module shell (source_name and the UModule struct itself) is freed normally.
     *
     * At urbi_vm_destroy, each rescued root_proto is freed via
     * uproto_destroy_buffers (walks nested[], frees all owned buffers),
     * then the root_proto struct itself is freed via its stored allocator.
     * Task 11: stdlib_protos (per-nested rescue) deleted; rescued_protos is
     * the sole deferred-destroy mechanism. */
    struct UProto      *rescued_protos;
    struct UProto  *stdlib_module;      /* M6 Phase 4 (Wave 2) — see field doc above; v0.9.2: was UModule* */
#ifdef URBI_ENABLE_UROBOTICS
    struct UProto  *urobotics_module;   /* v0.12.2: VM-owned Robotics overlay module (gated); freed at teardown like stdlib_module */
#endif
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
    /* Cached Exception-subclass protos for C raise sites (object_root.c,
     * uvm_slot.c).  Resolved by name from the global realm after the stdlib
     * bake-blob run (urbi_exception_subclass_protos_resolve), mirroring
     * channel_proto.  NULL until the first realm completes population. */
    struct UObject *typeerror_proto;
    struct UObject *arityerror_proto;
    struct UObject *lookuperror_proto;
    struct UObject *oomerror_proto;
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
    struct UObject *callmessage_proto;
    /* M6 Phase 9: primitive proto singletons.  T94 lands mutex_proto
     * (cooperative flag-flip); T95 date_proto (libc time() shim);
     * T96 duration_proto (thin wrapper over integer microseconds).
     * Allocated by urbi_stdlib_register_primitives; bound to realm
     * globals by urbi_stdlib_register_primitives_globals after the
     * registry loop.  NULL until first VM boot.  GC reachability via
     * object_roots_walker. */
    struct UObject *mutex_proto;
    struct UObject *date_proto;
    struct UObject *duration_proto;
    /* v1.0 stdlib-completeness: RegExp proto singleton.  Allocated by
     * urbi_stdlib_register_regexp; bound to realm globals by
     * urbi_stdlib_register_regexp_globals after the registry loop.  NULL
     * until first VM boot.  GC reachability via object_roots_walker. */
    struct UObject *regexp_proto;
    /* v0.9.1 Phase 5: Lobby proto singleton.  Allocated by
     * urbi_lobby_native_register (called from urbi_stdlib_boot AFTER
     * primitives so the proto exists before mark_readonly runs).  Bound
     * to realm globals by urbi_lobby_native_register_globals — slots
     * 15+, past the v1.0 packed-flag CONSTANT enforcement range.  The
     * proto carries the `__builtin_lobby_send` native method + a
     * `lobbies` list slot populated at lobby.u runtime.  GC
     * reachability via object_roots_walker. */
    struct UObject *lobby_proto;
    /* v0.10.10 / D7-A: Job proto singleton.  Allocated by
     * urbi_job_proto_register (called from urbi_stdlib_boot after
     * lobby_native_register).  Bound as realm-global "Job" by
     * urbi_job_proto_register_globals in the post-loop hook inside
     * urbi_populate_realm_globals.  GC reachability via
     * urbi_object_register_gc_roots (object_roots_walker). */
    struct UObject *job_proto;
    /* v0.10.11 / D6: Channel proto singleton (the Channel class object
     * baked from channel_overlay.u).  Resolved by
     * urbi_channel_proto_resolve after the first realm's bake-blob run
     * in urbi_populate_realm_globals; consumed by
     * urbi_channel_register_globals at every subsequent realm-create.
     * NULL until the first realm is populated.  GC reachability via
     * urbi_object_register_gc_roots (object_roots_walker). */
    struct UObject *channel_proto;
    /* v0.9.4 Phase 5: every() periodic-spawn primitive.
     * - every_native_closure: the C-native UClosure that script-side `every`
     *   resolves to.  Allocated by urbi_temporal_native_register (called
     *   from urbi_stdlib_boot); bound as a realm-global by
     *   urbi_temporal_native_register_globals.  GC reachability: the
     *   realm-global slot keeps it alive; periodic_table_walk_roots also
     *   yields it explicitly so it survives realms that haven't yet
     *   registered the global (defensive).
     * - periodics_head: singly-linked list of UPeriodic records (one per
     *   live every() call).  Walked by urbi_periodic_pump in urbi_step
     *   and urbi_periodic_table_walk_roots at GC mark.  See
     *   src/stdlib/temporal.h for record shape and lifecycle. */
    struct UClosure  *every_native_closure;
    struct UPeriodic *periodics_head;
    uint8_t     stdlib_booted;
    /* heap_locked (Phase 13 / T145): non-zero → urbi_gc_alloc declines
     * new allocations and returns NULL.  One-way latch set via the
     * public C API urbi_lock_heap; never cleared.  Reserved for v2.0
     * hard-RT mode where post-init allocation is forbidden.  Zero
     * default at urbi_vm_init time. */
    uint8_t     heap_locked;
    uint8_t     pad_stdlib[6];          /* padding; zeroed */
    /* Operator-overload IC (Gap #4, M6 Wave 3).  Heap-allocated at
     * urbi_vm_init time via vm->alloc_fn; freed at urbi_vm_destroy.
     * NULL until first allocation (urbi_vm_init ensures it is allocated).
     * Pointer to UOpOverloadIC keeps the UVM struct small so tests that
     * put `UVM vm;` on the C stack do not overflow. */
    UOpOverloadIC *op_overload_ic;

    /* T33 (v0.7.0 Wave 1): host-callback hook fired by
     * urbi_watcher_body_completed after internal cleanup, before any
     * re-spawn.  NULL default; installed via urbi_set_watcher_body_done_fn.
     * Declared as inline function-pointer to keep uvm.h independent of
     * <urbi/urbi.h> (which forward-declares UVM and would create a
     * circular include).  The public typedef urbi_watcher_body_done_fn
     * in <urbi/urbi.h> expands to a function pointer with the exact same
     * shape, so the setter wires through cleanly across the seam.
     * v0.10.3 (W3): gains void *ud; watcher_body_done_ud forwarded. */
    void (*watcher_body_done_fn)(struct UVM *vm, void *ud, int handle, int completion_status);
    void  *watcher_body_done_ud;

    /* --- Gap R: atomic event section state (v0.7.1) ---
     * atomic_active: true while urbi_atomic_begin has been called and
     *   urbi_atomic_end has not yet been called.  When true, uevent_ring_drain
     *   is a no-op so ISR-deposited events stay queued until urbi_atomic_end
     *   clears the flag and triggers a drain pass.
     * atomic_begin_us: monotonic timestamp (microseconds) captured at
     *   urbi_atomic_begin; used by the URBI_DEBUG watchdog in urbi_step to
     *   detect sections held beyond URBI_ATOMIC_MAX_US.  Only valid when
     *   atomic_active is true.
     * Both fields are zeroed by urbi_vm_init (part of the zero-init UVM). */
    uint8_t  atomic_active;
    uint8_t  pad_atomic[7];     /* alignment padding; zeroed */
    uint64_t atomic_begin_us;

    /* --- Gap B (v0.7.1): named-event registry ---
     * Maps urbi_event_id_t → (UEvent *, destruct_fn, name) triples.
     * Zero-initialized at urbi_vm_init.  Entries[] array heap-allocated
     * on first urbi_event_register call and freed at urbi_vm_destroy
     * via uevent_registry_destroy.
     *
     * GC note: UEvent cells in entries[i].event are GC-managed; the registry
     * holds a raw pointer that acts as an unrooted reference.  This is safe
     * because urbi_event_register also installs the event as a const
     * realm-global (strong root), keeping it alive for the VM lifetime.
     * A future GC root walker for the registry is deferred to v1.x if
     * urbi_event_unregister needs to support explicit removal. */
    UEventRegistry event_registry;

    /* --- Gap J (v0.7.1): host-side reactive watcher table ---
     * Growable array of UHostWatcher entries installed via
     * urbi_register_watcher.  Zero-initialized at urbi_vm_init.
     * Freed at urbi_vm_destroy via uhost_watcher_table_destroy.
     *
     * Walking and dispatch happen in uevent_ring_drain after the
     * script-side UEvent dispatch (c_event_emit_async) completes.
     * Single-threaded at v1.0 — no locking required. */
    UHostWatcherTable host_watcher_table;

    /* --- Gap P (v0.7.1): per-VM error ring buffer ---
     * Inline storage (~3.2 KB on 64-bit hosts).  See UErrorRing definition
     * above for the size note and stack-allocation warning.
     * Zero-initialized at urbi_vm_init (head=0, count=0 → empty ring).
     * No heap to free in urbi_vm_destroy (all storage is inline). */
    UErrorRing error_ring;

    /* --- Gap Q (v0.7.1): reference table ---
     * Heap-allocated URefSlot array; NULL until the first urbi_ref call.
     * Freed at urbi_vm_destroy.  GC roots walked by ref_table_walk_roots
     * (registered at urbi_vm_init). */
    URefTable ref_table;

    /* --- v0.9.1 Debug namespace proto (Task 22) ---
     * Lazily allocated by urbi_debug_namespace_register on first call;
     * stashed here so subsequent realm-global binds see the same singleton
     * and the GC root walker shades it once.  void* keeps the core VM
     * header free of an URBI_ENABLE_REPL conditional include cascade; the
     * debug_namespace TU casts back to UObject*. */
    void *debug_proto;

#ifdef URBI_ENABLE_ROS2
    /* v0.12.0: `ros` native namespace proto. void* keeps the core VM header
     * free of an URBI_ENABLE_ROS2 include cascade; uros.c casts to UObject*.
     * Allocated by urbi_ros_register at stdlib boot. */
    void *ros_proto;
#endif

#if URBI_TRACE
    /* v0.11.0 trace subsystem state; present ONLY in URBI_TRACE builds so the
     * OFF build keeps byte-identical UVM layout.  Heap-allocated (NOT embedded)
     * by urbi_trace_init so the multi-KB ring never lands on stack-allocated
     * UVMs — keeps sizeof(UVM) ~unchanged and avoids perturbing memory layout.
     * NULL until init (or if the init allocation fails ⇒ trace disabled). */
    UTraceState *trace;
#endif

#if URBI_PERF_COUNTERS
    UPerfCounters perf;   /* v0.11.1; ~160 B, embedded (small — unlike the trace ring) */
#endif

#if URBI_MEM_DEBUG
    struct UMemDebug *memdbg;   /* v0.11.3; lazy heap pointer, NULL until first alloc */
#endif
} UVM;

/* --- API --- */

/* Initialize vm. On hosted builds, passing alloc_fn == NULL wires up a
   stdlib-realloc shim internally. On freestanding builds the caller MUST
   supply alloc_fn; if NULL is passed, urbi_vm_init returns URBI_OK (no
   subsystem allocations attempt to run through the NULL alloc_fn), but
   any subsequent urbi_vm_run will NULL-deref in the frame allocation
   path — caller's bug. Zero-initializes last_error and last_errmsg.

   T23 (VM-010 + VM-024, v0.7.0 Wave 1) — promoted from void to int return.
   Returns URBI_OK on success, URBI_ERR_OOM if any sub-system allocation
   fails (event_ring, watcher pool, deferred_slot_changes ring, operator
   overload IC).  urbi_vm_destroy remains safe to call regardless of the
   return value; partial-init state is reaped by the destroy path. */
int urbi_vm_init(UVM *vm, UVMAllocFn alloc_fn, void *alloc_ud);

/* Strand-driven dispatch loop (T6).  Runs s's bytecode until one of:
   - strand reaches DEAD (top-level OP_RET or halt_error)
   - strand voluntarily yields via OP_YIELD (state → READY)
   - step_budget_in opcodes have been consumed (state remains RUNNING)
   Returns the number of opcodes consumed.  s->vm must be non-NULL.
   Caller must have initialised s->stack, s->R, s->pc, s->pc_base,
   s->cur_consts, s->root_proto, and s->state = USTRAND_STATE_RUNNING. */
uint64_t dispatch_loop_until_yield(struct UStrand *s, uint64_t step_budget_in);

/* Run module to completion. On URBI_OK (0), *out receives the RET value. On
   error, vm->last_error and vm->last_errmsg are populated and *out is
   set to UVAL_NIL (kind = UVAL_NIL, value payload zeroed).
   last_error and last_errmsg are reset at entry — a caller may inspect
   them after each urbi_vm_run call without stale state from prior runs.

   v0.10.3 (W3): return type changed from UVMError to int.  UVMError is now
   typedef int for source compat; existing callsites compile unchanged.

   API-004 (Wave 5): the `realm` argument selects which Realm the
   transient strand runs in.  realm == NULL falls back to the VM's
   global Realm (the pre-Wave-5 implicit behavior), preserving
   source-compat for existing callers via the matching update in the
   public header. */
int urbi_vm_run(UVM *vm, struct URealm *realm,
                const UProto *root, UValue *out);

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
/* v0.10.3 (W3): UVMError is now typedef int; uvm_error_name accepts int. */
const char *uvm_error_name(int code);

/* --- Internal cross-module declarations ---
 * Originally in uvm_internal.h (consolidated post-M3). All src/ headers are
 * internal-by-definition after the include/urbi/ split — no separate friend
 * header needed. */

/* Heapify all open upvalue cells whose stack address is >= threshold.
 * v0.8.4 Step C-3: closed_list parameter removed; UUpvalCell is GC-managed.
 * Called by OP_CLOSE, OP_RET, and urbi_unwind. */
void vm_close_upvalues(struct UStrand *s, const UValue *threshold);

#ifdef __cplusplus
}
#endif

#endif /* UVM_H */
