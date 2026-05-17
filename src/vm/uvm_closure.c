/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_closure.c — VM closure + upvalue lifecycle.
 * Extracted from uvm.c during v0.5.4-decompose (VM #5). */

#include "vm/uvm.h"
#include "vm/uvm_internal.h"
#include "runtime/uclosure.h"  /* UClosure, UUpvalCell, UProto */
#include "runtime/umacros.h"   /* urbi_zero */
#include "sched/ustrand.h"     /* UStrand */
#include "gc/ugc.h"            /* UTYPE_CLOSURE */
#include "value/uvalue.h"      /* UValue */
#include "module/umodule.h"
#include "runtime/uframe.h"
#include <stddef.h>
#include <stdint.h>

/* Allocate a UClosure that can hold `nupvals` upvalue cell pointers.
 * Uses the VM's allocator.  Threads the new closure into *list_head so
 * the caller can free every closure at end-of-run (pre-GC bookkeeping).
 *
 * M4: UClosure embeds UCell at offset 0.  The cell header is initialised
 * here (type_tag = UTYPE_CLOSURE, gc_byte = vm->current_white) so that
 * urbi_gc_upvalue_write may safely cast UClosure* → UCell* and read a
 * valid color for the barrier check.  The closure is NOT enrolled on
 * vm->all_cells_head — lifetime stays with the strand's closure_list
 * (legacy free-list).  GC-managed allocation via urbi_gc_alloc is tracked
 * as a follow-up M4 task; it requires enrolling the transient urbi_vm_run
 * strand as a GC root before closures stored in registers can survive
 * a mid-dispatch collection cycle.
 *
 * Returns NULL on OOM. */
UClosure *vm_alloc_closure(UVM *vm, UProto *proto,
                           UClosure **list_head) {
    uint8_t nup = proto->nupvals;
    /* sizeof(UClosure) already includes 1 pointer in upvals[1]; add nup-1 more. */
    size_t extra = (nup > 1U) ? (size_t)(nup - 1U) * sizeof(UUpvalCell *) : 0U;
    size_t nbytes = sizeof(UClosure) + extra;
    UClosure *cl = (UClosure *)vm->alloc_fn(NULL, nbytes, vm->alloc_ud);
    if (cl == NULL) return NULL;
    urbi_zero(cl, nbytes);
    /* Cell header (M4): well-formed for barrier safety even though the
     * closure is not on vm->all_cells_head at this commit. */
    cl->cell.type_tag = UTYPE_CLOSURE;
    cl->cell.gc_byte  = vm->current_white;
    cl->proto      = proto;
    /* Piece A — bump proto refcount so module_destroy can rescue this
     * proto if the closure outlives its compiling module. */
    umodule_proto_refcount_inc(proto);
    cl->nupvals    = nup;
    cl->next_alloc = *list_head;
    *list_head     = cl;
    return cl;
}

/* Find or create an open UUpvalCell for &R[slot].
 * Cells are kept in the strand's open_upvals list, sorted by stack address
 * (descending: newest captures at the front). */
UUpvalCell *vm_open_upvalue(UVM *vm, UStrand *s, UValue *slot) {
    /* Scan existing open cells. */
    UUpvalCell *cell = s->open_upvals;
    while (cell != NULL) {
        if (cell->u.stack_ptr == slot) return cell;
        cell = cell->next;
    }
    /* Create a new open cell. */
    cell = (UUpvalCell *)vm->alloc_fn(NULL, sizeof(UUpvalCell), vm->alloc_ud);
    if (cell == NULL) return NULL;
    urbi_zero(cell, sizeof(UUpvalCell));
    cell->on_heap    = false;
    cell->u.stack_ptr = slot;
    cell->next       = s->open_upvals;
    s->open_upvals   = cell;
    return cell;
}

/* Heapify all open cells whose stack address is >= threshold.
 * Removed cells are appended to *closed_list (for per-run bulk free at halt).
 * Called by OP_CLOSE, OP_RET, and urbi_unwind.
 * Declared in uvm.h for uunwind.c access. */
void vm_close_upvalues(UStrand *s, const UValue *threshold,
                       UUpvalCell **closed_list) {
    UUpvalCell **link = &s->open_upvals;
    while (*link != NULL) {
        UUpvalCell *cell = *link;
        if (cell->u.stack_ptr >= threshold) {
            cell->u.value = *cell->u.stack_ptr;
            cell->on_heap  = true;
            *link = cell->next;
            /* Thread into closed_list using the now-free next pointer. */
            cell->next = *closed_list;
            *closed_list = cell;
        } else {
            link = &cell->next;
        }
    }
}

/* Free all open upvalue cells remaining on a strand. */
void vm_free_open_upvalues(UVM *vm, UStrand *s) {
    UUpvalCell *cell = s->open_upvals;
    while (cell != NULL) {
        UUpvalCell *next = cell->next;
        vm->alloc_fn(cell, 0, vm->alloc_ud);
        cell = next;
    }
    s->open_upvals = NULL;
}
