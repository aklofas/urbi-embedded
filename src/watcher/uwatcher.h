/* SPDX-License-Identifier: BSD-3-Clause */
/* UWatcher: reactive watcher record + pool allocation.
 * Reactive runtime landed in M5 (see docs/milestones/m5-reactive.md).
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
#include "chunk/uchunk.h"   /* UValue, UClosure */

#ifdef __cplusplus
extern "C" {
#endif

/* === Forward declarations ===
 * Keep this header dependency-minimal: forward-decl rather than include. */
struct UVM;
struct UTag;
struct URealm;
struct UStrand;
struct UEvent;   /* defined in event/uevent.h; used only as pointer here */

/* === Pool size build flags === */

#ifndef URBI_WATCHER_POOL_SIZE
#  define URBI_WATCHER_POOL_SIZE  64
#endif

#ifndef URBI_WATCHER_READSET_MAX
#  define URBI_WATCHER_READSET_MAX  16
#endif

/* === Watcher mode constants === */

#define UWATCHER_AT                 1   /* at (cond) body — edge-triggered */
#define UWATCHER_WHENEVER           2   /* whenever (cond) body — level-triggered */
#define UWATCHER_AT_SYNC            3   /* at (cond) body synchronous variant */
#define UWATCHER_WAITUNTIL          4   /* waituntil(cond) — blocks caller until edge (spec #2 §5.1) */
#define UWATCHER_AT_EVENT           5   /* at (event) body — fires on Event.emit (spec #3 §3.2) */
#define UWATCHER_AT_EVENT_SYNC      6   /* at (event) body synchronous variant (spec #3 §3.2) */
#define UWATCHER_WHENEVER_EVENT     7   /* whenever (e?) body — perpetual event subscriber;
                                         * body re-fires on every emission, no one-shot
                                         * teardown.  W0/v0.10.2.  Closes reactive F1. */

/* === Mode predicates (refactor-3 SCHED-16) ===
 *
 * The mode lists for "is this an event watcher?" were duplicated 5+ times
 * and drifted: UWATCHER_WHENEVER_EVENT was missing from the
 * urbi_watcher_unregister_internal unlink branch and from the
 * uwatcher_pool_destroy slab walk.  All mode-class tests MUST go through
 * these two predicates.
 *
 * UWATCHER_IS_EVENT_MODE: true for the three event modes (5, 6, 7).
 *   These watchers thread on event->at_watchers_head via next_in_event
 *   and do NOT join vm->active_watchers_head.
 *
 * UWATCHER_IS_COND_MODE: true for the four condition modes (1, 2, 3, 4).
 *   These watchers thread on vm->active_watchers_head via next_active and
 *   carry a read-set.  IS_COND_MODE is the negation of IS_EVENT_MODE because
 *   the full mode space is exactly {1…7} — modes 1-4 cond, modes 5-7 event;
 *   there are no "neither" modes. */
#define UWATCHER_IS_EVENT_MODE(m)  ((m) == UWATCHER_AT_EVENT \
                                 || (m) == UWATCHER_AT_EVENT_SYNC \
                                 || (m) == UWATCHER_WHENEVER_EVENT)
#define UWATCHER_IS_COND_MODE(m)   (!UWATCHER_IS_EVENT_MODE(m))

/* === Watcher flag bits (stored in UWatcher.flags) === */

#define URBI_WATCHER_ACTIVE                    0x01U  /* installed and live */
#define URBI_WATCHER_PENDING_UNREGISTER        0x02U  /* stop requested; drain before free */
#define URBI_WATCHER_FIRED_DURING_EVAL         0x04U  /* condition fired while eval in progress */
/* 0x08U — reserved (formerly URBI_WATCHER_PENDING_REFIRE; replaced by the
 * pending_refire_count + max_refire_queue uint8_t fields below — see the
 * URBI_WATCHER_REFIRE_QUEUE_DEFAULT discussion).  Available for reuse. */
#define URBI_WATCHER_BODY_FIRED_SINCE_ONLEAVE  0x10U  /* body fired at least once since last onleave check (spec #2 §5.1) */
/* 0x20U, 0x40U, 0x80U — formerly URBI_WATCHER_OWNS_COND/_BODY/_ONLEAVE.
 * Deleted at v0.8.4 Step C-3: UClosure lifetime is GC-managed; watcher
 * install no longer needs to take manual ownership of closures. */

