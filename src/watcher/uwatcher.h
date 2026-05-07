/* SPDX-License-Identifier: BSD-3-Clause */
/* UWatcher: reactive watcher record + pool allocation.
 * Row 11 / T32.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * All allocation goes through vm->alloc_fn.
 * The watcher pool is UGC_IS_FIXED — the GC marks active watchers but
 * the pool slab is managed manually (alloc at init, free at destroy). */

#ifndef UWATCHER_H
#define UWATCHER_H

#include <stddef.h>    /* size_t */
#include <stdint.h>

#include "gc/ugc.h"       /* UCell, UTYPE_WATCHER */
#include "module/umodule.h"   /* UValue, UClosure */

#ifdef __cplusplus
extern "C" {
#endif

/* === Forward declarations ===
 * Keep this header dependency-minimal: forward-decl rather than include. */
struct UVM;
struct UTag;
struct URealm;
struct UStrand;
struct UEvent;   /* defined in T17; used only as pointer here */

/* === Pool size build flags === */

#ifndef URBI_WATCHER_POOL_SIZE
#  define URBI_WATCHER_POOL_SIZE  64
#endif

#ifndef URBI_WATCHER_READSET_MAX
#  define URBI_WATCHER_READSET_MAX  16
#endif

/* === Watcher mode constants === */

#define UWATCHER_AT             1   /* at (cond) body — edge-triggered */
#define UWATCHER_WHENEVER       2   /* whenever (cond) body — level-triggered */
#define UWATCHER_AT_SYNC        3   /* at (cond) body synchronous variant */
#define UWATCHER_WAITUNTIL      4   /* waituntil(cond) — blocks caller until edge (spec #2 §5.1) */
#define UWATCHER_AT_EVENT       5   /* at (event) body — fires on Event.emit (spec #3 §3.2) */
#define UWATCHER_AT_EVENT_SYNC  6   /* at (event) body synchronous variant (spec #3 §3.2) */

/* === Watcher flag bits (stored in UWatcher.flags) === */

#define URBI_WATCHER_ACTIVE                    0x01u  /* installed and live */
#define URBI_WATCHER_PENDING_UNREGISTER        0x02u  /* stop requested; drain before free */
#define URBI_WATCHER_FIRED_DURING_EVAL         0x04u  /* condition fired while eval in progress */
#define URBI_WATCHER_PENDING_REFIRE            0x08u  /* fire arrived while body running; re-spawn at completion (spec #1 §3.2) */
#define URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE  0x10u  /* body fired at least once since last onleave check (spec #2 §5.1) */
/* Closure-ownership bits: set by install_watcher_runtime / install_at_event_runtime when a
 * heap closure was unlinked from strand.closure_list.  pool_free frees each owned closure.
 * Only set when strand_closure_unlink confirms the closure was on the heap list. */
#define URBI_WATCHER_OWNS_COND                 0x20u  /* condition was unlinked from closure_list; pool_free must free it */
#define URBI_WATCHER_OWNS_BODY                 0x40u  /* body was unlinked; pool_free must free it */
#define URBI_WATCHER_OWNS_ONLEAVE              0x80u  /* onleave was unlinked; pool_free must free it */

/* === Exhaust-policy constants (M5 dispatch; field present at M3) === */

#define URBI_EXHAUST_QUEUE  0   /* queue firings (default) */
#define URBI_EXHAUST_DROP   1   /* drop if body is still running */

/* === UWatcher struct (spec §5.1) ===
 *
 * Exact layout per pre-M3 tag-lifecycle-and-watcher-dirty-set-design.md §5.1.
 *
 * Size at default build (URBI_WATCHER_READSET_MAX=16):
 *   Header fields  : 8 B  (type_tag + gc_byte + mode + exhaust_policy +
 *                          flags + read_set_count + pad0)
 *   next_active    : 8 B
 *   next_in_tag    : 8 B
 *   owning_tag     : 8 B
 *   condition      : 8 B
 *   body           : 8 B
 *   onleave        : 8 B
 *   realm          : 8 B  (spec #1 §4.1)
 *   body_strand    : 8 B  (spec #1 §4.1)
 *   waiter_strand  : 8 B  (spec #2 §5.1)
 *   next_in_event  : 8 B  (spec #3 §3.2)
 *   event          : 8 B  (spec #3 §3.2)
 *   last_value_cache : 16 B  (UValue = kind(4)+pad(4)+union(8))
 *   cells[]        : 16 × 8 B = 128 B
 *   Fixed portion  : 112 B
 *   Total          : 112 + 128 = 240 B
 *
 * At footprint preset (URBI_WATCHER_READSET_MAX=4): 112 + 32 = 144 B.
 * The fixed array (not flexible member) means sizeof(UWatcher) depends
 * on the macro — intended, per §5.1 size-budget table. */

typedef struct UWatcher {
    /* === Common cell header (row 10 §3.1 lock) === */
    uint8_t   type_tag;                    /* 1 B  UTYPE_WATCHER */
    uint8_t   gc_byte;                     /* 1 B  UGC_IS_FIXED set; color bits as usual */

    /* === Watcher-private state === */
    uint8_t   mode;                        /* 1 B  UWATCHER_AT / _WHENEVER / _AT_SYNC / _WAITUNTIL / _AT_EVENT / _AT_EVENT_SYNC */
    uint8_t   exhaust_policy;              /* 1 B  URBI_EXHAUST_QUEUE / _DROP (M5 dispatch) */
    uint8_t   flags;                       /* 1 B  URBI_WATCHER_ACTIVE / _PENDING_UNREGISTER / _FIRED_DURING_EVAL */
    uint8_t   read_set_count;              /* 1 B  number of valid entries in cells[] */
    uint16_t  pad0;                        /* 2 B  align to 8 */

    /* === Linked-list threading === */
    struct UWatcher *next_active;          /* 8 B  vm->active_watchers_head chain */
    struct UWatcher *next_in_tag;          /* 8 B  owning_tag->member_watchers_head chain */

    /* === Identity + closures === */
    struct UTag  *owning_tag;             /* 8 B  pin to tag scope */
    UClosure     *condition;              /* 8 B  evaluated each watcher_eval_dirty */
    UClosure     *body;                   /* 8 B  spawned per fire (M5) */
    UClosure     *onleave;               /* 8 B  NULL if no onleave clause */

    /* === Body-spawn lifecycle anchors (spec #1 §4.1) === */
    struct URealm  *realm;              /* 8 B  owning realm; set at install, cleared at unregister */
    struct UStrand *body_strand;        /* 8 B  non-NULL while body coroutine runs; NULL otherwise */

    /* === waituntil parking (spec #2 §5.1) === */
    struct UStrand *waiter_strand;      /* 8 B  strand blocked on waituntil(cond); NULL for AT/WHENEVER */

    /* === Event-watcher threading (spec #3 §3.2) === */
    struct UWatcher *next_in_event;     /* 8 B  UEvent.at_watchers_head chain; NULL for cond watchers */
    struct UEvent   *event;             /* 8 B  back-pointer for O(1) unregister; NULL for cond watchers */

    /* === Edge detection === */
    UValue    last_value_cache;            /* 16 B  prior condition result */

    /* === Read-set (fixed array; size set by URBI_WATCHER_READSET_MAX) === */
    UCell    *cells[URBI_WATCHER_READSET_MAX]; /* 8 × cap B */
} UWatcher;

/* Layout pin (Wave-1 v0.5.3 audit CHSTR-041 + sibling): UWatcher size is
 * 240 B at the default URBI_WATCHER_READSET_MAX (16); any change to the
 * read-set cap or to the leading fields must update this assert
 * deliberately.  Guarded on pointer width to avoid a hard failure on
 * 32-bit cross targets, matching the UEvent / UObject pattern. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(UWatcher) == 240,
               "UWatcher size pin on 64-bit (URBI_WATCHER_READSET_MAX=16)");
#endif

/* === Pool lifecycle === */

/* uwatcher_pool_init: allocate the watcher slab and thread the freelist.
 * Must be called after urbi_gc_init in uvm_init.
 * Returns 0 on success, -1 on OOM. */
int  uwatcher_pool_init(struct UVM *vm);

/* uwatcher_pool_destroy: free the watcher slab.
 * Must be called before urbi_gc_destroy in uvm_destroy. */
void uwatcher_pool_destroy(struct UVM *vm);

/* uwatcher_pool_alloc: pop one watcher entry from the pool freelist.
 * Initialises type_tag, gc_byte, flags, and read_set_count to safe defaults.
 * Returns NULL when the pool is exhausted.
 * Not ISR-safe.  Caller is responsible for wiring all semantic fields and
 * inserting the result into the active and tag member lists. */
struct UWatcher *uwatcher_pool_alloc(struct UVM *vm);

/* === Install / unregister (C-internal; not in public urbi headers at M3) ===
 *
 * T32 ships minimal stubs:
 *   - install: allocates from pool + wires active_watchers_head.
 *   - unregister: unlinks from active_watchers_head + returns to pool.
 *
 * T33 completes:
 *   - read_set wiring (cells[] + bit-6 UGC_HAS_WATCHER_OBSERVER)
 *   - member_watchers_head insertion in owning_tag
 *
 * **Test-only seam:** `urbi_watcher_install_internal` is the low-level pool
 * + wiring path used by unit tests (`tests/unit/test_watcher_*.c` etc.).  It
 * does NOT run real bytecode dispatch on the condition closure — install-
 * time seeding short-circuits via `test_watcher_condition_hook` when set,
 * else seeds nil.  Production install goes through
 * `install_watcher_runtime` (uwatcher_install.c), which uses the real
 * scratch-frame helper.  Tests passing fake `(UClosure *)1` sentinels MUST
 * also set `test_watcher_condition_hook` before any subsequent eval, since
 * eval *does* dispatch real bytecode now (post-T8). */

struct UWatcher *urbi_watcher_install_internal(
    struct UVM       *vm,
    uint8_t           mode,
    struct UTag      *owning_tag,
    UClosure         *condition,
    UClosure         *body,
    UClosure         *onleave,
    UCell           **read_set,
    size_t            read_set_count);

void urbi_watcher_unregister_internal(struct UVM *vm, struct UWatcher *w);

/* === Eval pass (T34) === */

/* invoke_condition_closure: evaluate w->condition on the VM scratch frame.
 * Routes to `vm->test_watcher_condition_hook` if set (existing fire-path
 * tests inject specific values); otherwise dispatches real bytecode via
 * `urbi_run_closure_on_scratch` (uwatcher_scratch.c).  Eval-time throws
 * fail-soft as nil — the watcher does not fire this pass and the caller
 * (watcher_eval_dirty, which is void) cannot propagate.  Returns nil when
 * `w->condition == NULL` (no-condition watchers fire on dirty-mark only).
 * Per spec §6.4. */
UValue invoke_condition_closure(struct UVM *vm, struct UWatcher *w);

/* watcher_eval_dirty: walk active_watchers_head, evaluate each condition, and
 * call spawn_body_coroutine on edge (AT/AT_SYNC) or level (WHENEVER) fire.
 * Called from the safepoint when vm->watcher_dirty_count > 0. Per spec §6.2. */
void   watcher_eval_dirty(struct UVM *vm);

/* === Pending-onleave queue (T35) ===
 *
 * pending_onleave_queue_push: transfer watcher from active lists to the FIFO.
 *   Sets URBI_WATCHER_PENDING_UNREGISTER, unlinks from active_watchers_head and
 *   owning_tag->member_watchers_head, appends to pending_onleave_queue tail.
 *   Called from OP_POP_TAG (uvm.c) and urbi_tag_stop (uunwind.c) cascade sites.
 *
 * drain_pending_onleave_queue: drain the FIFO in FIFO order, running onleave
 *   handlers and calling urbi_watcher_unregister_internal for each entry.
 *   Called from the dispatcher safepoint BEFORE watcher_eval_dirty. */
void pending_onleave_queue_push(struct UVM *vm, struct UWatcher *w);
void drain_pending_onleave_queue(struct UVM *vm);

/* === Body spawn (uwatcher_spawn.c) === */

/* do_spawn_body_coroutine: M5 real implementation (spec #1 §5.3 steps 2-6).
 *   Allocates body strand via urbi_strand_create, attaches owning_tag when
 *   distinct from realm->tag, arms via urbi_strand_arm_from_closure, wires
 *   back-pointers, and enqueues via urbi_strand_start.  Three OOM points
 *   (strand alloc / ambient overflow / stack alloc) all log URBI_LOG_WARN and
 *   tear down any partial state — watcher remains installed for future fires.
 *   fire_context is NULL at M5 baseline; spec #2 wires patterns later.
 *   Exhaust gate arrives in T26. */
void   do_spawn_body_coroutine(struct UVM *vm, struct UWatcher *w,
                               void *fire_context);

/* spawn_body_coroutine: eval-pass entry called by watcher_eval_dirty.
 * Precondition: w->body != NULL (watcher_eval_dirty only calls this when body
 * is set; body-less watchers use test_watcher_fire_hook directly in eval).
 * In URBI_DEBUG builds, asserts: in_watcher_eval == 1, AT/WHENEVER mode,
 * ACTIVE, no PENDING_UNREGISTER, body and realm non-NULL. */
void   spawn_body_coroutine(struct UVM *vm, struct UWatcher *w);

/* respawn_body_coroutine: completion-path entry (spec #1 §5.2).
 * Called when body strand reaches DEAD and PENDING_REFIRE is set.
 * No in_watcher_eval assert (runs outside eval at strand-DEAD notification).
 * Not in the public include/urbi/ API; declared here for internal callers
 * and test code (reachable via this header or an explicit extern declaration). */
void   respawn_body_coroutine(struct UVM *vm, struct UWatcher *w);

/* === Completion callback (uwatcher_spawn.c) === */

/* urbi_watcher_body_completed: called by the dispatcher's strand-DEAD path
 * when a watcher body strand finishes execution (spec #1 §6.2).
 *   - Recovers w from s->watcher_body_owner (DEBUG-asserts non-NULL).
 *   - Logs URBI_LOG_WARN on UEXEC_THROW; silent for TAG_STOP/CANCEL/OK.
 *   - Clears both s->watcher_body_owner and w->body_strand atomically.
 *   - If PENDING_UNREGISTER: clears PENDING_REFIRE and returns (no respawn).
 *   - Else if PENDING_REFIRE: clears flag and calls respawn_body_coroutine. */
void urbi_watcher_body_completed(struct UVM *vm, struct UStrand *s);

/* === GC root provider (uwatcher_gc.c) === */

/* watcher_table_walk_roots: registered via urbi_gc_register_root_provider at
 * uvm_init.  Walks vm->active_watchers_head + vm->pending_onleave_head, yielding
 * closure + last_value_cache UValues to the GC mark callback.  Per spec §6.6.
 * M3 deferrals: owning_tag (UVAL_TAG kind doesn't exist until M5/M6) and
 * read-set cells[] (concrete cell types land at M4).
 * Note (spec #1 §7.1): body_strand and realm are NOT yielded — body strands
 * are reached via realm->strands_head; realms are host-allocated. */
void   watcher_table_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx);

