/* SPDX-License-Identifier: BSD-3-Clause */
/* test_module_refcount — direct unit tests for the UProto / UProto refcount
 * mechanism.  After Task 11 (v0.8.1-uproto-root), UProto.refcount and
 * UProto.destroy_requested are deleted; refcount lives on root_proto only.
 * umodule_refcount_inc/dec (UProto-level) are deleted; callers use
 * uproto_refcount_inc/dec on root_proto directly. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "chunk/uchunk.h"
#include "vm/uvm.h"
#include "value/uarena.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* After UModule deletion, UProto IS the root.
 * Verify that a zero-initialized UProto has refcount == 0 and that
 * uproto_refcount_inc/dec operate directly on it. */
UTEST(refcount_lives_on_root_proto)
{
    UProto m = {0};
    UASSERT_EQ((unsigned)0, (unsigned)m.refcount);
    uproto_refcount_inc(&m);
    UASSERT_EQ((unsigned)1, (unsigned)m.refcount);
    uproto_refcount_dec(&m);
    UASSERT_EQ((unsigned)0, (unsigned)m.refcount);
}

/* uproto_root_of: returns proto itself when proto->root is NULL (root case),
 * and returns proto->root when set (nested case). */
UTEST(uproto_root_of_routing)
{
    UProto root = {0};
    UProto nested = {0};
    nested.root = &root;

    UASSERT(uproto_root_of(&root)   == &root);
    UASSERT(uproto_root_of(&nested) == &root);
    UASSERT(uproto_root_of(NULL)    == NULL);
}

/* Probe the rescue path: if root->refcount > 0 when uchunk_destroy is
 * called with a non-NULL vm, the root UProto is rescued to vm->rescued_protos.
 * vm_destroy then frees the rescued root. */
UTEST(umodule_destroy_rescues_when_refcount_nonzero)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* Allocate a root UProto on the heap.  heap_allocated=true so that
     * vm_destroy can free it via the alloc_fn free-path. */
    UProto *rp = (UProto *)vm.alloc_fn(NULL, sizeof(UProto), vm.alloc_ud);
    UASSERT(rp != NULL);
    memset(rp, 0, sizeof(UProto));
    rp->alloc_fn = vm.alloc_fn;
    rp->alloc_ud = vm.alloc_ud;
    rp->heap_allocated = true;

    /* Simulate a strand binding: bump root->refcount to 1. */
    uproto_refcount_inc(rp);
    UASSERT_EQ((unsigned)1, (unsigned)rp->refcount);

    /* vm->rescued_protos starts NULL. */
    UASSERT(vm.rescued_protos == NULL);

    uchunk_destroy(rp, &vm);  /* host releases ref — vm != NULL → rescue path */

    /* Rescue path: root must now be on vm->rescued_protos. */
    UASSERT(vm.rescued_protos == rp);

    /* urbi_vm_destroy must free rescued_protos (rp) cleanly — no leaks. */
    urbi_vm_destroy(&vm);
}

UTEST(umodule_destroy_immediate_when_refcount_zero)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UProto *m = (UProto *)vm.alloc_fn(NULL, sizeof(UProto), vm.alloc_ud);
    UASSERT(m != NULL);
    memset(m, 0, sizeof(UProto));

    /* refcount == 0 (no strand binding); uchunk_destroy frees sub-buffers
     * and zeroes the struct immediately (legacy behavior unchanged).
     * The UProto struct allocation itself is caller-owned; free it here. */
    uchunk_destroy(m, &vm);
    vm.alloc_fn(m, 0, vm.alloc_ud);  /* release the heap UProto struct */

    urbi_vm_destroy(&vm);
}

/* End-to-end: compile a minimal chunk, drive it via urbi_run_chunk,
 * verify the binding bump+decrement cycle leaves root_proto->refcount at zero. */
UTEST(refcount_bump_decrement_via_strand_binding)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena  arena;
    UProto module = {0};
    uarena_init(&arena, 4096);

    /* Compile + run a trivial chunk via urbi_run_chunk — the public path. */
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "42", NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* After the strand dies, the root UProto refcount is 0.  The module
     * is still valid (host owns it; auto-destroy was removed from the
     * strand-refcount-dec path).  refcount==0 is directly readable. */
    UASSERT_EQ((unsigned)0, (unsigned)module.refcount);

    uarena_destroy(&arena);
    uchunk_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

void test_module_refcount_suite(void) {
    utest_run("module_refcount: refcount lives on root_proto",
              refcount_lives_on_root_proto);
    utest_run("module_refcount: uproto_root_of routing",
              uproto_root_of_routing);
    utest_run("module_refcount: uchunk_destroy rescues when refcount nonzero",
              umodule_destroy_rescues_when_refcount_nonzero);
    utest_run("module_refcount: uchunk_destroy immediate when refcount zero",
              umodule_destroy_immediate_when_refcount_zero);
    utest_run("module_refcount: bump/decrement via strand binding",
              refcount_bump_decrement_via_strand_binding);
}
