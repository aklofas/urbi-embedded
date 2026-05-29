/* SPDX-License-Identifier: BSD-3-Clause */
/* src/runtime/umemdebug.c — on-target memory-debug substate (v0.11.3). */
#include "runtime/umemdebug.h"

#if URBI_MEM_DEBUG

#include "vm/uvm.h"
#include "gc/ugc.h"   /* UCell */

/* Local byte fill (freestanding-strict; mirror urbi_zero's discipline). */
static void mem_fill(void *p, uint8_t v, size_t n)
{
    uint8_t *b = (uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++) b[i] = v;
}

static int poison_intact(const struct UCell *cell, size_t size)
{
    const uint8_t *b = (const uint8_t *)cell;
    size_t i;
    for (i = 0; i < size; i++) if (b[i] != (uint8_t)URBI_MEM_POISON_BYTE) return 0;
    return 1;
}

void umemdbg_init(struct UVM *vm)
{
    UMemDebug *m;
    if (vm == NULL || vm->memdbg != NULL || vm->alloc_fn == NULL) return;
    m = (UMemDebug *)vm->alloc_fn(NULL, sizeof(UMemDebug), vm->alloc_ud);
    if (m == NULL) return;                /* graceful: owner/quarantine degrade */
    mem_fill(m, 0, sizeof(*m));
    vm->memdbg = m;
}

/* Physically free a cell through the host allocator after verifying poison. */
static void release_now(struct UVM *vm, UQuarantineEnt *e)
{
    if (e->cell == NULL) return;
    if (!poison_intact(e->cell, e->size)) vm->memdbg->poison_violations++;
    vm->alloc_fn(e->cell, 0U, vm->alloc_ud);
    e->cell = NULL;
}

void umemdbg_release_cell(struct UVM *vm, struct UCell *cell, size_t size,
                          uint64_t seq, const uint32_t *owner_pc, void *owner_ret)
{
    UMemDebug *m = vm->memdbg;
    UQuarantineEnt *slot;
    /* Poison the user payload regardless of quarantine availability. */
    mem_fill(cell, (uint8_t)URBI_MEM_POISON_BYTE, size);
    if (m == NULL) { vm->alloc_fn(cell, 0U, vm->alloc_ud); return; }  /* no substate: free now */
    if (m->q_count == URBI_MEM_QUARANTINE_DEPTH) {
        /* Evict + free the oldest (verifying its poison first). */
        release_now(vm, &m->quarantine[m->q_head]);
        m->q_head = (m->q_head + 1u) % URBI_MEM_QUARANTINE_DEPTH;
        m->q_count--;
    }
    slot = &m->quarantine[(m->q_head + m->q_count) % URBI_MEM_QUARANTINE_DEPTH];
    slot->cell = cell; slot->size = size; slot->seq = seq;
    slot->owner_pc = owner_pc; slot->owner_ret = owner_ret;
    m->q_count++;
}

int umemdbg_quarantine_verify(struct UVM *vm)
{
    UMemDebug *m = vm->memdbg;
    int viol = 0;
    uint32_t i, idx;
    if (m == NULL) return 0;
    for (i = 0; i < m->q_count; i++) {
        idx = (m->q_head + i) % URBI_MEM_QUARANTINE_DEPTH;
        if (m->quarantine[idx].cell &&
            !poison_intact(m->quarantine[idx].cell, m->quarantine[idx].size)) {
            m->poison_violations++;
            viol++;
        }
    }
    return viol;
}

void umemdbg_write_redzone(struct UCell *cell, size_t size)
{
    mem_fill((uint8_t *)cell + size, (uint8_t)URBI_MEM_REDZONE_BYTE, URBI_MEM_REDZONE_BYTES);
}

void umemdbg_note_heaplock(struct UVM *vm, size_t size,
                           const uint32_t *owner_pc, uint16_t owner_op,
                           uint16_t strand_id, void *owner_ret)
{
    UMemDebug *m = vm->memdbg;
    UHeapLockViol *v;
    if (m == NULL) return;
    v = &m->hlv[(m->hlv_head + m->hlv_count) % URBI_MEM_HLV_DEPTH];
    if (m->hlv_count == URBI_MEM_HLV_DEPTH) m->hlv_head = (m->hlv_head + 1u) % URBI_MEM_HLV_DEPTH;
    else m->hlv_count++;
    v->owner_pc = owner_pc; v->owner_op = owner_op; v->strand_id = strand_id;
    v->size = size; v->owner_ret = owner_ret;
}

void umemdbg_handle_grow(struct UVM *vm, uint32_t new_cap)
{
    UMemDebug *m = vm->memdbg;
    UHandleOwner *grown;
    uint32_t i;
    if (m == NULL || new_cap <= m->handle_owner_cap) return;
    grown = (UHandleOwner *)vm->alloc_fn(m->handle_owner,
                                         new_cap * sizeof(UHandleOwner), vm->alloc_ud);
    if (grown == NULL) return;            /* graceful: owner tracking degrades */
    for (i = m->handle_owner_cap; i < new_cap; i++) {
        grown[i].owner_ret = NULL; grown[i].strand_id = 0; grown[i].seq = 0;
    }
    m->handle_owner = grown;
    m->handle_owner_cap = new_cap;
}

void umemdbg_handle_created(struct UVM *vm, uint32_t slot, void *owner_ret,
                            uint16_t strand_id)
{
    UMemDebug *m = vm->memdbg;
    if (m == NULL || m->handle_owner == NULL || slot >= m->handle_owner_cap) return;
    m->handle_owner[slot].owner_ret = owner_ret;
    m->handle_owner[slot].strand_id = strand_id;
    m->handle_owner[slot].seq = ++m->alloc_seq;
}

void umemdbg_handle_released(struct UVM *vm, uint32_t slot, int was_live)
{
    UMemDebug *m = vm->memdbg;
    if (m == NULL) return;
    if (!was_live) { m->double_frees++; return; }   /* releasing an already-nil slot */
    if (m->handle_owner && slot < m->handle_owner_cap)
        m->handle_owner[slot].owner_ret = NULL;
}

void umemdbg_destroy(struct UVM *vm)
{
    UMemDebug *m;
    if (vm == NULL || (m = vm->memdbg) == NULL) return;
    /* Flush + verify the whole quarantine. */
    while (m->q_count > 0) {
        release_now(vm, &m->quarantine[m->q_head]);
        m->q_head = (m->q_head + 1u) % URBI_MEM_QUARANTINE_DEPTH;
        m->q_count--;
    }
    if (m->handle_owner) vm->alloc_fn(m->handle_owner, 0U, vm->alloc_ud);
    vm->alloc_fn(m, 0U, vm->alloc_ud);
    vm->memdbg = NULL;
}

#endif /* URBI_MEM_DEBUG */
