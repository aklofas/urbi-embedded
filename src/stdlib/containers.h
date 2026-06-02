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

#include "urbi/types.h"   /* UValue (needed by v0.9.1 host-side List mutators) */

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct URealm;
struct UObject;

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

/* === v0.9.1 Phase 5: host-side List mutators for Lobby.lobbies ==========
 *
 * Append / remove a UValue from the UList backing a List UObject.  Intended
 * for the REPL dispatcher's session-lifecycle hooks (urbi_lobby_register_-
 * session / urbi_lobby_unregister_session) — NOT for general user-facing
 * mutation, which should go through the urbiscript .add / .set methods.
 *
 * Both functions are no-ops returning URBI_OK if list_obj is NULL or
 * carries no `_storage` slot (e.g. called before lobby.u has populated
 * Lobby.lobbies).  remove finds the first item that uvalue_equal-matches
 * and shifts the tail; missing items are also no-ops.
 *
 * Returns URBI_OK / URBI_ERR_INVALID_ARG / URBI_ERR_OOM. */
int urbi_stdlib_list_append_value(struct UVM *vm, struct UObject *list_obj,
                                  UValue item);
int urbi_stdlib_list_remove_first_equal(struct UVM *vm,
                                        struct UObject *list_obj,
                                        UValue item);

/* Create an empty List UObject (clone of the List atom proto, fresh
 * UList backing).  Used by lobby_native to install Lobby.lobbies as a
 * VM-singleton slot at boot.  Returns the new List UObject or NULL on
 * OOM. */
struct UObject *urbi_stdlib_list_new_empty(struct UVM *vm);

/* Ungated List-read accessors (UObject* form) for stdlib code outside the
 * ROS2 component.  len returns 0 and get returns nil for NULL/invalid/out-of-
 * range, never failing. */
size_t urbi_stdlib_list_len(struct UVM *vm, struct UObject *list_obj);
UValue urbi_stdlib_list_get(struct UVM *vm, struct UObject *list_obj, size_t i);

#ifdef __cplusplus
}
#endif

#endif /* URBI_STDLIB_CONTAINERS_H */
