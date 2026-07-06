/* SPDX-License-Identifier: BSD-3-Clause */
/* Tag.enter / Tag.leave native getters with lazy alloc (spec #3 §8.2). */

#ifndef TAG_NATIVE_H
#define TAG_NATIVE_H

#include "chunk/uchunk.h"    /* UValue */
#include "vm/uvm.h"            /* UVMError */

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct UTag;

/* urbi_tag_enter_getter: lazy-allocate tag->enter_event on first read.
 *   Returns UVAL_EVENT wrapping the UEvent.
 *   On OOM: throws URBI_ERR_OOM via urbi_throw (requires vm->cur_strand
 *   to be set) and returns NIL.
 *   Idempotent: second call returns the same event. */
UValue urbi_tag_enter_getter(struct UVM *vm, struct UTag *tag);

/* urbi_tag_leave_getter: same contract as urbi_tag_enter_getter but for leave_event. */
UValue urbi_tag_leave_getter(struct UVM *vm, struct UTag *tag);

/* urbi_tag_native_register: allocate vm->tag_proto and install getter/setter slots.
 *   Called from urbi_vm_init after urbi_object_register_gc_roots.
 *   Returns UVM_OK on success, UVM_OOM if the proto allocation or any of
 *   the four slot installs fail.  On failure, vm->tag_proto is reset to
 *   NULL; the proto cell itself is GC-managed and is collected at the
 *   next sweep. */
UVMError urbi_tag_native_register(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* TAG_NATIVE_H */
