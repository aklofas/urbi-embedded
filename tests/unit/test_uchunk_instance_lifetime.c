/* SPDX-License-Identifier: BSD-3-Clause */
/* Verify UChunkInstance is unlinked from vm->module_instances_head when its
 * UModule is destroyed (immediate-destroy path).  Latent-bug hunt per
 * spec §5.4.
 *
 * Two sub-tests:
 *   1. instance_unlinked_on_module_destroy  — single module; after destroy
 *      the instance must no longer appear on vm->module_instances_head.
 *   2. correct_instance_unlinked_multi      — two modules; destroy one;
 *      the other's instance must remain on the list. */

#include "utest.h"

#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

#include "urbi/urbi.h"
#include "chunk/uchunk.h"
#include "object/uchunk_instance.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* Walk vm->module_instances_head; return true if mi is present. */
static bool instance_on_vm_list(UVM *vm, UChunkInstance *mi)
{
    for (UChunkInstance *p = vm->module_instances_head;
         p != NULL; p = p->next_in_vm) {
        if (p == mi) return true;
    }
    return false;
}

/* Compile src → serialized bytecode (heap-allocated; caller must free). */
static int compile_to_bytes(UVM *vm, const char *src,
                             unsigned char **bc_out, size_t *bc_len_out)
{
    char err[256] = {0};
    return urbi_compile_source(vm, src, strlen(src), "t.u",
                               bc_out, bc_len_out, err, sizeof err);
}

/* -----------------------------------------------------------------------
 * Test 1: single module — instance must be gone after module destroy
 * ----------------------------------------------------------------------- */

UTEST(instance_unlinked_on_module_destroy)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    unsigned char *bc = NULL; size_t bc_len = 0;
    UASSERT_EQ(URBI_OK, compile_to_bytes(&vm, "1 + 2 |", &bc, &bc_len));

    UModule mod = {0};
    char errmsg[128];
    UASSERT_EQ(UCHUNK_LOAD_OK, uchunk_deserialize(&mod, bc, bc_len,
                                              errmsg, sizeof errmsg));

    UChunkInstance *mi = urbi_module_instance_create(&vm, &mod);
    UASSERT(mi != NULL);
    /* Precondition: instance is registered. */
    UASSERT(instance_on_vm_list(&vm, mi));

    /* Destroy the module — instance must be unlinked. */
    uchunk_destroy(&mod, &vm);

    UASSERT(!instance_on_vm_list(&vm, mi));

    free(bc);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Test 2: two modules — destroy one; other's instance must survive
 * ----------------------------------------------------------------------- */

UTEST(correct_instance_unlinked_multi)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    unsigned char *bc_a = NULL; size_t bc_a_len = 0;
    unsigned char *bc_b = NULL; size_t bc_b_len = 0;
    UASSERT_EQ(URBI_OK, compile_to_bytes(&vm, "1 + 2 |", &bc_a, &bc_a_len));
    UASSERT_EQ(URBI_OK, compile_to_bytes(&vm, "3 + 4 |", &bc_b, &bc_b_len));

    UModule mod_a = {0}, mod_b = {0};
    char errmsg[128];
    UASSERT_EQ(UCHUNK_LOAD_OK, uchunk_deserialize(&mod_a, bc_a, bc_a_len,
                                              errmsg, sizeof errmsg));
    UASSERT_EQ(UCHUNK_LOAD_OK, uchunk_deserialize(&mod_b, bc_b, bc_b_len,
                                              errmsg, sizeof errmsg));

    UChunkInstance *mi_a = urbi_module_instance_create(&vm, &mod_a);
    UChunkInstance *mi_b = urbi_module_instance_create(&vm, &mod_b);
    UASSERT(mi_a != NULL);
    UASSERT(mi_b != NULL);
    UASSERT(instance_on_vm_list(&vm, mi_a));
    UASSERT(instance_on_vm_list(&vm, mi_b));

    /* Destroy only mod_a. */
    uchunk_destroy(&mod_a, &vm);

    /* mi_a must be gone; mi_b must survive. */
    UASSERT(!instance_on_vm_list(&vm, mi_a));
    UASSERT(instance_on_vm_list(&vm, mi_b));

    uchunk_destroy(&mod_b, &vm);
    free(bc_a);
    free(bc_b);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Suite entry
 * ----------------------------------------------------------------------- */

void
test_uchunk_instance_lifetime_suite(void)
{
    utest_run("uchunk_instance_lifetime: instance unlinked on module destroy",
              instance_unlinked_on_module_destroy);
    utest_run("uchunk_instance_lifetime: correct instance unlinked (multi-module)",
              correct_instance_unlinked_multi);
}
