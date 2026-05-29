/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_runtime_typed_throw.c — cached Exception-subclass protos.
 *
 * The exception_subclasses.u bake run installs TypeError / ArityError /
 * LookupError / OutOfMemoryError as realm globals; the resolver then
 * caches each as a vm->*error_proto field so C raise sites can clone a
 * typed instance without a realm handle.  This test proves all four
 * resolve to non-NULL after the first realm completes population. */

#include "utest.h"
#include "urbi/urbi.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* ==== Test: error_protos_resolved =======================================
 *
 * After the global realm completes its bake-blob run (triggered by any
 * trivial eval), the four cached Exception-subclass protos must be
 * non-NULL. */
UTEST(error_protos_resolved)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Force global-realm population (runs the stdlib bake-blob that
     * installs the Exception subclasses, then the new resolver). */
    int rc = urbi_repl_eval(&vm, NULL, "1", 1, NULL, 0);
    UASSERT_EQ(rc, URBI_OK);

    UASSERT(vm.typeerror_proto   != NULL);
    UASSERT(vm.arityerror_proto  != NULL);
    UASSERT(vm.lookuperror_proto != NULL);
    UASSERT(vm.oomerror_proto    != NULL);

    urbi_vm_destroy(&vm);
}

/* ---- suite entry point ------------------------------------------------- */
void test_runtime_typed_throw_suite(void);

void
test_runtime_typed_throw_suite(void)
{
    printf("test_runtime_typed_throw\n");
    utest_run("error_protos_resolved", error_protos_resolved);
}
