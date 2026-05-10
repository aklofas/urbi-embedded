/* SPDX-License-Identifier: BSD-3-Clause */
/* runtime_types.h — M6 Phase 7: C-native runtime-type protos.
 *
 * Phase 7 of M6 stdlib lands the runtime-type method surfaces on top of
 * the M3/M4/M5/Wave-1 baselines:
 *
 *   - Exception primitive root: fresh proto exposing `.new(message)`,
 *     `.message` slot, and `.raise` (which deposits THROW unwind so
 *     try/catch blocks can intercept).
 *
 * Phase 7 is intentionally narrow:
 *   - Code (closure) reflection methods (`apply`, `bodyString`) — DEFERRED:
 *     UVAL_CLOSURE has no atom proto in v1.0 (urbi_atom_proto_for_value
 *     routes closures to root Object), so installing methods would require
 *     a new dispatch surface beyond the v1.0 scope.  Tracked in
 *     docs/urbi-embedded-backlog.md.
 *   - Tag.new / Tag.stop scripted constructors — DEFERRED: there is no
 *     UVAL_TAG kind, so a script-side Tag instance would need a UObject
 *     wrapper carrying a UTag in a hidden slot, and existing M3 utag_create
 *     is realm-bound.  Tracked in docs/urbi-embedded-backlog.md.
 *   - Event.new / Event.emit / Event.syncEmit — already shipped at Wave 1
 *     via src/event/uevent_native.c.
 *
 * Boot order: urbi_stdlib_register_runtime_types(vm) is called from
 * urbi_stdlib_boot AFTER containers (which also depends on atom protos). */

#ifndef URBI_STDLIB_RUNTIME_TYPES_H
#define URBI_STDLIB_RUNTIME_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct URealm;

/* Allocates vm->exception_proto + installs Exception.new / Exception.raise
 * + binds "Exception" as a realm-global constant (deferred to a post-loop
 * hook that mirrors the container post-loop registration).
 *
 * Idempotent: re-entry through urbi_stdlib_boot is gated upstream by
 * vm->stdlib_booted.
 *
 * Returns URBI_OK on success or URBI_ERR_OOM on alloc failure. */
int urbi_stdlib_register_runtime_types(struct UVM *vm);

/* Post-registry hook: installs Exception as a realm global on `realm`.
 * Called by urbi_populate_realm_globals AFTER the 15-row registry loop
 * completes (same pattern as urbi_stdlib_register_container_globals).
 *
 * Returns URBI_OK on success, URBI_ERR_OOM / URBI_ERR_INVALID_ARG. */
int urbi_stdlib_register_runtime_globals(struct UVM *vm, struct URealm *realm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_STDLIB_RUNTIME_TYPES_H */
