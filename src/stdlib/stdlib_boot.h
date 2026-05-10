/* SPDX-License-Identifier: BSD-3-Clause */
/* stdlib_boot.h — M6 Phase 3 stdlib bootstrap entry point.
 *
 * Wave 1 minimum: register the nine Object root C-native methods on the
 * realm-global Object proto.  Wave 2 grows this to load the full stdlib
 * (atom proto methods + container internals + .u overlay blob).
 *
 * Per spec §5.2 boot order: stdlib boot runs AFTER urbi_native_protos_init
 * (Tag, Event natives) and BEFORE realm-globals populate.  At Phase 3 the
 * call site is inside urbi_populate_realm_globals immediately after the
 * idempotent urbi_native_protos_init guard, so the Object proto's
 * methods are visible the moment "Object" is installed as a realm
 * global. */

#ifndef URBI_STDLIB_BOOT_H
#define URBI_STDLIB_BOOT_H

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;

/* Returns URBI_OK on success or URBI_ERR_OOM on allocation failure.
 * Idempotent: subsequent calls are silent no-ops once vm->stdlib_booted
 * is set. */
int urbi_stdlib_boot(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_STDLIB_BOOT_H */
