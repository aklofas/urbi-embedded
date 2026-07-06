/* SPDX-License-Identifier: BSD-3-Clause */
/* === UTestHooks lifecycle (audit-1 F8, v0.10.4). === */

#include "runtime/utest_hooks.h"
#include "vm/uvm.h"
#include "runtime/umacros.h"  /* urbi_zero */

/* utest_hooks_create: allocate a zeroed UTestHooks.
 *
 * Called from urbi_vm_init.  All function pointer fields initialise to NULL
 * (hook disabled → real path taken by the callers in src/watcher/).
 * Returns NULL on OOM or when no allocator is wired (freestanding builds). */
UTestHooks *
utest_hooks_create(struct UVM *vm)
{
    if (vm == NULL || vm->alloc_fn == NULL) return NULL;

    UTestHooks *th = (UTestHooks *)vm->alloc_fn(NULL, sizeof(UTestHooks),
                                                  vm->alloc_ud);
    if (th == NULL) return NULL;
    urbi_zero(th, sizeof(UTestHooks));
    return th;
}

/* utest_hooks_destroy: free the UTestHooks wrapper.  NULL-tolerant. */
void
utest_hooks_destroy(struct UVM *vm, UTestHooks *th)
{
    if (th == NULL) return;
    if (vm == NULL || vm->alloc_fn == NULL) return;
    vm->alloc_fn(th, 0, vm->alloc_ud);
}
