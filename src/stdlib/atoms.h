/* SPDX-License-Identifier: BSD-3-Clause */
/* atoms.h — C-native methods on Boolean / Integer / Float /
 * String atom protos.
 *
 * Boolean `&&`, `||`, `!` similarly dispatch through inline opcodes when
 * they exist (today: only inline truthiness via OP_TEST in conditions);
 * the legacy `'!' = false` form is a slot, not a method, and the v1.0
 * surface uses `negate()` for the named-method form.
 */

#ifndef URBI_STDLIB_ATOMS_H
#define URBI_STDLIB_ATOMS_H

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;

/* Install Phase-5 C-native method slots on the Boolean / Integer / Float
 * / String atom protos.  Idempotent — subsequent calls overwrite existing
 * slot values with the same closure objects (mirrors atom_protos.c).
 *
 * Returns URBI_OK on success or URBI_ERR_OOM on allocation failure. */
int urbi_stdlib_register_atom_methods(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_STDLIB_ATOMS_H */
