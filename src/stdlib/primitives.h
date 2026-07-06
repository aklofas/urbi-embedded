/* SPDX-License-Identifier: BSD-3-Clause */
/* primitives.h — C-native primitives (Mutex, Date, Duration).
 *
 * This module lands three primitive types on top of the
 * Phase 5/6/7/8 baselines:
 *
 *   Mutex     — cooperative single-VM lock.  v1.0 is URBI_SCHED_COOPERATIVE,
 *               so the mutex is a non-blocking flag (lock / unlock /
 *               tryLock).  Phase 10's `.u` overlay grows Mutex.synchronized
 *               via waituntil for cooperative wait semantics.
 *   Date      — wall-clock access.  Date.now / Date.fromSeconds round-trip
 *               seconds-since-epoch via libc time(); Date.asString
 *               formats UTC as ISO-style "YYYY-MM-DD HH:MM:SS".
 *               Freestanding builds without time() return 0 / "".
 *   Duration  — thin wrapper over integer microseconds.  Time literals
 *               (100ms / 2s / 1d) lex to integer
 *               microseconds; Duration.fromMicroseconds wraps an integer
 *               in a Duration proto-shaped UObject for typed dispatch.
 *               .asMicroseconds / .asMilliseconds read the backing slot.
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
