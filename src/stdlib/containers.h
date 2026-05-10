/* SPDX-License-Identifier: BSD-3-Clause */
/* containers.h — M6 Phase 6: C-native container types.
 *
 * Phase 6 lands the v1.0 container surface as realm-globals registered
 * from this module.  Types covered:
 *
 *   Pair    — immutable 2-tuple with .first / .second slots
 *   Triplet — immutable 3-tuple with .first / .second / .third slots
 *   Tuple   — variadic immutable n-tuple over a fixed-cap UList backing
 *   List    — mutable, growable container of UValues
 *   Dict    — mutable, string-keyed open-address hash table
 *
 * Storage strategy: List and Dict instances allocate a backing buffer
 * (UList / UDict structs) via vm->alloc_fn and thread the buffer onto
 * vm->stdlib_containers for VM-lifetime cleanup at urbi_vm_destroy.
 * The per-instance UObject holds a hidden `_storage` slot whose UValue
 * encodes the backing pointer as UVAL_INT (cast through uintptr_t).
 *
 * The VM-lifetime ownership is intentional at v1.0 — backing buffers
 * are NOT GC-collected mid-run.  Tracked at design-risks under
 * "stdlib container backing buffers vm-lifetime".  v1.x lifts this to
 * proper UTYPE_LIST / UTYPE_DICT GC types when the cross-cutting
 * walker plumbing lands.
 *
 * Methods: only named methods (no symbolic operators).  v1.0 lex has
 * no `<<` or `[]` operator tokens, so List.add / List.at / Dict.get /
 * etc. are the surface.  Phase 10's .u overlay lands operator wrappers
 * if/when the lex/parse extensions land. */

#ifndef URBI_STDLIB_CONTAINERS_H
#define URBI_STDLIB_CONTAINERS_H

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct URealm;

/* Allocate the Pair / Triplet / Tuple atom protos and install C-native
 * method slots on List / Dict atom protos (URBI_ATOM_LIST /
 * URBI_ATOM_DICT singletons that the realm-populate registry already
 * publishes as "List" / "Dict" globals).  Pair / Triplet / Tuple
 * proto pointers are stashed in vm fields for later realm-global
 * binding; registering them as realm globals from this hook would
 * shift the registry's stable slot 0..7 layout that the v1.0 packed-
 * flag CONSTANT enforcement depends on.
 *
 * Called from urbi_stdlib_boot, which runs from urbi_populate_realm_-
 * globals BEFORE the 15-row registry loop.
 *
 * Idempotent — safe to call multiple times.  Returns URBI_OK on
 * success or URBI_ERR_OOM on allocation failure. */
int urbi_stdlib_register_containers(struct UVM *vm);

/* Install Pair / Triplet / Tuple as realm globals on `realm`.  Called
 * by urbi_populate_realm_globals AFTER the 15-row registry loop, so
 * these names land at slots 15+ — past the v1.0 packed-flag CONSTANT
 * enforcement range (slots 0..7).  The protos themselves are
 * already constructed by urbi_stdlib_register_containers and stashed
 * in vm fields. */
int urbi_stdlib_register_container_globals(struct UVM *vm, struct URealm *realm);

/* Free every backing buffer threaded onto vm->stdlib_containers.
 * Called by urbi_vm_destroy. */
void urbi_stdlib_containers_destroy(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_STDLIB_CONTAINERS_H */
