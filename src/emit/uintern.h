/* SPDX-License-Identifier: BSD-3-Clause */
/* Per-VM string canonicalization — the emitter's view of it.  Freestanding.
 *
 * The implementation lives in src/emit/ufront.c and is backed by the
 * runtime core's USym table (src/rt/ustr.h): one table per VM, immortal
 * entries, pointer equality implying content equality.  The old
 * open-addressing table in src/value/uintern.c is no longer built; this
 * header keeps the one declaration the emitter needs so none of the
 * src/emit/ translation units have to know where interning happens. */

#ifndef UINTERN_H
#define UINTERN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;

/* Intern bytes[0..nbytes) into vm's string table.
 *
 * On success: returns a canonical, NUL-terminated `const char *` whose
 * bytes match the input, stable for the lifetime of the UVM.  Two calls
 * with byte-equal inputs return the SAME pointer.  Returns NULL on OOM or
 * on a NULL argument; callers must check and propagate.
 *
 * Pointers are per-VM and must never be stored in serialized state —
 * re-intern from raw bytes on every load. */
const char *ustr_intern(struct UVM *vm, const char *bytes, size_t nbytes);

#ifdef __cplusplus
}
#endif

#endif /* UINTERN_H */
