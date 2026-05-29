/* SPDX-License-Identifier: BSD-3-Clause */
/* Host-handle table implementation — row 10 §5.6.
 * Freestanding-strict: no <stdlib.h> / <string.h>.  All allocation goes
 * through vm->alloc_fn.  Zero-init uses a byte loop. */

#include "runtime/uhandle.h"
#include "runtime/umacros.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"   /* URBI_ASSERT_NOT_ISR */
#include "gc/ugc.h"
#include <stddef.h>
#include <stdint.h>

#define INITIAL_CAP  16U

/* Grow the handle table.  Returns 0 on success, -1 on OOM or overflow. */
static int
handle_table_grow(UVM *vm)
{
    uint32_t old_cap = vm->handle_table_cap;
    uint32_t new_cap = old_cap ? old_cap * 2U : INITIAL_CAP;
    /* FOUND-002: doubling overflow check.  At old_cap > UINT32_MAX/2 the
     * `* 2U` wraps to a smaller value than old_cap; reject rather than
     * silently shrink the table. */
    if (old_cap != 0U && new_cap < old_cap) return -1;
    /* vm->alloc_fn with (ptr, n>0) == realloc semantics (umodule.h comment). */
    UValue *grown = (UValue *)vm->alloc_fn(vm->handle_table,
                                           new_cap * sizeof(UValue),
                                           vm->alloc_ud);
    if (grown == NULL) return -1;
    /* Zero-init the newly added slots. */
    urbi_zero(&grown[old_cap], (new_cap - old_cap) * sizeof(UValue));
    vm->handle_table     = grown;
    vm->handle_table_cap = new_cap;
#if URBI_MEM_DEBUG
    if (vm->memdbg == NULL) umemdbg_init(vm);
    umemdbg_handle_grow(vm, new_cap);   /* parallel owner array tracks the table */
#endif
    return 0;
}

UHandle
urbi_handle_create(UVM *vm, UValue v)
{
    URBI_ASSERT_NOT_ISR(vm);
    /* FOUND-002: pre-check next_id wraparound.  If the post-increment below
     * would wrap to 0, the returned (slot+1) collides with URBI_HANDLE_INVALID
     * AND the indexed write goes to a non-existent slot.  Reject here. */
    if (vm->handle_table_next_id == 0xFFFFFFFFU) {
        return URBI_HANDLE_INVALID;
    }
    /* Grow if the next slot would be out of range. */
    if (vm->handle_table_next_id >= vm->handle_table_cap) {
        if (handle_table_grow(vm) != 0) return URBI_HANDLE_INVALID;
    }
    uint32_t slot = vm->handle_table_next_id++;
    vm->handle_table[slot] = v;
#if URBI_MEM_DEBUG
    umemdbg_handle_created(vm, slot, __builtin_return_address(0),
                           (uint16_t)(uintptr_t)vm->cur_strand);
#endif
    return slot + 1U;   /* 1-indexed; 0 is URBI_HANDLE_INVALID */
}

UValue
urbi_handle_get(UVM *vm, UHandle h)
{
    /* FOUND-010: ISR-safety symmetry with urbi_handle_create / _release. */
    URBI_ASSERT_NOT_ISR(vm);
    UValue nil = urbi_make_nil();
    if (h == URBI_HANDLE_INVALID) return nil;
    /* h is 1-indexed; slot = h - 1. */
    uint32_t slot = h - 1U;
    if (slot >= vm->handle_table_cap) return nil;
    return vm->handle_table[slot];
}

void
urbi_handle_release(UVM *vm, UHandle h)
{
    URBI_ASSERT_NOT_ISR(vm);
    if (h == URBI_HANDLE_INVALID) return;
    uint32_t slot = h - 1U;
    if (slot >= vm->handle_table_cap) return;
#if URBI_MEM_DEBUG
    {
        int was_live = (vm->handle_table[slot].kind != UVAL_NIL);
        umemdbg_handle_released(vm, slot, was_live);   /* flags double-release */
    }
#endif
    /* FOUND-019 + FOUND-048: zero-init UValue via canonical helper. */
    vm->handle_table[slot] = urbi_make_nil();
    /* Slot is logically free but not reused at M3 — no free-list.
     * v1.x adds slot reuse by threading nil slots as a free-list. */
}

void
host_handle_walk_roots(UVM *vm, UGcRootCallback cb, void *ctx)
{
    URBI_ASSERT_NOT_ISR(vm);
    uint32_t i;
    for (i = 0U; i < vm->handle_table_cap; i++) {
        if (vm->handle_table[i].kind != UVAL_NIL)
            cb(vm, &vm->handle_table[i], ctx);
    }
}
