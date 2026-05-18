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
 *
 * v0.8.4 Option B Step C-2: UClosure is now GC-managed.  urbi_gc_alloc
 * zeroes the payload, sets type_tag = UTYPE_CLOSURE, sets
 * gc_byte = current_white, and threads the cell onto vm->all_cells_head
 * with a sidecar.  The type descriptor registered at Step B carries the
 * walk_uclosure + uclosure_destroy finalizer so the GC sweep handles
 * both marking and freeing.
 *
 * list_head threading is preserved for Step C-2 so callers still pass
 * &s->closure_list; the chain is dormant (no code reads it for lifetime
 * purposes after Step C-2).  Step C-3 deletes closure_list + the param.
 *
 * Returns NULL on OOM. */
UClosure *vm_alloc_closure(UVM *vm, UProto *proto,
                           UClosure **list_head) {
    uint8_t nup = proto->nupvals;
    /* sizeof(UClosure) already includes 1 pointer in upvals[1]; add nup-1 more. */
    size_t extra = (nup > 1U) ? (size_t)(nup - 1U) * sizeof(UUpvalCell *) : 0U;
    size_t nbytes = sizeof(UClosure) + extra;

    /* v0.8.4 Option B Step C-2: promote to GC-managed allocation. */
    UCell *c = urbi_gc_alloc(vm, nbytes, UTYPE_CLOSURE);
    if (c == NULL) return NULL;
    UClosure *cl = (UClosure *)c;

    cl->proto = proto;
    /* v0.8.1 Variant B Phase 2: bump root_proto.refcount via uproto_root_of()
     * so the single canonical counter accumulates all closure binds.
     * For a nested proto this lands on proto->root (the module's root_proto);
     * for a root proto (native stdlib closures) it lands on proto itself.
     * Paired with the dec in uclosure_destroy (the finalizer). */
    umodule_proto_refcount_inc(uproto_root_of(proto));
    cl->nupvals    = nup;

    /* list_head threading is dormant — reachability via strand_walk_roots
     * (entry_closure, frames[i].closure) handles lifetime.  Kept in
     * signature until Step C-3 deletes it. */
    if (list_head != NULL) {
        cl->next_alloc = *list_head;
        *list_head     = cl;
    }
    return cl;
}

/* Find or create an open UUpvalCell for &R[slot].
 * Cells are kept in the strand's open_upvals list, sorted by stack address
 * (descending: newest captures at the front).
 *
 * v0.8.4 Option B Step C-2: UUpvalCell is now GC-managed.  urbi_gc_alloc
 * zeroes the payload + sets type_tag = UTYPE_UPVAL_CELL.  The open_upvals
 * chain is yielded as a GC root by strand_walk_roots (Step C-1 root #8). */
UUpvalCell *vm_open_upvalue(UVM *vm, UStrand *s, UValue *slot) {
    /* Scan existing open cells. */
    UUpvalCell *cell = s->open_upvals;
    while (cell != NULL) {
        if (cell->u.stack_ptr == slot) return cell;
        cell = cell->next;
    }
    /* Create a new open cell via GC-managed allocation. */
    UCell *c = urbi_gc_alloc(vm, sizeof(UUpvalCell), UTYPE_UPVAL_CELL);
    if (c == NULL) return NULL;
    cell = (UUpvalCell *)c;
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

/* v0.8.4 Option B Step C-2: UUpvalCells are GC-managed; sweep reclaims them.
 * This function used to alloc_fn-free every cell on the open_upvals chain;
 * now it only clears the head pointer so the strand no longer reaches them
 * as GC roots (after which the next sweep collects them if unreachable).
 * Step C-3 deletes this function outright once open_upvals management is
 * fully GC-driven and the call sites are updated. */
void vm_free_open_upvalues(UVM *vm, UStrand *s) {
    (void)vm;
    s->open_upvals = NULL;
}
