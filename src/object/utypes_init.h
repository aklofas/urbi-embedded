/* SPDX-License-Identifier: BSD-3-Clause */
/* utypes_init.h — single-purpose declaration for the object-model
 * built-in UType descriptor registration entry point.
 *
 * Implementation in utypes_init.c registers seven UTYPE_* tags
 * (UObject / UProtos / UShape / UProps / USlotHandle / UChunkInstance /
 * UProtoInstance) directly into vm->type_table[], bypassing
 * urbi_register_type which guards tags < UTYPE_HOST_BASE per src/utype.c.
 *
 * Kept out of ushape.h and uobject.h: the function spans seven types and
 * does not belong inside any one of their layout headers. */

#ifndef UTYPES_INIT_H
#define UTYPES_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;

/* Register the object-model cell types' UType descriptors directly into
 * vm->type_table[].  Called from urbi_vm_init after vm->type_table[] has been
 * zeroed. */
void urbi_object_builtin_types_init(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* UTYPES_INIT_H */
