/* SPDX-License-Identifier: BSD-3-Clause */
/* uclosure.h — UClosure runtime function value (proto + captured upvalues).
 *
 * UClosure embeds UCell as its first member at offset 0 (M4 — closes the
 * M3 baseline TODO at gc/ugc_incremental.h).  This makes the
 * UClosure* → UCell* cast performed by urbi_gc_upvalue_write well-defined.
 *
 * Lives outside umodule.h because the struct definition needs both UValue
 * (from umodule.h) and UCell (from gc/ugc.h), and gc/ugc.h itself includes
 * umodule.h for UValue — a direct UCell embed inside umodule.h would create
 * a circular include.  Files that only need `struct UClosure *` keep the
 * forward typedef in umodule.h; files that touch UClosure fields include
 * this header. */

#ifndef UCLOSURE_H
#define UCLOSURE_H

#include <stdint.h>

#include "umodule.h"                       /* UProto + forward typedef `UClosure` + UUpvalCell */
#include "gc/ugc.h"                        /* UCell (2 B) */
#include "object/umoduleinstance.h"        /* UProtoInstance — M4: IC table per nested proto */

/* --- UClosure: runtime function value (proto + captured upvalues).
 * Heap-allocated by OP_CLOSURE; lives until end-of-run via the strand's
 * pre-GC closure_list (threaded by next_alloc).  GC migration is tracked
 * as a follow-up M4 task — at this commit the cell header is initialised
 * (type_tag + gc_byte) so the upvalue write barrier can safely cast
 * UClosure* → UCell*, but the closure is NOT yet enrolled on
 * vm->all_cells_head.  The legacy free-list mechanic still owns lifetime.
 *
 * The upvals[] array is a trailing flexible member — allocate
 * sizeof(UClosure) + (nupvals - 1) * sizeof(UUpvalCell*).
 * `next_alloc` threads all closures allocated in one run into a free list
 * so they can be reclaimed at halt (pre-GC bookkeeping). */
struct UClosure {
    UCell             cell;        /* 2 B — type_tag + gc_byte at offset 0/1 */
    /* 6 B compiler-inserted padding before next pointer field */
    UProto           *proto;
    UProtoInstance   *proto_inst;  /* M4 — points into the owning UModuleInstance's
                                      proto_instances bulk; carries this closure's
                                      IC table for OP_GETSLOT/OP_SETSLOT.  NULL when
                                      no module instance is bound (e.g. uvm_run
                                      transient strands at the M4 baseline; full
                                      module-instance wiring lands at a later task). */
    struct UClosure  *next_alloc; /* legacy free-list link (lifetime owner pre-GC) */
    uint8_t           nupvals;
    UUpvalCell       *upvals[1];  /* flexible trailing array of pointers */
};

#endif /* UCLOSURE_H */
