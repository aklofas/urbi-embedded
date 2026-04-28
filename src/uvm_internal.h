/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_internal.h — private declarations shared between uvm.c and uunwind.c.
 * Not part of the public C API; do not include from embedder code.
 *
 * T8: vm_close_upvalues promoted from static to non-static so that
 * uunwind.c can call it from the bridging-stub walker.  T9 may re-evaluate
 * whether this export remains necessary once the real walker lands. */

#ifndef UVM_INTERNAL_H
#define UVM_INTERNAL_H

#include "ustrand.h"
#include "uframe.h"  /* UUpvalCell */

/* Heapify all open upvalue cells whose stack address is >= threshold.
 * Removed cells are appended to *closed_list.
 * Called by OP_CLOSE, OP_RET, and urbi_unwind. */
void vm_close_upvalues(UStrand *s, UValue *threshold,
                       UUpvalCell **closed_list);

#endif /* UVM_INTERNAL_H */
