/* SPDX-License-Identifier: BSD-3-Clause */
/* Public C API for the urbi object model (M4 / T8).
 *
 * Atom-family singletons + prototype-list mutators.  Internals (UObject
 * layout, UPROTOS_FOREACH macro, etc.) live in src/object/uobject.h and
 * are not exposed to host embedders.  The URBIAtomFamily enum (single
 * source of truth at v0.5.5) is shared between this public header and
 * the internal one; src/object/uobject.h includes this header rather
 * than redefining the enum. */

#ifndef URBI_OBJECT_H
#define URBI_OBJECT_H

#include "urbi/urbi.h"   /* struct UVM forward decl + URBI_OK / UErrCode */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle types — full definitions in src/object/uobject.h
 * (UObject) and src/object/ushape.h (UShape).  Host code never
 * dereferences these directly.  Guarded so internal TUs that include
 * src/object/uobject.h before this public header skip the redeclaration
 * (C99 forbids typedef redeclaration; C11 permits it). */
#ifndef URBI_OBJECT_TYPEDEF_DEFINED
#define URBI_OBJECT_TYPEDEF_DEFINED
typedef struct UObject UObject;
typedef struct UShape  UShape;
#endif

/* Atom families (single source of truth; v0.5.5 retired the dual-enum
 * with `_F` suffixes).  Low 4 bits of UObject.flags encode the family;
 * 9..15 reserved for v1.x. */
typedef enum {
    URBI_ATOM_OBJECT  = 0,   /* root Object */
    URBI_ATOM_INTEGER = 1,
    URBI_ATOM_FLOAT   = 2,
    URBI_ATOM_STRING  = 3,
    URBI_ATOM_LIST    = 4,
    URBI_ATOM_DICT    = 5,
    URBI_ATOM_TAG     = 6,
    URBI_ATOM_EVENT   = 7,
    URBI_ATOM_SYMBOL  = 8
} URBIAtomFamily;

/* === Atom-family accessors (T8) ===
 *
 * Lazy-allocate the per-VM atom singleton on first call; return the cached
 * pointer thereafter.  The root Object is the atom of all atoms; its protos
 * field is empty (no parent prototypes).  Every other atom's protos field
 * holds the single-tag encoding pointing at the root Object.
 *
 * Returns NULL on OOM (allocation failure during first-touch creation). */
UObject *urbi_object_root(struct UVM *vm);
UObject *urbi_object_atom(struct UVM *vm, URBIAtomFamily family);

/* === Prototype-list mutators (T11 implements; declared here to lock the
 *     ABI surface introduced by T8) ===
 *
 * urbi_object_add_proto    — append `proto` to obj's prototype list.
 * urbi_object_remove_proto — remove first occurrence of `proto`.
 * urbi_object_set_protos   — replace obj's prototype list with `list[0..n)`.
 *
 * Returns URBI_OK on success or a negative UErrCode on failure.  All three
 * are stubbed at T8 (return URBI_ERR_INVALID_ARG); T11 lands the real
 * cycle-check + storage-form transitions. */
int urbi_object_add_proto    (struct UVM *vm, UObject *obj, UObject *proto);
int urbi_object_remove_proto (struct UVM *vm, UObject *obj, UObject *proto);
int urbi_object_set_protos   (struct UVM *vm, UObject *obj, UObject **list, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* URBI_OBJECT_H */
