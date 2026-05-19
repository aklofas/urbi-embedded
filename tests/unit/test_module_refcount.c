/* SPDX-License-Identifier: BSD-3-Clause */
/* test_module_refcount — direct unit tests for the UModule / UProto refcount
 * mechanism.  After Task 11 (v0.8.1-uproto-root), UModule.refcount and
 * UModule.destroy_requested are deleted; refcount lives on root_proto only.
 * umodule_refcount_inc/dec (UModule-level) are deleted; callers use
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

/* Task 11: UModule is now a thin 5-field shell.
 * Verify that a zero-initialized UModule has root_proto == NULL
 * and that root_proto carries the refcount. */
UTEST(refcount_lives_on_root_proto)
{
    UModule m = {0};
    UASSERT(m.root_proto == NULL);
    /* Allocate a synthetic root_proto and verify its refcount. */
    UProto rp = {0};
    m.root_proto = &rp;
    UASSERT_EQ((unsigned)0, (unsigned)rp.refcount);
    uproto_refcount_inc(&rp);
    UASSERT_EQ((unsigned)1, (unsigned)rp.refcount);
    uproto_refcount_dec(&rp);
    UASSERT_EQ((unsigned)0, (unsigned)rp.refcount);
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

/* Probe the rescue path: if root_proto->refcount > 0 when uchunk_destroy is
 * called with a non-NULL vm, the root_proto is rescued to vm->rescued_protos.
 * vm_destroy then frees the rescued root_proto. */
UTEST(umodule_destroy_rescues_when_refcount_nonzero)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* Allocate a minimal UModule + a synthetic root_proto on the heap. */
    UModule *m = (UModule *)vm.alloc_fn(NULL, sizeof(UModule), vm.alloc_ud);
    UASSERT(m != NULL);
    memset(m, 0, sizeof(UModule));

    /* Allocate a synthetic root_proto and attach it to the module.
     * Set alloc_fn so the rescue-then-vm_destroy path can free it. */
    UProto *rp = (UProto *)vm.alloc_fn(NULL, sizeof(UProto), vm.alloc_ud);
    UASSERT(rp != NULL);
    memset(rp, 0, sizeof(UProto));
    rp->alloc_fn = vm.alloc_fn;
    rp->alloc_ud = vm.alloc_ud;
    m->root_proto = rp;

    /* Simulate a strand binding: bump root_proto->refcount to 1. */
    uproto_refcount_inc(rp);
    UASSERT_EQ((unsigned)1, (unsigned)rp->refcount);

    /* vm->rescued_protos starts NULL. */
    UASSERT(vm.rescued_protos == NULL);

    uchunk_destroy(m, &vm);  /* host releases ref — vm != NULL → rescue path */

    /* Task 9 rescue path: root_proto must now be on vm->rescued_protos. */
    UASSERT(vm.rescued_protos == rp);
    /* module->root_proto detached. */
    UASSERT(m->root_proto == NULL);

    /* Release the heap UModule struct (zeroed by uchunk_destroy_internal). */
    vm.alloc_fn(m, 0, vm.alloc_ud);

    /* urbi_vm_destroy must free rescued_protos (rp) cleanly — no leaks. */
    urbi_vm_destroy(&vm);
}

UTEST(umodule_destroy_immediate_when_refcount_zero)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UModule *m = (UModule *)vm.alloc_fn(NULL, sizeof(UModule), vm.alloc_ud);
    UASSERT(m != NULL);
    memset(m, 0, sizeof(UModule));

    /* refcount == 0 (no strand binding); uchunk_destroy frees sub-buffers
     * and zeroes the struct immediately (legacy behavior unchanged).
     * The UModule struct allocation itself is caller-owned; free it here. */
    uchunk_destroy(m, &vm);
    vm.alloc_fn(m, 0, vm.alloc_ud);  /* release the heap UModule struct */

    urbi_vm_destroy(&vm);
}

/* End-to-end: compile a minimal chunk, drive it via urbi_run_chunk,
 * verify the binding bump+decrement cycle leaves root_proto->refcount at zero. */
UTEST(refcount_bump_decrement_via_strand_binding)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    /* Compile + run a trivial chunk via urbi_run_chunk — the public path. */
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "42", NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* After the strand dies, root_proto->refcount is 0.  The module shell
     * is still valid (host owns it; auto-destroy was removed from the
     * strand-refcount-dec path).  refcount==0 is directly readable. */
    UASSERT(module.root_proto != NULL);
    UASSERT_EQ((unsigned)0, (unsigned)module.root_proto->refcount);

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
