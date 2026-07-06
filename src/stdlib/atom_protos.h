/* SPDX-License-Identifier: BSD-3-Clause */
/* atom_protos.h — per-atom-family C-native method registration.
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

/* === v0.9.1: mark every builtin atom + runtime-type proto readonly =====
 *
 * Sets URBI_OBJ_FLAG_READONLY (= UPROTO_FLAG_READONLY) on all 15 builtin
 * protos per spec §4.2, denying bytecode-side mutation (OP_SETSLOT raises
 * URBI_ERR_FROZEN_PROTO).  Host-side C API mutators are unaffected.
 *
 * Called from urbi_stdlib_boot AFTER every register helper has installed
 * its method/slot tables — the readonly bit must NOT block the population
 * pass that puts methods on Object/Number/etc.
 *
 * The Global namespace proto (vm->global_namespace_proto) is deliberately
 * NOT marked readonly; it's the designated mutable shared-state surface
 * (spec §4.1).
 *
 * Idempotent.  Returns URBI_OK; never fails (just an OR on flag bits). */
int urbi_atom_protos_mark_readonly(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_STDLIB_ATOM_PROTOS_H */
