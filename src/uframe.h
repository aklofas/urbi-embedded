/* SPDX-License-Identifier: BSD-3-Clause */
/* Per-strand call frame types + capacity constants.
   Extracted here so that both ustrand.h and uvm.h can include this
   header without circularity.

   IMPORTANT: This header uses UValue but does NOT include umodule.h or
   uvalue.h to avoid circular dependencies (uvalue.h → umodule.h → uframe.h
   → uvalue.h).  Includers must ensure UValue is in scope before including
   this header.  In practice every translation unit includes umodule.h first,
   which defines UValue, so this is satisfied automatically.

   T6 migration: UCallFrame and UUpvalCell moved from umodule.h to uframe.h.
   UVM_MAX_FRAMES and UVM_STACK_CAP moved from uvm.h to uframe.h.
   umodule.h includes uframe.h to re-export the types it previously defined.
   ustrand.h includes uframe.h after uvalue.h so UValue is available. */

#ifndef UFRAME_H
#define UFRAME_H

#include <stdbool.h>
#include <stdint.h>

/* UValue is required by this header; includers must pull it in via umodule.h
   before including uframe.h.  The circular-include constraint prevents us
   from including umodule.h or uvalue.h here. */

#ifdef __cplusplus
extern "C" {
#endif

/* --- Capacity constants --- */

#define UVM_MAX_FRAMES 64
#define UVM_STACK_CAP  2048        /* total register slots across all frames */

/* --- Forward declarations for pointer-only uses in struct fields --- */

struct UProto;    /* defined in umodule.h */
struct UClosure;  /* defined in umodule.h */

/* --- UUpvalCell: runtime heap cell for captured locals.
   When a closure captures a local that is still live on a call frame
   stack, the cell points into the register window (on_heap=false).
   When the scope exits (OP_CLOSE), the value is copied into the cell
   itself and on_heap is set to true. */
typedef struct UUpvalCell {
    bool    on_heap;
    union {
        UValue  *stack_ptr;     /* on_heap=false: pointer into register window */
        UValue   value;         /* on_heap=true:  owned copy                   */
    } u;
    struct UUpvalCell *next;    /* intrusive singly-linked list in strand */
} UUpvalCell;

/* --- UCallFrame: per-call-frame state saved/restored on function dispatch. --- */
typedef struct UCallFrame {
    struct UClosure *closure;         /* NULL for top-level frame */
    struct UProto   *proto;           /* bytecode source for this frame */
    const uint32_t  *pc;              /* current instruction pointer */
    UValue          *base;            /* base of register window in shared stack */
    int              result_dest_reg; /* where caller wants the return value */
} UCallFrame;

#ifdef __cplusplus
}
#endif

#endif /* UFRAME_H */
