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
    /* urbi_gc_alloc zeroes the allocation; set the identity fields.
     * gc_byte is managed by the GC (set to current_white by urbi_gc_alloc). */
    ev->type_tag        = UTYPE_EVENT;
    ev->flags           = 0U;
    ev->pad0[0]         = 0U;
    ev->pad0[1]         = 0U;
    ev->pad0[2]         = 0U;
    ev->pad0[3]         = 0U;
    ev->pad0[4]         = 0U;
    ev->at_watchers_head = NULL;
    ev->waiters_head     = NULL;
    ev->name.kind        = UVAL_NIL;
    ev->name.v.i         = 0;

    return ev;
}
