/* SPDX-License-Identifier: BSD-3-Clause */
/* namespaces.h — C-native namespace globals.
 *
 * This module lands the namespace globals on top of the
 * atom-proto baselines:
 *
 *   Math          — IEEE-754 constants (pi / e / nan / infinity).  Method
 *                   surface (sin / cos / sqrt …) defers to the Phase 10
 *                   .u overlay, which bounces to the Float atom-proto
 *                   methods landed in Phase 5.
 *   System        — host primitives.  time / cycle as a host-clock pair,
 *                   getenv as a libc shim, gc as an explicit collection
 *                   trigger.
 *   System.Platform — kind constant set at compile time via #ifdef.
 *   Global        — reflective namespace; .length returns the live count
 *                   of slots on realm->global_object.
 *   CallMessage   — placeholder proto reserved for v1.x legacy-fallback()
 *                   reflection.
 *
 * Boot order: urbi_stdlib_register_namespaces(vm) is called from
 * urbi_stdlib_boot AFTER runtime_types.  Realm-global binding for the
 * namespace names (Math / System / Global / CallMessage) is deferred to
 * urbi_stdlib_register_namespace_globals (called by urbi_populate_realm_-
 * globals AFTER the 15-row registry loop), mirroring the container +
 * runtime-type post-loop pattern so the registry's slot 0..7 layout for
 * the v1.0 packed-flag CONSTANT enforcement range stays intact. */

#ifndef URBI_STDLIB_NAMESPACES_H
#define URBI_STDLIB_NAMESPACES_H

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct URealm;

/* Allocates Math / System / Global / CallMessage proto UObjects, installs
 * their constants and methods, and stashes the proto pointers in vm
 * fields (vm->math_proto, vm->system_proto, vm->global_namespace_proto,
 * vm->callmessage_proto).  Realm-global binding is deferred to the
 * post-loop hook urbi_stdlib_register_namespace_globals.
 *
 * Idempotent — guarded by vm->stdlib_booted upstream.  Returns URBI_OK
 * on success or URBI_ERR_OOM on alloc failure. */
int urbi_stdlib_register_namespaces(struct UVM *vm);

/* Post-registry hook: installs Math / System / Global / CallMessage as
 * realm globals on `realm`.  Mirrors urbi_stdlib_register_runtime_globals
 * — lands at slots 15+, past the v1.0 packed-flag CONSTANT enforcement
 * range (slots 0..7).
 *
 * Returns URBI_OK on success, URBI_ERR_OOM / URBI_ERR_INVALID_ARG. */
int urbi_stdlib_register_namespace_globals(struct UVM *vm, struct URealm *realm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_STDLIB_NAMESPACES_H */
