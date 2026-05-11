/* SPDX-License-Identifier: BSD-3-Clause */
/* Per-VM string canonicalization. Freestanding. */

#ifndef UINTERN_H
#define UINTERN_H

#include <stddef.h>

/* Pull in UGcRootCallback typedef (via ugc.h → umodule.h for UValue). */
#include "gc/ugc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration — UVM is defined in uvm.h. */
struct UVM;

/* Intern bytes[0..nbytes) into vm's intern table.
 *
 * On success: returns a canonical, null-terminated `const char *` whose
 * bytes match the input. The pointer is stable for the lifetime of the
 * UVM (no unintern at v1.0 — strings live until urbi_vm_destroy). Two calls
 * with byte-equal inputs return the SAME pointer (pointer-equality
 * implies content-equality and vice versa).
 *
 * On OOM: returns NULL. Caller must check and propagate.
 *
 * Thread-safety: NOT thread-safe. Single-threaded per VM at v1.0.  Public
 * entry points (ustr_intern, uintern_destroy, uintern_count) are guarded
 * by URBI_ASSERT_NOT_ISR since T30 / FOUND-011 — ISR-deposited events that
 * intern would race the cooperative-VM intern table.  ustr_op_name
 * inherits the guard via its forward to ustr_intern.
 *
 * Lifetime invariants (FOUND-044, v0.5.5):
 *   - Intern pointers are NOT cross-VM-stable.  Two distinct UVMs each own
 *     their own intern table; pointers from one MUST NOT be passed to
 *     another, and pointer-equality between VMs has no meaning.
 *   - Do NOT store interned pointers in serialized state (bytecode caches,
 *     persisted snapshots, on-disk module manifests).  They are valid only
 *     for the address-space of the originating VM and only until that VM
 *     is destroyed.  Re-intern from raw bytes on every load.
 *   - Intern-table teardown happens at urbi_vm_destroy via uintern_destroy
 *     below; every previously returned pointer is invalid after that call.
 *
 * Implementation: open-addressing hash, FNV-1a, grow-by-2 at load > 0.7.
 * Allocates via vm->alloc_fn. */
const char *ustr_intern(struct UVM *vm, const char *bytes, size_t nbytes);

/* Free all interned strings and the table itself. Called from
 * urbi_vm_destroy. Safe on a NULL or zero-initialized table. */
void uintern_destroy(struct UVM *vm);

/* Debug helper. Returns the count of unique strings interned in vm.
 * Returns 0 if intern_table is NULL. */
size_t uintern_count(struct UVM *vm);

/* Operator-name interning helpers (Gap #4 — operator overload via method
 * dispatch, M6 Wave 3).
 *
 * Each helper interns the operator's slot-name string on the first call and
 * caches the result in a static (valid for the process lifetime, since intern
 * pointers are stable within a VM until urbi_vm_destroy).
 *
 * IMPORTANT: these caches are per-process-lifetime, NOT per-VM.  If two VMs
 * are created in the same process the second VM will produce a DIFFERENT
 * pointer for the same string (intern tables are per-VM), but the VM that
 * did the first call will have its pointer cached.  Because urbi is always
 * single-VM in practice (URBI_SCHED_COOPERATIVE), and unit tests call
 * urbi_vm_destroy between runs, the static-cache shortcut is safe at v1.0.
 * A multi-VM aware version would key on vm pointer; that is a v1.x concern.
 *
 * Unary negation ("-") and binary subtraction ("-") share a slot name by
 * the locked convention in the Wave 3 plan; dispatch is contextual. */
USymbol *ustr_op_name(struct UVM *vm, const char *op, size_t len);

/* GC root provider for the intern table (row 10 §5.5).
 *
 * No-op by design through v1.0 (FOUND-024, v0.5.5).  Interned strings are
 * stored as raw `const char *` inside UInternStr allocations — they are NOT
 * GC-managed UValues, and the intern table itself owns each UInternStr block
 * directly (freed at uintern_destroy).  Strong ownership by the table means
 * the GC has nothing to keep alive on its behalf.
 *
 * The function is registered as a root provider so the provider-slot dispatch
 * path stays symmetric with the other GC subsystems; the body deliberately
 * reports zero roots.  See uintern.c for the full disposition note + the
 * v1.x upgrade path (string→UString GC cell migration). */
void intern_table_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* UINTERN_H */
