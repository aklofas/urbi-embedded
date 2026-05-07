/* SPDX-License-Identifier: BSD-3-Clause */
/* uobject_internal.h — private inter-TU API for the object subsystem.
 * Created v0.5.4-decompose; consumed only by src/object/ TUs. */

#ifndef UOBJECT_INTERNAL_H
#define UOBJECT_INTERNAL_H

#include "object/uobject.h"
#include "object/uic.h"

/* OBJ-026: lift function-scope macro to file-scope #define. */
#define URBI_RESOLVE_STACK_CAP   64

/* struct UVM is forward-declared in uobject.h; .c files that include
 * vm/uvm.h pick up the full typedef. */

/* From uobject.c residual (lifecycle). */
uint32_t  next_id(struct UVM *vm);

/* From uobject_proto.c. */
void      shade_existing_protos(struct UVM *vm, UObject *obj);
int       valid_proto(const UObject *obj, const UObject *p);
UProtos  *urbi_protos_alloc(struct UVM *vm, uint32_t n);

/* From uobject_lookup.c. */
int  lookup_inner(struct UVM *vm, UObject *obj, USymbol *name, UValue *out);
void clear_lookup_stamp_cb(struct UVM *vm, UCell *cell, void *ctx);

#endif /* UOBJECT_INTERNAL_H */