/* === Exhaust-policy constants (M5 dispatch; field present at M3) === */

#define URBI_EXHAUST_QUEUE  0   /* queue firings (default) */
#define URBI_EXHAUST_DROP   1   /* drop if body is still running */

/* URBI_WATCHER_REFIRE_QUEUE_DEFAULT: per-watcher max pending refires under
 * URBI_EXHAUST_QUEUE policy.  Each event arriving while a body is in flight
 * increments pending_refire_count up to this cap; firings beyond the cap
 * are silently dropped (same end-state as URBI_EXHAUST_DROP for the excess
 * events).  At body completion, if the counter is positive, ONE respawn
 * happens and the counter is decremented; this drains the queue at the
 * pace of body execution.
 *
 * Default 15 matches URBI_EVENT_RING_DEPTH-1: a full SPSC ring's worth of
 * pending events can be drained in one urbi_step pass without any loss
 * under typical eye_demo-shaped loads (5-25 Hz emit, ~100µs bodies).
 *
 * Tuning guidance:
 *   - Fast bodies, bursty events (camera frames, sensor reads): keep 15.
 *   - Slow bodies (>10 ms), bursty events: lower (e.g. 3) to bound latency.
 *   - Bodies you genuinely want to throttle: use URBI_EXHAUST_DROP instead.
 *
 * Compile-time override only at present.  A per-watcher runtime API is
 * tracked as a backlog item (urbi_watcher_set_max_refire_queue). */
#ifndef URBI_WATCHER_REFIRE_QUEUE_DEFAULT
#  define URBI_WATCHER_REFIRE_QUEUE_DEFAULT  15
#endif

/* === UWatcher struct (spec §5.1) ===
 *
 * Exact layout per pre-M3 tag-lifecycle-and-watcher-dirty-set-design.md §5.1.
 *
 * Size at default build (URBI_WATCHER_READSET_MAX=16):
 *   Header fields  : 16 B  (type_tag + gc_byte + mode + exhaust_policy +
 *                           flags + read_set_count + pending_refire_count +
 *                           max_refire_queue + eval_pass_gen [1 B] +
 *                           7 B implicit alignment padding)
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
 *   Fixed portion  : 120 B
 *   Total          : 120 + 128 = 248 B
 *
 * At footprint preset (URBI_WATCHER_READSET_MAX=4): 120 + 32 = 152 B.
 * The fixed array (not flexible member) means sizeof(UWatcher) depends
 * on the macro — intended, per §5.1 size-budget table. */

