/* SPDX-License-Identifier: BSD-3-Clause */
/* Phase 13 / T145: urbi_lock_heap public C API test.
 *
 * Pre-lock allocations succeed; post-lock urbi_gc_alloc returns NULL.
 * Idempotency: a second urbi_lock_heap call is a no-op (still locked).
 * NULL-safety: urbi_lock_heap(NULL) does not crash. */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/gc.h"
#include "vm/uvm.h"
#include "gc/ugc.h"

#define UTEST(name) static void name(void)

UTEST(pre_lock_alloc_succeeds) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Pre-lock: a small generic-cell allocation must succeed. */
    UCell *c = urbi_gc_alloc(&vm, 32, /* type_tag */ 0xFF);
    UASSERT(c != NULL);

    urbi_vm_destroy(&vm);
}

UTEST(post_lock_alloc_returns_null) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Pre-lock allocation OK. */
    UCell *c1 = urbi_gc_alloc(&vm, 32, 0xFF);
    UASSERT(c1 != NULL);

    /* Lock the heap. */
    urbi_lock_heap(&vm);
    UASSERT(vm.heap_locked != 0);

    /* Post-lock allocation must return NULL.  Caller observes the
     * standard OOM-shaped failure mode the rest of the runtime
     * already handles. */
    UCell *c2 = urbi_gc_alloc(&vm, 32, 0xFF);
    UASSERT(c2 == NULL);

    urbi_vm_destroy(&vm);
}

UTEST(lock_is_idempotent) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    urbi_lock_heap(&vm);
    UASSERT(vm.heap_locked != 0);
    /* Second call is a no-op. */
    urbi_lock_heap(&vm);
    UASSERT(vm.heap_locked != 0);

    /* Still locked. */
    UASSERT(urbi_gc_alloc(&vm, 32, 0xFF) == NULL);

    urbi_vm_destroy(&vm);
}

UTEST(null_vm_is_no_op) {
    /* Must not crash. */
    urbi_lock_heap(NULL);
}

void
test_lock_heap_suite(void)
{
    utest_run("pre-lock urbi_gc_alloc succeeds",
              pre_lock_alloc_succeeds);
    utest_run("post-lock urbi_gc_alloc returns NULL",
              post_lock_alloc_returns_null);
    utest_run("urbi_lock_heap is idempotent",
              lock_is_idempotent);
    utest_run("urbi_lock_heap(NULL) is a no-op",
              null_vm_is_no_op);
}
