/* SPDX-License-Identifier: BSD-3-Clause */
/* isa_method.h — v0.10.11 / isA: universal type-test method on Object root.
 *
 * obj.isA(Proto) -> Bool.  True iff Proto appears in obj's transitive
 * proto chain.  For atom-typed receivers (UVAL_INT / FLOAT / STR /
 * BOOL / NIL), the logical proto is looked up via urbi_atom_proto_for_value.
 * For UVAL_OBJECT, the receiver's own UObject.protos chain is walked.
 *
 * Installed on vm->atom_object at stdlib_boot.  Reached by every proto
 * chain (atom_object is the root). */

#ifndef URBI_ISA_METHOD_H
#define URBI_ISA_METHOD_H

struct UVM;

#ifdef __cplusplus
extern "C" {
#endif

/* Install the isA native method on vm->atom_object.  Idempotent; safe to
 * call multiple times.  Returns URBI_OK on success, URBI_ERR_OOM or
 * URBI_ERR_INVALID_STATE on failure. */
int urbi_isa_method_register(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_ISA_METHOD_H */
