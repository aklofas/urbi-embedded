/* SPDX-License-Identifier: BSD-3-Clause */
/* Per-strand call frame types + capacity constants.
   Extracted here so that both ustrand.h and uvm.h can include this
   header without circularity.

   UValue dependency: this header uses UValue but cannot include
   src/value/uvalue.h directly because the include chain
   uvalue.h → umodule.h → uframe.h would form a cycle.
   Includers must ensure UValue is in scope before including this header.
   In practice every translation unit includes umodule.h first (which
   defines UValue and then includes uframe.h), so this is satisfied
   automatically.

   T6 migration: UCallFrame and UUpvalCell moved from umodule.h to uframe.h.
   UVM_MAX_FRAMES and UVM_STACK_CAP moved from uvm.h to uframe.h.
   umodule.h includes uframe.h to re-export the types it previously defined.
   ustrand.h includes uframe.h after uvalue.h so UValue is available. */

#ifndef UFRAME_H
#define UFRAME_H

#include <stdbool.h>
#include <stdint.h>

/* UValue is required by this header.  See include-cycle note in the file
   banner above; includers must pull it in via src/module/umodule.h. */

#ifdef __cplusplus
extern "C" {
#endif

/* --- Capacity constants --- */

#ifndef UVM_MAX_FRAMES
#  define UVM_MAX_FRAMES 64
#endif
#ifndef UVM_STACK_CAP
#  define UVM_STACK_CAP  2048      /* total register slots across all frames */
#endif

/* --- Forward declarations for pointer-only uses in struct fields --- */

struct UProto;    /* defined in umodule.h */
struct UClosure;  /* defined in umodule.h */

/* --- UUpvalCell: runtime heap cell for captured locals.
   Forward typedef only — the full struct definition (with UCell prefix at
   offset 0 for GC) lives in uclosure.h.  This mirrors how UClosure is
   forward-declared in umodule.h: uframe.h → ugc.h → umodule.h → uframe.h
   would form a circular include chain, so the complete layout is deferred
   to the first header that has both UValue and UCell in scope.
   Files that only need `UUpvalCell *` (e.g. ustrand.h, uvm.h) include this
   header; files that touch UUpvalCell fields include uclosure.h. */
typedef struct UUpvalCell UUpvalCell;

/* --- UCallFrame: per-call-frame state saved/restored on function dispatch. --- */
typedef struct UCallFrame {
    struct UClosure *closure;         /* NULL for top-level frame */
    struct UProto   *proto;           /* bytecode source for this frame */
    const uint32_t  *pc;              /* current instruction pointer */
    UValue          *base;            /* base of register window in shared stack */
    int              result_dest_reg; /* where caller wants the return value */
    UValue           recv;            /* receiver value saved at OP_CALL dispatch
                                         time — sourced from R[A+1] when the
                                         instruction's C carries the method flag
                                         (set by the preceding OP_SELF), nil
                                         otherwise.  Loaded by OP_LOAD_RECV for
                                         `this` access (Gap #3 — v0.6.2 Phase 2;
                                         re-sourced from R[A+1] at v1.6 S42). */
} UCallFrame;

#ifdef __cplusplus
}
#endif

#endif /* UFRAME_H */