#ifdef URBI_DEBUG
/* urbi_watcher_check_invariants: URBI_DEBUG-only bidirectional pointer check.
 * Validates that every active watcher with body_strand != NULL has:
 *   1. body_strand->watcher_body_owner == w.
 *   2. body_strand on w->realm->strands_head.
 * Called at watcher_eval_dirty entry.  spec #1 §7.2. */
void   urbi_watcher_check_invariants(struct UVM *vm);
#endif /* URBI_DEBUG */

/* Default 4096 dispatch ops — generous for typical conds (`x > 5`,
 * `obj.slot != nil`).  Override at compile time for footprint targets:
 *   -DURBI_SCRATCH_BUDGET_OPS=512
 * Conds that exhaust the budget log a warn, set out_threw=1, and return 0
 * (caller treats as cond-throw → install fails or eval skips fire). */
#ifndef URBI_SCRATCH_BUDGET_OPS
#  define URBI_SCRATCH_BUDGET_OPS 4096
#endif

/* === urbi_run_closure_on_scratch (spec #2 §6.4 + §7.3 phase 3) ===
 *
 * Run `closure` to OP_RET on a transient scratch-frame strand and capture
 * the return value.  Used by:
 *   - run_closure_on_scratch_frame_with_result (install-time cond eval)
 *   - invoke_condition_closure                  (eval-time cond)
 *
 * The transient strand is allocated on the C stack (mirroring uvm_run's
 * pattern), threaded onto vm->global_realm->strands_head for the duration
 * of the call so the GC walker visits its register window, then unlinked
 * and torn down before return.  Bounded by URBI_SCRATCH_BUDGET_OPS dispatch
 * ops; cond closures must not OP_YIELD or block (spec §6.4 no-yield contract
 * — yield/block trips a debug-mode assertion and degrades to nil + warn).
 *
 * `closure` may be NULL: returns 0 immediately with *out_result=UVAL_NIL,
 * *out_threw=0 (matches the prior stub contract for watchers installed
 * without a condition).
 *
 * Return value: 0 on clean OP_RET, NULL closure, budget exhaustion, or
 * cond throw; -1 only on register-stack OOM (transient setup fail).
 * *out_result holds the OP_RET value (UVAL_NIL on OOM, NULL closure,
 * cond throw, or budget exhaustion).  *out_threw is set to 1 on unhandled
 * THROW / TAG_STOP unwind or budget exhaustion, 0 otherwise.  Callers must
 * pass non-NULL out pointers. */
int urbi_run_closure_on_scratch(struct UVM      *vm,
                                struct UClosure *closure,
                                UValue          *out_result,
                                int             *out_threw);

/* === urbi_run_closure_on_scratch_with_payload (spec #3 §5.3) ===
 *
 * Same as urbi_run_closure_on_scratch but writes `payload` into the
 * closure's R[0] before dispatch.  Used by event sync-emit subscribers
 * (uevent_emit.c) — AT_EVENT_SYNC bodies receive the emit payload as
 * their first argument.
 *
 * NULL closure handled identically to the no-payload variant
 * (returns 0, *out_result = nil, *out_threw = 0).
 *
 * Same return-value semantics: 0 on clean OP_RET / NULL closure /
 * budget exhaustion / throw; -1 only on register-stack OOM. */
int urbi_run_closure_on_scratch_with_payload(struct UVM      *vm,
                                             struct UClosure *closure,
                                             UValue           payload,
                                             UValue          *out_result,
                                             int             *out_threw);

#ifdef __cplusplus
}
#endif

#endif /* UWATCHER_H */
