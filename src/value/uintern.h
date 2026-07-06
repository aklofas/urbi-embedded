/* SPDX-License-Identifier: BSD-3-Clause */
/* Per-VM string canonicalization. Freestanding. */

#ifndef UINTERN_H
#define UINTERN_H

#include <stddef.h>

/* Pull in UGcRootCallback typedef (via ugc.h → urbi/types.h for UValue). */
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
 * by URBI_ASSERT_NOT_ISR — ISR-deposited events that
 * intern would race the cooperative-VM intern table.  ustr_op_name
 * inherits the guard via its forward to ustr_intern.
 *
 * Lifetime invariants (v0.5.5):
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
size_t uintern_count(const struct UVM *vm);

/* Stats accessor. Total bytes currently allocated by
 * the intern subsystem on behalf of vm: every live UInternStr block
 * (header + payload, NUL included in the header's bytes[1]) PLUS the
 * current entries[] pointer array.  The fixed-size UInternTable struct
 * itself (one ~32 B block per VM) is not counted.  Grows on every miss;
 * on rehash the old entries[] array is subtracted and the new (larger)
 * one added.  Because interned strings are NEVER evicted at v1.0 (no
 * unintern), this counter only grows for the life of the VM — see
 * "Interned strings never evict" in docs/internals/gc.md.  Returns 0 on
 * NULL vm or before the first intern (the table is created lazily).
 * Surfaced to urbiscript via Debug.gc()'s "intern_bytes" field. */
size_t urbi_intern_bytes(const struct UVM *vm);

/* Operator-name interning helpers (Gap #4 — operator overload via method
 * dispatch).
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
 * No-op by design through v1.0 (v0.5.5).  Interned strings are
 * stored as raw `const char *` inside UInternStr allocations — they are NOT
 * GC-managed UValues, and the intern table itself owns each UInternStr block
 * directly (freed at uintern_destroy).  Strong ownership by the table means
 * the GC has nothing to keep alive on its behalf.
 *
 * The function is registered as a root provider so the provider-slot dispatch
 * path stays symmetric with the other GC subsystems; the body deliberately
 * reports zero roots.  See uintern.c for the full disposition note + the
 * v1.x upgrade path (string→UString GC cell migration). */
void urbi_gc_intern_table_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* UINTERN_H */
