/* SPDX-License-Identifier: BSD-3-Clause */
/* src/runtime/umemdebug.h — on-target memory-debug tooling (v0.11.3).
 *
 * Gated by URBI_MEM_DEBUG: undefined => zero bytes (no fields, no code, no
 * symbols), every URBI_MEM_* macro is a no-op.  Metadata lives on the existing
 * UAllCellsNode sidecar (owner fields, added in ugc_incremental.c) plus this
 * lazily-heap-allocated UMemDebug substate (one +8 B UVM pointer in debug
 * builds).  EXCLUDED from urbi_get_determinism_checksum — never add any field
 * here to checksum_walk_cb.  Internal only (no public C API). */
#ifndef URBI_MEMDEBUG_H
#define URBI_MEMDEBUG_H

#include <stddef.h>
#include <stdint.h>

#ifndef URBI_MEM_DEBUG
#  define URBI_MEM_DEBUG 0
#endif

/* MCU-small defaults; host overrides via -D. Only meaningful with the gate on. */
#ifndef URBI_MEM_QUARANTINE_DEPTH
#  define URBI_MEM_QUARANTINE_DEPTH 16
#endif
#ifndef URBI_MEM_REDZONE_BYTES
#  define URBI_MEM_REDZONE_BYTES 16
#endif
#define URBI_MEM_POISON_BYTE   0xDEu   /* freed-cell poison pattern */
#define URBI_MEM_REDZONE_BYTE  0xC5u   /* trailing-redzone guard pattern */
#define URBI_MEM_HLV_DEPTH     8       /* heap-lock-violation ring depth */

struct UVM;
struct UCell;

#if URBI_MEM_DEBUG

/* One quarantined freed cell (carries the owner info copied off the node, since
 * the node is freed at sweep time before the cell is physically released). */
typedef struct {
    struct UCell   *cell;
    size_t          size;       /* user size (redzone excluded) */
    uint64_t        seq;
    const uint32_t *owner_pc;
    void           *owner_ret;
} UQuarantineEnt;

/* A denied post-urbi_lock_heap() allocation attempt. */
typedef struct {
    const uint32_t *owner_pc;
    void           *owner_ret;
    uint16_t        owner_op;
    uint16_t        strand_id;
    size_t          size;
} UHeapLockViol;

/* Per-handle-slot creation site (parallel to vm->handle_table). */
typedef struct {
    void           *owner_ret;
    uint16_t        strand_id;
    uint64_t        seq;
} UHandleOwner;

typedef struct UMemDebug {
    uint64_t        alloc_seq;                          /* monotonic; feeds node->seq */

    UQuarantineEnt  quarantine[URBI_MEM_QUARANTINE_DEPTH];
    uint32_t        q_head;
    uint32_t        q_count;

    UHeapLockViol   hlv[URBI_MEM_HLV_DEPTH];
    uint32_t        hlv_head;
    uint32_t        hlv_count;

    UHandleOwner   *handle_owner;                       /* alloc_fn-managed; grown w/ handle_table */
    uint32_t        handle_owner_cap;

    size_t          redzone_violations;                 /* found by urbi_gc_mem_validate */
    size_t          poison_violations;                  /* write-after-free caught on eviction */
    size_t          double_frees;                       /* double handle release */
} UMemDebug;

/* Substate lifecycle — lazy: umemdbg_init may leave vm->memdbg NULL on OOM
 * (owner/quarantine then degrade gracefully; redzone/poison still work). */
void umemdbg_init(struct UVM *vm);
void umemdbg_destroy(struct UVM *vm);   /* flush+verify quarantine, free substate + handle_owner */

/* Tier-2: poison + quarantine release (replaces a bare alloc_fn(cell,0,ud)). */
void umemdbg_release_cell(struct UVM *vm, struct UCell *cell, size_t size,
                          uint64_t seq, const uint32_t *owner_pc, void *owner_ret);
/* Verify all quarantined cells' poison is still intact; returns violation count. */
int  umemdbg_quarantine_verify(struct UVM *vm);
/* Write the trailing redzone guard for a cell of `size` user bytes. */
void umemdbg_write_redzone(struct UCell *cell, size_t size);

/* B5: record a denied post-lock allocation attempt. */
void umemdbg_note_heaplock(struct UVM *vm, size_t size,
                           const uint32_t *owner_pc, uint16_t owner_op,
                           uint16_t strand_id, void *owner_ret);

/* B4: handle owner tracking. */
void umemdbg_handle_grow(struct UVM *vm, uint32_t new_cap);
void umemdbg_handle_created(struct UVM *vm, uint32_t slot, void *owner_ret,
                            uint16_t strand_id);
void umemdbg_handle_released(struct UVM *vm, uint32_t slot, int was_live);

#endif /* URBI_MEM_DEBUG */

#endif /* URBI_MEMDEBUG_H */
