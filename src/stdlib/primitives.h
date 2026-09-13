/* SPDX-License-Identifier: BSD-3-Clause */
/* primitives.h — C-native primitives (Mutex, Date, Duration).
 *
 * Boot order: urbi_stdlib_register_primitives(vm) is called from
 * urbi_stdlib_boot AFTER namespaces.  Realm-global binding for the
 * primitive names (Mutex / Date / Duration) is deferred to
 * urbi_stdlib_register_primitives_globals (called by urbi_populate_-
 * realm_globals AFTER the 15-row registry loop), mirroring the container
 * + runtime-type + namespace post-loop pattern so the registry's slot
 * 0..7 layout for the v1.0 packed-flag CONSTANT enforcement range stays
 * intact. */

#ifndef URBI_STDLIB_PRIMITIVES_H
#define URBI_STDLIB_PRIMITIVES_H

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct URealm;

/* Allocates Mutex / Date / Duration proto UObjects, installs their
 * native methods, and stashes the proto pointers in vm fields
 * (vm->mutex_proto, vm->date_proto, vm->duration_proto).  Realm-global
 * binding is deferred to the post-loop hook urbi_stdlib_register_-
 * primitives_globals.
 *
 * Idempotent — guarded by vm->stdlib_booted upstream.  Returns URBI_OK
 * on success or URBI_ERR_OOM on alloc failure. */
int urbi_stdlib_register_primitives(struct UVM *vm);

/* Post-registry hook: installs Mutex / Date / Duration as realm globals
 * on `realm`.  Mirrors urbi_stdlib_register_namespace_globals — lands
 * at slots 15+, past the v1.0 packed-flag CONSTANT enforcement range
 * (slots 0..7).
 *
 * Returns URBI_OK on success, URBI_ERR_OOM / URBI_ERR_INVALID_ARG. */
int urbi_stdlib_register_primitives_globals(struct UVM *vm, struct URealm *realm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_STDLIB_PRIMITIVES_H */
