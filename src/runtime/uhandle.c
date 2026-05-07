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

/* Grow the handle table.  Returns 0 on success, -1 on OOM. */
static int
handle_table_grow(UVM *vm)
{
    uint32_t old_cap = vm->handle_table_cap;
    uint32_t new_cap = old_cap ? old_cap * 2U : INITIAL_CAP;
    /* vm->alloc_fn with (ptr, n>0) == realloc semantics (umodule.h comment). */
    UValue *grown = (UValue *)vm->alloc_fn(vm->handle_table,
                                           new_cap * sizeof(UValue),
                                           vm->alloc_ud);
    if (grown == NULL) return -1;
    /* Zero-init the newly added slots. */
    urbi_zero(&grown[old_cap], (new_cap - old_cap) * sizeof(UValue));
    vm->handle_table     = grown;
    vm->handle_table_cap = new_cap;
    return 0;
}

UHandle
urbi_handle_create(UVM *vm, UValue v)
{
    URBI_ASSERT_NOT_ISR(vm);
    /* Grow if the next slot would be out of range. */
    if (vm->handle_table_next_id >= vm->handle_table_cap) {
        if (handle_table_grow(vm) != 0) return URBI_HANDLE_INVALID;
    }
    uint32_t slot = vm->handle_table_next_id++;
    vm->handle_table[slot] = v;
    return slot + 1U;   /* 1-indexed; 0 is URBI_HANDLE_INVALID */
}

UValue
urbi_handle_get(UVM *vm, UHandle h)
{
    UValue nil = {0};
    nil.kind = UVAL_NIL;
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
    UValue nil = {0};
    nil.kind = UVAL_NIL;
    vm->handle_table[slot] = nil;
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
