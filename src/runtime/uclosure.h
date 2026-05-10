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

#include "module/umodule.h"                       /* UProto + forward typedef `UClosure` + UUpvalCell */
#include "gc/ugc.h"                        /* UCell (2 B) */
#include "object/umodule_instance.h"        /* UProtoInstance — M4: IC table per nested proto */

/* --- UClosure: runtime function value (proto + captured upvalues).
 * Heap-allocated by OP_CLOSURE; lives until end-of-run via the strand's
 * pre-GC closure_list (threaded by next_alloc).  The cell header is
 * initialised (type_tag + gc_byte) so the upvalue write barrier can
 * safely cast UClosure* → UCell*, but the closure is NOT yet enrolled on
 * vm->all_cells_head — the strand-local closure_list still owns lifetime
 * pre-GC (full GC enrollment deferred to v1.x).
 *
 * proto_inst is bound end-to-end by the M4 follow-up landed on `main`
 * 2026-05-03: OP_CLOSURE writes
 * `s->module_instance->proto_instances->entries[bx + 1]` here so
 * OP_GETSLOT/OP_SETSLOT can read the per-(vm,proto) IC table off this
 * field.  NULL only when no module_instance was wired (defensive — does
 * not occur in production paths from urbi_vm_run / urbi_run_chunk).
 *
 * The upvals[] array is a trailing flexible member — allocate
 * sizeof(UClosure) + (nupvals - 1) * sizeof(UUpvalCell*).
 * `next_alloc` threads all closures allocated in one run into a free list
 * so they can be reclaimed at halt (pre-GC bookkeeping). */
/* Native-method extension forward typedef — full definition in
 * src/stdlib/object_root.h.  When set on a UClosure, OP_CALL invokes
 * this C function instead of dispatching the proto's bytecode body. */
struct UVM;
typedef int (*urbi_native_method_fn)(struct UVM *vm,
                                     UValue self,
                                     UValue *args,
                                     uint8_t nargs,
                                     UValue *out);

struct UClosure {
    UCell             cell;        /* 2 B — type_tag + gc_byte at offset 0/1 */
    /* Compiler-inserted padding before the next pointer field.
     * 64-bit targets: 6 B (cell at 0..1, pad 2..7, proto at 8).
     * 32-bit targets: 2 B (cell at 0..1, pad 2..3, proto at 4).
     * Either way the field order below matches natural pointer alignment;
     * no manual pad bytes are inserted. */
    UProto           *proto;
    UProtoInstance   *proto_inst;  /* M4 follow-up: per-(vm,proto) IC tier
                                      pointer.  See banner above for
                                      binding/lifecycle. */
    struct UClosure  *next_alloc; /* legacy free-list link (lifetime owner pre-GC) */
    /* M6 Phase 3: C-native method dispatch. NULL for ordinary urbiscript
     * closures.  When non-NULL, OP_CALL calls this function instead of
     * pushing a bytecode frame.  proto / proto_inst / upvals are NULL on
     * native closures (GC trace already guards each via NULL checks).
     * The receiver (`self`) is read from vm->last_recv (set by OP_GETSLOT
     * each time a slot is loaded) — no bytecode change required. */
    urbi_native_method_fn native_fn;
    uint8_t           nupvals;
    UUpvalCell       *upvals[1];  /* flexible trailing array of pointers */
};

#endif /* UCLOSURE_H */
