/* SPDX-License-Identifier: BSD-3-Clause */
/* v0.10.10 / D7-C: detach / disown C-natives.
 *
 * These back the overlay-side `var detach = function(lazy fun) { __detach_strand(fun) }`
 * and `var disown = function(lazy fun) { __disown_strand(fun) }` wrappers in
 * control_overlay.u.  The lazy-arg emit path (uemit_stmt.c) wraps the
 * call-site expression in an implicit closure; the native receives the
 * UClosure and spawns it. */

#ifndef URBI_CONTROL_NATIVE_H
#define URBI_CONTROL_NATIVE_H

struct UVM;
struct URealm;

#ifdef __cplusplus
extern "C" {
#endif

/* Bind __detach_strand / __disown_strand as realm globals.  Called from
 * urbi_populate_realm_globals via the static built-in registry.  Returns
 * URBI_OK on success, URBI_ERR_OOM on allocation failure. */
int urbi_control_native_register_globals(struct UVM *vm, struct URealm *realm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_CONTROL_NATIVE_H */
