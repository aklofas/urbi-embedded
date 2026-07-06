/* SPDX-License-Identifier: BSD-3-Clause */
/* tag_globals.h — v0.10.10-job-introspection / D7-D:
 * tag-related realm globals (scopeTag).
 *
 * scopeTag is a call-style native that returns the innermost
 * UCLEANUP_TAG_SCOPE.owning_tag on the current strand's cleanup stack
 * as a UVAL_TAG, or nil if there is none.  Script-side: `scopeTag()`.
 *
 * Per REVIVAL §3.8 the Go-defer / C++-RAII analog: assign to a local
 * in a function to bind every spawn-or-watcher inside the function to
 * a tag that auto-stops on function return.
 *
 * Why call-style (not the legacy getter-property form per
 * share/urbi/system.u:212-213): the v1.0 OGET dispatch path
 * (urbi_vm_dispatch_getter → urbi_run_closure_on_scratch) is bytecode-only
 * and crashes on native closures (proto NULL).  Bridging native
 * closures into the OGET path is a v1.x follow-up.
 *
 * No new public C API.  Lives entirely on the urbiscript surface. */

#ifndef URBI_TAG_GLOBALS_H
#define URBI_TAG_GLOBALS_H

struct UVM;
struct URealm;

#ifdef __cplusplus
extern "C" {
#endif

/* VM-level initialisation hook for tag-globals subsystem.
 * Currently a no-op — scopeTag is bound per-realm in
 * urbi_tag_globals_register_globals, not at VM creation.
 * Kept symmetric with the other stdlib *_register hooks for
 * future extensibility. */
int urbi_tag_globals_register(struct UVM *vm);

/* Bind "scopeTag" as a realm-global pointing at a native closure that
 * wraps urbi_strand_scope_tag.  Returns the innermost ambient
 * UCLEANUP_TAG_SCOPE.owning_tag as UVAL_TAG.  One allocation per
 * realm; the realm-global slot roots the closure for the lifetime of
 * the realm.
 *
 * Returns URBI_OK on success, URBI_ERR_INVALID_ARG on NULL args,
 * URBI_ERR_OOM on closure-alloc / slot-set failure. */
int urbi_tag_globals_register_globals(struct UVM *vm, struct URealm *realm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_TAG_GLOBALS_H */
