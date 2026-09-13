/* SPDX-License-Identifier: BSD-3-Clause */
/* stdlib_boot.h — stdlib bootstrap entry point.
 *
 * Registers the nine Object root C-native methods on the realm-global
 * Object proto, then loads the full stdlib (atom proto methods +
 * container internals + .u overlay blob).
 */

#ifndef URBI_STDLIB_BOOT_H
#define URBI_STDLIB_BOOT_H

#include <stddef.h>  /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;

/* Returns URBI_OK on success or URBI_ERR_OOM on allocation failure.
 * Idempotent: subsequent calls are silent no-ops once vm->stdlib_booted
 * is set. */
int urbi_stdlib_boot(struct UVM *vm);

/* Baked stdlib bytecode blob.
 */
extern const unsigned char urbi_stdlib_bytecode[];
extern const size_t        urbi_stdlib_bytecode_len;

#ifdef __cplusplus
}
#endif

#endif /* URBI_STDLIB_BOOT_H */
