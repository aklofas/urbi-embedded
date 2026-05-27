/* SPDX-License-Identifier: BSD-3-Clause */
/* v0.10.11 / D6: Channel proto realm-global bindings.
 *
 * channel_overlay.u defines the Channel class as a stdlib overlay; this
 * C side does two things:
 *
 *   1. urbi_channel_proto_resolve caches vm->channel_proto by looking up
 *      "Channel" in the provided realm's global namespace.  Called from
 *      urbi_populate_realm_globals AFTER the bake-blob runs (the first
 *      realm call resolves it; subsequent calls are idempotent).
 *
 *   2. urbi_channel_register_globals installs Channel + cout + cerr +
 *      clog as realm globals.  Called per-realm from
 *      urbi_populate_realm_globals after urbi_channel_proto_resolve.
 *      Each realm gets fresh cout/cerr/clog instances (per P3 —
 *      multi-session safe). */

#ifndef URBI_CHANNEL_NATIVE_H
#define URBI_CHANNEL_NATIVE_H

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct URealm;

/* Cache vm->channel_proto by looking up "Channel" in realm's globals.
 * Must be called AFTER urbi_run_chunk (which runs channel_overlay.u and
 * installs the Channel class as a realm global).  Idempotent — returns
 * URBI_OK immediately if vm->channel_proto is already set.
 * Returns URBI_OK on success, URBI_ERR_INVALID_STATE if "Channel" not
 * yet present. */
int urbi_channel_proto_resolve(struct UVM *vm, struct URealm *realm);

/* Bind Channel + cout/cerr/clog as realm globals.  cout uses
 * Channel.new("output"); cerr uses Channel.new("error") + sets
 * quote=false; clog uses Channel.new("clog").  Called per-realm from
 * urbi_populate_realm_globals after urbi_channel_proto_resolve.
 * Returns URBI_OK / URBI_ERR_OOM / URBI_ERR_INVALID_STATE. */
int urbi_channel_register_globals(struct UVM *vm, struct URealm *realm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_CHANNEL_NATIVE_H */
