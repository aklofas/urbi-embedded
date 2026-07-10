/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_closure.c — VM closure + upvalue lifecycle.
 * Extracted from uvm.c during v0.5.4-decompose (VM #5). */

#include "vm/uvm.h"
#include "vm/uvm_internal.h"
#include "runtime/uclosure.h"  /* UClosure, UUpvalCell, UProto */
#include "runtime/umacros.h"   /* urbi_zero */
#include "sched/ustrand.h"     /* UStrand */
#include "gc/ugc.h"            /* UTYPE_CLOSURE */
#include "gc/ugc_incremental.h" /* urbi_gc_upvalue_pre_store (close barrier) */
#include "value/uvalue.h"      /* UValue */
#include "chunk/uchunk.h"
#include "runtime/uframe.h"
#include <stddef.h>
#include <stdint.h>

/* Allocate a UClosure that can hold `nupvals` upvalue cell pointers.
 *
 * v0.8.4 Option B Step C-2/C-3: UClosure is GC-managed.  urbi_gc_alloc
 * zeroes the payload, sets type_tag = UTYPE_CLOSURE, sets
 * gc_byte = current_white, and threads the cell onto vm->all_cells_head
 * with a sidecar.  The type descriptor registered at Step B carries the
 * walk_uclosure + uclosure_destroy finalizer so the GC sweep handles
 * both marking and freeing.
 *
 * The legacy list_head / closure_list parameter was deleted at Step C-3.
 *
 * Returns NULL on OOM. */
UClosure *urbi_vm_alloc_closure(UVM *vm, UProto *proto) {
    uint8_t nup = proto->nupvals;
    /* sizeof(UClosure) already includes 1 pointer in upvals[1]; add nup-1 more. */
    size_t extra = (nup > 1U) ? (size_t)(nup - 1U) * sizeof(UUpvalCell *) : 0U;
    size_t nbytes = sizeof(UClosure) + extra;

    UCell *c = urbi_gc_alloc(vm, nbytes, UTYPE_CLOSURE);
    if (c == NULL) return NULL;
    UClosure *cl = (UClosure *)c;

    cl->proto = proto;
    /* v0.8.1 Variant B Phase 2: bump root_proto.refcount via uproto_root_of()
     * so the single canonical counter accumulates all closure binds.
     * For a nested proto this lands on proto->root (the module's root_proto);
     * for a root proto (native stdlib closures) it lands on proto itself.
     * Paired with the dec in uclosure_destroy (the finalizer).
     * v0.10.1: use typed-handle API for saturation/underflow diagnostics. */
    urbi_proto_ref_acquire(uproto_root_of(proto), URBI_PROTO_REF_OWNER_CLOSURE);
    cl->nupvals    = nup;
    return cl;
}

/* Find or create an open UUpvalCell for &R[slot].
 * Cells are kept in the strand's open_upvals list in insertion order — each
 * new cell is prepended, so the most recent capture is at the front.  The
 * list is NOT sorted by stack address; urbi_vm_close_upvalues scans the whole
 * chain and closes every cell at or above the threshold, so order is
 * immaterial to correctness.
 *
 * v0.8.4 Option B Step C-2: UUpvalCell is now GC-managed.  urbi_gc_alloc
 * zeroes the payload + sets type_tag = UTYPE_UPVAL_CELL.  The open_upvals
 * chain is yielded as a GC root by strand_walk_roots (Step C-1 root #8). */
UUpvalCell *urbi_vm_open_upvalue(UVM *vm, UStrand *s, UValue *slot) {
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
 * v0.8.4 Step C-3: closed_list parameter removed; UUpvalCell is GC-managed.
 * Heapified cells are reachable via any closure's upvals[] array (GC root);
 * no per-run bulk list needed.  Called by OP_CLOSE, OP_RET, and urbi_unwind.
 * Declared in uvm.h for uunwind.c access. */
void urbi_vm_close_upvalues(UStrand *s, const UValue *threshold) {
    UUpvalCell **link = &s->open_upvals;
    while (*link != NULL) {
        UUpvalCell *cell = *link;
        if (cell->u.stack_ptr >= threshold) {
            /* GC-07: Dijkstra forward barrier on the CELL.  It
             * may already be BLACK mid-cycle while the captured value is
             * still WHITE (either white — see uvalue_is_heap_white);
             * without a shade the value's only surviving reference can
             * end up inside an already-scanned cell and the sweep frees it
             * while reachable.  Same helper + same cell-parent shape as
             * OP_SETUPVAL's on_heap arm (the cell — shared between
             * sibling closures — is the barrier parent at both heapified
             * store sites; a closure's color is never the right check). */
            urbi_gc_upvalue_pre_store(s->vm, &cell->cell,
                                      *cell->u.stack_ptr);
            cell->u.value = *cell->u.stack_ptr;
            cell->on_heap  = true;
            *link = cell->next;
            cell->next = NULL;  /* no longer chained on open_upvals */
        } else {
            link = &cell->next;
        }
    }
}

/* vm_free_open_upvalues deleted at v0.8.4 Step C-3.
 * UUpvalCells are GC-managed; open_upvals is cleared directly at the one
 * remaining call site (ustrand_destroy via release_strand_resource_chain).
 * Declaration in uvm_internal.h removed simultaneously. */
