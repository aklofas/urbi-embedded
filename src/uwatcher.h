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

#include "ugc.h"       /* UCell, UTYPE_WATCHER */
#include "umodule.h"   /* UValue, UClosure */

#ifdef __cplusplus
extern "C" {
#endif

/* === Forward declarations ===
 * Keep this header dependency-minimal: forward-decl rather than include. */
struct UVM;
struct UTag;

/* === Pool size build flags === */

#ifndef URBI_WATCHER_POOL_SIZE
#  define URBI_WATCHER_POOL_SIZE  64
#endif

#ifndef URBI_WATCHER_READSET_MAX
#  define URBI_WATCHER_READSET_MAX  16
#endif

/* === Watcher mode constants === */

#define UWATCHER_AT       1   /* at (cond) body — edge-triggered */
#define UWATCHER_WHENEVER 2   /* whenever (cond) body — level-triggered */
#define UWATCHER_AT_SYNC  3   /* at (cond) body synchronous variant */

/* === Watcher flag bits (stored in UWatcher.flags) === */

#define URBI_WATCHER_ACTIVE              0x01u  /* installed and live */
#define URBI_WATCHER_PENDING_UNREGISTER  0x02u  /* stop requested; drain before free */
#define URBI_WATCHER_FIRED_DURING_EVAL   0x04u  /* condition fired while eval in progress */

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
 *   last_value_cache : 16 B  (UValue = kind(4)+pad(4)+union(8))
 *   cells[]        : 16 × 8 B = 128 B
 *   Fixed portion  : 72 B
 *   Total          : 72 + 128 = 200 B + natural padding ≈ 208 B
 *
 * At footprint preset (URBI_WATCHER_READSET_MAX=4): 72 + 32 = 104 B.
 * The fixed array (not flexible member) means sizeof(UWatcher) depends
 * on the macro — intended, per §5.1 size-budget table. */

typedef struct UWatcher {
    /* === Common cell header (row 10 §3.1 lock) === */
    uint8_t   type_tag;                    /* 1 B  UTYPE_WATCHER */
    uint8_t   gc_byte;                     /* 1 B  UGC_IS_FIXED set; color bits as usual */

    /* === Watcher-private state === */
    uint8_t   mode;                        /* 1 B  UWATCHER_AT / _WHENEVER / _AT_SYNC */
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

    /* === Edge detection === */
    UValue    last_value_cache;            /* 16 B  prior condition result */

    /* === Read-set (fixed array; size set by URBI_WATCHER_READSET_MAX) === */
    UCell    *cells[URBI_WATCHER_READSET_MAX]; /* 8 × cap B */
} UWatcher;

/* === Pool lifecycle === */

/* uwatcher_pool_init: allocate the watcher slab and thread the freelist.
 * Must be called after urbi_gc_init in uvm_init.
 * Returns 0 on success, -1 on OOM. */
int  uwatcher_pool_init(struct UVM *vm);

/* uwatcher_pool_destroy: free the watcher slab.
 * Must be called before urbi_gc_destroy in uvm_destroy. */
void uwatcher_pool_destroy(struct UVM *vm);

/* === Install / unregister (C-internal; not in public urbi headers at M3) ===
 *
 * T32 ships minimal stubs:
 *   - install: allocates from pool + wires active_watchers_head.
 *   - unregister: unlinks from active_watchers_head + returns to pool.
 *
 * T33 completes:
 *   - read_set wiring (cells[] + bit-6 UGC_HAS_WATCHER_OBSERVER)
 *   - member_watchers_head insertion in owning_tag */

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
 * At M3, uses vm->test_watcher_condition_hook if non-NULL; otherwise returns
 * UVAL_NIL (graceful degradation — M5 wires real bytecode execution here).
 * Per spec §6.4. */
UValue invoke_condition_closure(struct UVM *vm, struct UWatcher *w);

/* watcher_eval_dirty: walk active_watchers_head, evaluate each condition, and
 * call spawn_body_coroutine on edge (AT/AT_SYNC) or level (WHENEVER) fire.
 * Called from the safepoint when vm->watcher_dirty_count > 0. Per spec §6.2. */
void   watcher_eval_dirty(struct UVM *vm);

/* === Body spawn — T36 owns real impl; T34 ships minimal stub for linker === */

/* spawn_body_coroutine: called by watcher_eval_dirty when a watcher fires.
 * M3 stub: invokes vm->test_watcher_fire_hook if non-NULL; otherwise no-op.
 * M5 implementation: pool-alloc body strand, inherit ambient tag chain,
 * bind w->body as entry closure, call urbi_strand_start. Per spec §6.8. */
void   spawn_body_coroutine(struct UVM *vm, struct UWatcher *w);

#ifdef __cplusplus
}
#endif

#endif /* UWATCHER_H */
