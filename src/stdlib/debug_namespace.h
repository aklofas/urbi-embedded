/* SPDX-License-Identifier: BSD-3-Clause */
/* src/stdlib/debug_namespace.h — Debug urbiscript namespace (v0.9.1).
 *
 * Allocates a Debug proto UObject with 9 C-native methods (one per
 * introspect primitive) and binds it as a slot on each realm's global
 * object via urbi_realm_set_global.  Gated on URBI_ENABLE_REPL — the
 * default build (and freestanding cross targets) never link this TU
 * because the introspection primitives + JSON parser it depends on live
 * under src/repl/ which itself is REPL-only.
 *
 * Return values: each Debug.X() method returns the introspect output as
 * a urbi String (UVAL_STR).  This sidesteps the absence of UVAL_LIST /
 * UVAL_DICT in the v1.0 value space — clients that need structured data
 * can parse the JSON client-side, or use the dispatcher's introspect op
 * directly (which returns the same JSON in {kind:result,value} form).
 *
 * Debug.lobbies() / Debug.coros() / Debug.gc() etc. all share this contract.
 *
 * The proto is allocated lazily on first urbi_debug_namespace_register
 * call; subsequent calls are no-ops.  Idempotent. */

#ifndef URBI_STDLIB_DEBUG_NAMESPACE_H
#define URBI_STDLIB_DEBUG_NAMESPACE_H

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct URealm;

/* Allocate the Debug proto + install its 9 native methods.  Idempotent.
 * Returns URBI_OK on success or URBI_ERR_OOM on alloc failure. */
int urbi_debug_namespace_register(struct UVM *vm);

/* Bind "Debug" as a realm-global slot on `realm`.  Called from
 * urbi_populate_realm_globals after the runtime-types post-loop hook.
 * Returns URBI_OK / URBI_ERR_INVALID_ARG / URBI_ERR_OOM. */
int urbi_debug_namespace_register_globals(struct UVM *vm, struct URealm *realm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_STDLIB_DEBUG_NAMESPACE_H */
