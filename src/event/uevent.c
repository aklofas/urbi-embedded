/* SPDX-License-Identifier: BSD-3-Clause */
/* UEvent lifecycle.
 * Spec #3 §3.1.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * Allocation uses urbi_gc_alloc (GC-managed from birth). */

#include "event/uevent.h"
#include "vm/uvm.h"
#include "urbi/gc.h"   /* urbi_gc_alloc, UTYPE_EVENT */
#include "gc/ugc.h"
#include <stddef.h>

/* === urbi_event_create ===
 *
 * Allocate a UEvent via urbi_gc_alloc and zero-initialize payload fields.
 * urbi_gc_alloc zeroes the whole allocation, so every byte is 0 on entry;
 * we then set the mandatory non-zero fields explicitly.
 *
 * Returns NULL on OOM. */

UEvent *
urbi_event_create(struct UVM *vm)
{
    UCell *c;
    UEvent *ev;

    if (vm == NULL) return NULL;

    c = urbi_gc_alloc(vm, sizeof(UEvent), UTYPE_EVENT);
    if (c == NULL) return NULL;

    ev = (UEvent *)c;
    /* urbi_gc_alloc zeroes the allocation, then sets cell->gc_byte to
     * vm->current_white (see ugc_incremental.c::urbi_gc_alloc).  We then
     * re-write the identity fields explicitly.
     *
     * EVENT-001: gc_byte is INTENTIONALLY skipped here.  The allocator
     * owns gc_byte — writing `ev->gc_byte = 0;` "for symmetry with the
     * other zero-init fields" would clobber the current_white color and
     * make the cell observe as already-marked when current_white == 1,
     * risking premature collection at the next mark phase.  Do not add
     * a gc_byte assignment here without coordinating with the GC barrier
     * contract.  pad0[0..4] is zero-init from urbi_gc_alloc — no loop. */
    ev->type_tag        = UTYPE_EVENT;
    ev->flags           = 0U;
    ev->at_watchers_head = NULL;
    ev->waiters_head     = NULL;
    ev->name.kind        = UVAL_NIL;
    ev->name.v.i         = 0;

    return ev;
}
