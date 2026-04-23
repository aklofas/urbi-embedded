/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode interpreter. Freestanding. */

#ifndef UVM_H
#define UVM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "uchunk.h"  /* Chunk, UConst, UValKind, UOpcode */

#ifdef __cplusplus
extern "C" {
#endif

/* --- errors --- */

typedef enum {
    UVM_OK = 0,
    UVM_TYPE_ERROR,
    UVM_OOM,
} UVMError;

/* --- pluggable allocator (matches uchunk pattern) --- */

typedef void *(*UVMAllocFn)(void *ptr, size_t nbytes, void *ud);
/* Standard realloc semantics:
 *   ptr == NULL && nbytes > 0  : allocate fresh buffer; return non-NULL or NULL on OOM.
 *   ptr != NULL && nbytes == 0 : free ptr; return NULL.
 *   ptr != NULL && nbytes > 0  : reallocate ptr to nbytes (may move); return non-NULL or NULL on OOM.
 *   ptr == NULL && nbytes == 0 : no-op; return NULL. */

/* --- VM state --- */

#define UVM_ERRMSG_CAP 128

typedef struct UVM {
    UVMAllocFn alloc_fn;
    void      *alloc_ud;
    UVMError   last_error;
    char       last_errmsg[UVM_ERRMSG_CAP];
} UVM;

/* --- API --- */

/* Initialize vm. On hosted builds, passing alloc_fn == NULL wires up a
   stdlib-realloc shim internally. On freestanding builds the caller MUST
   supply alloc_fn; if NULL is passed, uvm_init still returns (cannot fail
   at M1), but any subsequent uvm_run will NULL-deref in the frame
   allocation path — caller's bug. Zero-initializes last_error and
   last_errmsg. */
void uvm_init(UVM *vm, UVMAllocFn alloc_fn, void *alloc_ud);

/* Run chunk to completion. On UVM_OK, *out receives the RET value. On
   error, vm->last_error and vm->last_errmsg are populated and *out is
   set to UVAL_NIL (kind = UVAL_NIL, value payload zeroed). */
UVMError uvm_run(UVM *vm, const Chunk *chunk, UConst *out);

/* Free any VM-owned resources. Safe to call on a zero-initialized UVM. */
void uvm_destroy(UVM *vm);

/* Return a static string such as "UVM_TYPE_ERROR" for debug. */
const char *uvm_error_name(UVMError code);

#ifdef __cplusplus
}
#endif

#endif /* UVM_H */
