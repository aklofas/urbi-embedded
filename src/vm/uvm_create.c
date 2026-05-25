/* SPDX-License-Identifier: BSD-3-Clause */
/* src/vm/uvm_create.c — opaque VM allocation API (W1, v0.10.3).
 *
 * urbi_vm_create allocates storage for struct UVM via the supplied allocator,
 * then defers to urbi_vm_init for initialisation.  urbi_vm_free is the
 * symmetric teardown + free.  urbi_vm_sizeof / urbi_vm_alignof expose layout
 * to embedders who want static/BSS allocation without including vm/uvm.h. */

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"

#include <stddef.h>

struct UVM *urbi_vm_create(UVMAllocFn alloc_fn, void *alloc_ud)
{
    if (alloc_fn == NULL) return NULL;

    void *storage = alloc_fn(NULL, sizeof(struct UVM), alloc_ud);
    if (storage == NULL) return NULL;

    struct UVM *vm = (struct UVM *)storage;
    int rc = urbi_vm_init(vm, alloc_fn, alloc_ud);
    if (rc != URBI_OK) {
        alloc_fn(storage, 0, alloc_ud);
        return NULL;
    }
    return vm;
}

void urbi_vm_free(struct UVM *vm)
{
    if (vm == NULL) return;
    /* Capture allocator locally so the storage-free below uses stable
     * pointers — defensive even though urbi_vm_destroy() currently leaves
     * vm->alloc_fn intact (a future destroy may zero or trample fields). */
    UVMAllocFn alloc_fn = vm->alloc_fn;
    void *alloc_ud      = vm->alloc_ud;
    urbi_vm_destroy(vm);
    if (alloc_fn != NULL) {
        alloc_fn(vm, 0, alloc_ud);
    }
}

size_t urbi_vm_sizeof(void)
{
    return sizeof(struct UVM);
}

size_t urbi_vm_alignof(void)
{
    return __alignof__(struct UVM);
}
