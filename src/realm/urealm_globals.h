/* SPDX-License-Identifier: BSD-3-Clause */
/* urealm_globals.h — static built-in name registry + realm populate routine.
 * Spec #5 §3.  Row 13 (built-in name registry). */

#ifndef UREALM_GLOBALS_H
#define UREALM_GLOBALS_H

#include <stddef.h>
#include <stdbool.h>

#include "urbi/types.h"  /* UValue (avoids pulling module/umodule.h here) */
#include "urbi/urbi.h"   /* UErrCode */

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct URealm;

/* === URegistryEntry ===
 *
 * One entry in the static built-in registry.  The resolver is called
 * lazily at populate time (realm creation) so the table can be declared
 * const in .rodata — VM singletons are runtime-allocated, not link-time.
 *
 * is_self_ref: when true, urbi_populate_realm_globals overrides the resolver
 * result with realm->global_object (the "Realm" self-loop per spec #5 §3.3).
 * Explicit flag prevents confusion with NULL from OOM or not-yet-initialized
 * atom protos (which are lazy-allocated by the resolver functions themselves). */
typedef struct URegistryEntry {
    const char  *name;                      /* literal C string; NOT interned at table time */
    UValue     (*resolver)(struct UVM *vm); /* lazy: returns the value at populate time */
    bool         is_const;                  /* sets URBI_SLOT_FLAG_CONSTANT on populate */
    bool         is_self_ref;               /* true only for "Realm" → points at global_object */
} URegistryEntry;

/* The static table and its entry count.  15 entries at v1.0 (spec #5 §3.1). */
extern const URegistryEntry urbi_builtin_registry[];
extern const size_t         urbi_builtin_registry_count;

/* Populate a freshly-created realm's global_object with the 15 built-in
 * name→value bindings.  Called from urbi_realm_create after global_object
 * is created.
 *
 * Iterates urbi_builtin_registry[], calls each resolver, interns the name,
 * and writes via urbi_object_set_local_slot + urbi_object_install_property
 * (CONSTANT) onto realm->global_object.
 *
 * Returns URBI_OK on success.  Returns URBI_ERR_OOM if any intern or slot
 * allocation fails.  Partial population is acceptable — caller destroys
 * the realm on failure. */
UErrCode urbi_populate_realm_globals(struct UVM *vm, struct URealm *realm);

#ifdef __cplusplus
}
#endif

#endif /* UREALM_GLOBALS_H */
