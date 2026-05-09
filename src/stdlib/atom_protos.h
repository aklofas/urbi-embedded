/* SPDX-License-Identifier: BSD-3-Clause */
/* atom_protos.h — M6 Phase 4: per-atom-family C-native method registration.
 *
 * Each atom family (Boolean, Integer, Float, String, nil, void) gets a
 * dedicated proto with at least .clone() and a minimum method set.
 * Wave 1 ships:
 *   Boolean: toString
 *   String:  length
 * Integer / Float / Nil / Void protos exist but inherit clone +
 * getSlot/setSlot/etc. from the Object root via prototype chain.
 *
 * Wave 2 fills out the family-specific arithmetic / math / overlay
 * methods. */

#ifndef URBI_STDLIB_ATOM_PROTOS_H
#define URBI_STDLIB_ATOM_PROTOS_H

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;

/* Allocate the Boolean / Nil / Void singletons and install C-native
 * method slots on the existing atom protos.  Idempotent — safe to call
 * multiple times; subsequent calls overwrite existing slot values with
 * the same closure objects.
 *
 * Returns URBI_OK on success or URBI_ERR_OOM on allocation failure. */
int urbi_atom_protos_register(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_STDLIB_ATOM_PROTOS_H */
