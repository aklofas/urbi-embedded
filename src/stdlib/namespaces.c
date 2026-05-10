/* SPDX-License-Identifier: BSD-3-Clause */
/* namespaces.c — M6 Phase 8: C-native namespace globals.
 *
 * Math / System / System.Platform / Global / CallMessage — see banner in
 * namespaces.h.  Phase 8 task T85 lands the shell + register entry; the
 * subsequent T86..T91 tasks fill in constants + native methods + post-
 * loop realm-global registration.
 *
 * Allocation pattern mirrors runtime_types.c (Exception primitive proto):
 * a vanilla URBI_ATOM_OBJECT-family UObject per namespace, methods
 * installed via a per-namespace method table walked by install_methods.
 * GC reachability comes from object_roots_walker (uobject.c) which
 * shades each vm->*_proto field during MARK_ROOTS. */

#include "stdlib/namespaces.h"

#include "urbi/urbi.h"                 /* URBI_OK / URBI_ERR_* */
#include "vm/uvm.h"                    /* UVM */

int
urbi_stdlib_register_namespaces(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    /* T85 shell: no namespace protos allocated yet.  T86..T91 land each
     * namespace incrementally.  Idempotent: vm->stdlib_booted upstream
     * gates re-entry from a second boot. */
    return URBI_OK;
}

int
urbi_stdlib_register_namespace_globals(UVM *vm, struct URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;
    /* T85 shell: no realm-global bindings yet.  Each subsequent task
     * extends this hook to bind one namespace name to its proto. */
    return URBI_OK;
}
