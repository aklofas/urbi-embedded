/* SPDX-License-Identifier: BSD-3-Clause */
/* M3 forward declarations: stub implementations for functions that land in
   later tasks.  Each stub is a static inline no-op so that dispatch_loop_until_yield
   can reference them now without link errors.
   DELETE each stub in its owning task:
     gc_slice                — T22/T24
     drain_pending_onleave_queue — T35
   (watcher_eval_dirty removed at T34: real impl lives in src/uwatcher_eval.c)
   (unwind_walk removed at T8: real urbi_unwind() lives in src/uunwind.c) */

#ifndef M3_FORWARD_DECLS_H
#define M3_FORWARD_DECLS_H

#include "uvm.h"
#include "ustrand.h"

#ifndef URBI_GC_SLICE_BUDGET
#  define URBI_GC_SLICE_BUDGET 16384u    /* row 10 §6.5; T24 may move to ugc.h */
#endif

/* M3 stubs — replaced by their owning tasks. */
static inline void gc_slice(UVM *vm, size_t budget) { (void)vm; (void)budget; }
static inline void drain_pending_onleave_queue(UVM *vm) { (void)vm; }
/* unwind_walk removed at T8: replaced by urbi_unwind() in src/uunwind.c */

#endif /* M3_FORWARD_DECLS_H */
