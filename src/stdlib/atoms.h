/* SPDX-License-Identifier: BSD-3-Clause */
/* atoms.h — M6 Phase 5: C-native methods on Boolean / Integer / Float /
 * String atom protos.
 *
 * Wave 1 (Phase 4) registered the Boolean / String stubs (toString /
 * length) on the atom protos.  Phase 5 fills out the Tier 1 method set:
 *   Boolean: negate
 *   Integer: asString / asFloat / asBoolean / bitand / bitor / bitxor /
 *            bitnot / shl / shr
 *   Float:   asString / asInteger / asBoolean / sqrt / sin / cos / tan /
 *            asin / acos / atan / atan2 / log / log10 / exp / pow / floor /
 *            ceil / abs / round / isNaN / isInfinite
 *   String:  size / isEmpty / charAt / asciiAt / indexOf / contains /
 *            startsWith / endsWith / toUpper / toLower / asInteger / asFloat
 *
 * Symbolic operators (`+`, `-`, `*`, `/`, `==`, `<`, …) are NOT registered
 * as slots: those are inline VM opcodes (OP_ADD, OP_EQ, OP_LT, …) — see
 * src/vm/uvm.c and src/vm/uvm_arith.c.  Phase 5 registers only the named
 * methods that resolve via OP_GETSLOT atom-method dispatch (Phase 2).
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
