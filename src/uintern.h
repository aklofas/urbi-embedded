/* SPDX-License-Identifier: BSD-3-Clause */
/* Per-VM string canonicalization. Freestanding. */

#ifndef UINTERN_H
#define UINTERN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration — UVM is defined in uvm.h. */
struct UVM;

/* Intern bytes[0..nbytes) into vm's intern table.
 *
 * On success: returns a canonical, null-terminated `const char *` whose
 * bytes match the input. The pointer is stable for the lifetime of the
 * UVM (no unintern at v1.0 — strings live until uvm_destroy). Two calls
 * with byte-equal inputs return the SAME pointer (pointer-equality
 * implies content-equality and vice versa).
 *
 * On OOM: returns NULL. Caller must check and propagate.
 *
 * Thread-safety: NOT thread-safe. Single-threaded per VM at v1.0.
 *
 * Implementation: open-addressing hash, FNV-1a, grow-by-2 at load > 0.7.
 * Allocates via vm->alloc_fn. */
const char *ustr_intern(struct UVM *vm, const char *bytes, size_t nbytes);

/* Free all interned strings and the table itself. Called from
 * uvm_destroy. Safe on a NULL or zero-initialized table. */
void uintern_destroy(struct UVM *vm);

/* Debug helper. Returns the count of unique strings interned in vm.
 * Returns 0 if intern_table is NULL. */
size_t uintern_count(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* UINTERN_H */