typedef struct UWatcher {
    /* === Common cell header (matches UCell layout; type_tag + gc_byte) === */
    uint8_t   type_tag;                    /* 1 B  UTYPE_WATCHER */
    uint8_t   gc_byte;                     /* 1 B  UGC_IS_FIXED set; color bits as usual */

    /* === Watcher-private state === */
    uint8_t   mode;                        /* 1 B  UWATCHER_AT / _WHENEVER / _AT_SYNC / _WAITUNTIL / _AT_EVENT / _AT_EVENT_SYNC */
    uint8_t   exhaust_policy;              /* 1 B  URBI_EXHAUST_QUEUE / _DROP (M5 dispatch) */
    uint8_t   flags;                       /* 1 B  URBI_WATCHER_ACTIVE / _PENDING_UNREGISTER / _FIRED_DURING_EVAL */
    uint8_t   read_set_count;              /* 1 B  number of valid entries in cells[] */
    uint8_t   pending_refire_count;        /* 1 B  events queued while body in flight (0..max_refire_queue); see URBI_WATCHER_REFIRE_QUEUE_DEFAULT */
    uint8_t   max_refire_queue;            /* 1 B  cap for pending_refire_count; init to URBI_WATCHER_REFIRE_QUEUE_DEFAULT */
    uint8_t   eval_pass_gen;               /* 1 B  eval-pass generation stamp; matched against
                                             * vm->watchers->eval_pass_gen to skip re-visits on
                                             * rescan after a PENDING cascade.  Zero-init = never
                                             * evaluated.  7 B implicit alignment padding follow
                                             * before next_active on 64-bit. */

    /* === Linked-list threading === */
    /* next_active doubles as the threading link for two mutually exclusive
     * lists, depending on watcher state:
     *   - Live cond watchers (AT/WHENEVER/AT_SYNC/WAITUNTIL, no
     *     PENDING_UNREGISTER): link in vm->active_watchers_head.
     *     AT_EVENT/AT_EVENT_SYNC do not use this field while live — they
     *     thread on event->at_watchers_head via next_in_event below.
     *   - Drained watchers (PENDING_UNREGISTER set, awaiting onleave):
     *     link in vm->pending_onleave_head FIFO.
     * pending_onleave_queue_push transfers from the active chain to the
     * pending FIFO (same field re-used as queue link).  A watcher is never
     * on both lists simultaneously. */
    struct UWatcher *next_active;          /* 8 B  active or pending-onleave list link (mutually exclusive) */
    struct UWatcher *next_in_tag;          /* 8 B  owning_tag->member_watchers_head chain */

    /* === Identity + closures === */
    struct UTag  *owning_tag;             /* 8 B  pin to tag scope */
    UClosure     *condition;              /* 8 B  evaluated each urbi_vm_watcher_eval_dirty */
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
URBI_STATIC_ASSERT(sizeof(UWatcher) == 248,
               "UWatcher size pin on 64-bit (URBI_WATCHER_READSET_MAX=16; +8 B for eval_pass_gen + alignment padding)");
#endif

/* === Pool lifecycle === */

/* uwatcher_pool_init: allocate the watcher slab and thread the freelist.
 * Must be called after urbi_gc_init in urbi_vm_init.
 * Returns 0 on success, -1 on OOM. */
int  uwatcher_pool_init(struct UVM *vm);

/* uwatcher_pool_destroy: free the watcher slab.
 * Must be called before urbi_gc_destroy in urbi_vm_destroy. */
void uwatcher_pool_destroy(struct UVM *vm);

/* uwatcher_pool_alloc: pop one watcher entry from the pool freelist.
 * Initialises type_tag, gc_byte, flags, and read_set_count to safe defaults.
 * Returns NULL when the pool is exhausted.
 * Not ISR-safe.  Caller is responsible for wiring all semantic fields and
 * inserting the result into the active and tag member lists. */
struct UWatcher *uwatcher_pool_alloc(struct UVM *vm);

/* === Unregister (C-internal; not in public urbi headers) ===
 *
 * urbi_watcher_unregister_internal:
 *   - clear bit-6 on cells no other watcher observes.
 *   - unlink from active_watchers_head + owning_tag->member_watchers_head.
 *   - return slot to pool.
 *
 * Production install entry points live in src/watcher/uwatcher_install.c
 * (`install_watcher_runtime`, `install_at_event_runtime`); both inline their
 * own pool-alloc + list-wiring sequence.  Unit tests that exercise the pool
 * primitive directly use `urbi_watcher_install_for_test`
 * (tests/unit/twatcher_install_helper.{c,h}); WATCH-023 retired the former
 * `urbi_watcher_install_internal` test seam from this header. */

void urbi_watcher_unregister_internal(struct UVM *vm, struct UWatcher *w);

/* === Eval pass === */

/* invoke_condition_closure: evaluate w->condition on the VM scratch frame.
 * Routes to `vm->test_hooks->watcher_condition` if set (existing fire-path
 * tests inject specific values); otherwise dispatches real bytecode via
 * `urbi_run_closure_on_scratch` (src/runtime/uscratch.c).  Eval-time throws
 * fail-soft as nil — the watcher does not fire this pass and the caller
 * (urbi_vm_watcher_eval_dirty, which is void) cannot propagate.  Returns nil when
 * `w->condition == NULL` (no-condition watchers fire on dirty-mark only).
 * Per spec §6.4. */
UValue invoke_condition_closure(struct UVM *vm, struct UWatcher *w);

/* urbi_vm_watcher_eval_dirty: walk active_watchers_head, evaluate each condition, and
 * call spawn_body_coroutine on edge (AT/AT_SYNC) or level (WHENEVER) fire.
 * Called from the safepoint when vm->watchers->dirty_count > 0. Per spec §6.2. */
void   urbi_vm_watcher_eval_dirty(struct UVM *vm);

/* === Pending-onleave queue ===
 *
 * pending_onleave_queue_push: transfer watcher from active lists to the FIFO.
 *   Sets URBI_WATCHER_PENDING_UNREGISTER, unlinks from active_watchers_head and
 *   owning_tag->member_watchers_head, appends to pending_onleave_queue tail.
 *   Called from OP_POP_TAG (uvm.c) and urbi_tag_stop (uunwind.c) cascade sites.
 *
 * drain_pending_onleave_queue: drain the FIFO in FIFO order, running onleave
 *   handlers and calling urbi_watcher_unregister_internal for each entry.
 *   Called from the dispatcher safepoint BEFORE urbi_vm_watcher_eval_dirty. */
void pending_onleave_queue_push(struct UVM *vm, struct UWatcher *w);
void drain_pending_onleave_queue(struct UVM *vm);

/* === Body spawn (uwatcher_spawn.c) === */

/* do_spawn_body_coroutine: spec #1 §5.3 steps 1-6.
 *   Step 1 is the exhaust-policy gate (URBI_EXHAUST_QUEUE / _DROP).
 *   Steps 2-6 allocate the body strand via urbi_strand_create, attach
 *   owning_tag when distinct from realm->tag, arm via urbi_strand_arm_from_closure,
 *   wire back-pointers, and enqueue via urbi_strand_start.  Three OOM points
 *   (strand alloc / ambient overflow / stack alloc) all log URBI_LOG_WARN
 *   and tear down any partial state — watcher remains installed for future
 *   fires.  fire_context is NULL today; spec #2 wires patterns later. */
void   do_spawn_body_coroutine(struct UVM *vm, struct UWatcher *w,
                               const void *fire_context);

/* spawn_body_coroutine: eval-pass entry called by urbi_vm_watcher_eval_dirty.
 * Precondition: w->body != NULL (urbi_vm_watcher_eval_dirty only calls this when body
 * is set; body-less watchers use vm->test_hooks->watcher_fire directly in
 * eval).  In URBI_DEBUG builds, asserts: in_watcher_eval == 1, AT/WHENEVER
 * mode, ACTIVE, no PENDING_UNREGISTER, body and realm non-NULL. */
void   spawn_body_coroutine(struct UVM *vm, struct UWatcher *w);

/* respawn_body_coroutine: completion-path entry (spec #1 §5.2).
 * Called when body strand reaches DEAD and pending_refire_count > 0.
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
 *   - If PENDING_UNREGISTER: zeroes pending_refire_count and returns (no respawn).
 *   - Else if pending_refire_count > 0: decrements counter and calls respawn_body_coroutine. */
void urbi_watcher_body_completed(struct UVM *vm, struct UStrand *s);

/* === GC root provider (uwatcher_gc.c) === */

/* urbi_gc_watcher_table_walk_roots: registered via urbi_gc_register_root_provider at
 * urbi_vm_init.  Pool-wide walk (refactor-3 GC-05): every in-use slab slot
 * (URBI_WATCHER_ACTIVE set) yields closure + last_value_cache UValues to the
 * GC mark callback and shades owning_tag + event cells directly, regardless
 * of which list (if any) threads the slot.  Per spec §6.6 as amended.
 * Remaining deferral: read-set cells[] (v1.x backlog).
 * Note (spec #1 §7.1): body_strand and realm are NOT yielded — body strands
 * are reached via realm->strands_head; realms are host-allocated. */
void   urbi_gc_watcher_table_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx);

#ifdef URBI_DEBUG
/* urbi_watcher_check_invariants: URBI_DEBUG-only bidirectional pointer check.
 * Validates that every active watcher with body_strand != NULL has:
 *   1. body_strand->watcher_body_owner == w.
 *   2. body_strand on w->realm->strands_head.
 * Called at urbi_vm_watcher_eval_dirty entry.  spec #1 §7.2. */
void   urbi_watcher_check_invariants(struct UVM *vm);
#endif /* URBI_DEBUG */


#ifdef __cplusplus
}
#endif

#endif /* UWATCHER_H */
