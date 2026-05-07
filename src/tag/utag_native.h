/* SPDX-License-Identifier: BSD-3-Clause */
/* Tag.enter / Tag.leave native getters with lazy alloc (spec #3 §8.2). */

#ifndef TAG_NATIVE_H
#define TAG_NATIVE_H

#include "module/umodule.h"    /* UValue */

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct UTag;

/* tag_enter_getter: lazy-allocate tag->enter_event on first read.
 *   Returns UVAL_EVENT wrapping the UEvent.
 *   On OOM: throws URBI_ERR_OUT_OF_MEMORY via urbi_throw (requires vm->cur_strand
 *   to be set) and returns NIL.
 *   Idempotent: second call returns the same event. */
UValue tag_enter_getter(struct UVM *vm, struct UTag *tag);

/* tag_leave_getter: same contract as tag_enter_getter but for leave_event. */
UValue tag_leave_getter(struct UVM *vm, struct UTag *tag);

/* tag_native_register: allocate vm->tag_proto and install getter/setter slots.
 *   Called from uvm_init after urbi_object_register_gc_roots. */
void tag_native_register(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* TAG_NATIVE_H */
