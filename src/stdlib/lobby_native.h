/* SPDX-License-Identifier: BSD-3-Clause */
/* src/stdlib/lobby_native.h — v0.9.1 Phase 5: Lobby proto + native primitive.
 *
 * The Lobby proto is a runtime-type singleton (sibling to Tag / Event /
 * Mutex / Date / Duration) that ships in every build — its existence is
 * NOT gated on URBI_ENABLE_REPL because the per-realm writer fallback
 * chain (Phase 1) is also a default-build feature and `Lobby.echo` /
 * `Lobby.wall` are useful as urbiscript-side output primitives even when
 * no network REPL is configured.
 *
 * urbi_lobby_native_register(vm)
 *   Allocates vm->lobby_proto as a URBI_ATOM_OBJECT-family UObject,
 *   chains it onto root Object, and installs the single native method
 *   `__builtin_lobby_send`.  Idempotent (no-op if vm->lobby_proto is
 *   already non-NULL).  Returns URBI_OK / URBI_ERR_INVALID_ARG /
 *   URBI_ERR_OOM.
 *
 * urbi_lobby_native_register_globals(vm, realm)
 *   Post-loop hook for urbi_populate_realm_globals: binds "Lobby" as a
 *   realm-global slot pointing at vm->lobby_proto.  Mirrors
 *   urbi_stdlib_register_primitives_globals — lands past the slot-0..7
 *   packed-flag CONSTANT enforcement range.
 *
 * urbi_lobby_register_session(vm, session_realm)
 *   Append session_realm->global_object to the `lobbies` List slot on
 *   vm->lobby_proto.  Called by urepl_session_create (REPL only) to keep
 *   the urbiscript-visible `Lobby.lobbies` collection in sync with live
 *   sessions.  Safe to call before the .u overlay has populated
 *   Lobby.lobbies — early calls during VM init return URBI_OK without
 *   mutation.
 *
 * urbi_lobby_unregister_session(vm, session_realm)
 *   Remove session_realm->global_object from the `lobbies` List.  Called
 *   by urepl_session_destroy.
 *
 * urbi_lobby_invoke_handleDisconnect(vm, session_realm)
 *   Look up `handleDisconnect` on session_realm->global_object's proto
 *   chain; if it resolves to a UClosure, invoke it with the lobby as
 *   `self` and no args.  Errors are silently dropped (the dispatcher
 *   cannot meaningfully recover from a user-defined cleanup hook fault
 *   during teardown).  Returns URBI_OK whether or not a handler ran.
 *
 * GC reachability: vm->lobby_proto is shaded by object_roots_walker
 * alongside the primitive protos.  The List held in its
 * `lobbies` slot is reachable through the proto's normal slot walk. */

#ifndef URBI_STDLIB_LOBBY_NATIVE_H
#define URBI_STDLIB_LOBBY_NATIVE_H

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct URealm;

int urbi_lobby_native_register(struct UVM *vm);
int urbi_lobby_native_register_globals(struct UVM *vm, struct URealm *realm);

int urbi_lobby_register_session(struct UVM *vm, struct URealm *session_realm);
int urbi_lobby_unregister_session(struct UVM *vm, struct URealm *session_realm);

int urbi_lobby_invoke_handleDisconnect(struct UVM *vm,
                                       struct URealm *session_realm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_STDLIB_LOBBY_NATIVE_H */
