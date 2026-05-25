/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_vm_create_opaque.c — Wave 4 W1: opaque VM allocation API */

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* Standard realloc-style allocator. */
static void *opaque_test_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

UTEST(vm_create_returns_non_null) {
    struct UVM *vm = urbi_vm_create(opaque_test_alloc, NULL);
    UASSERT_NE(vm, NULL);
    urbi_vm_free(vm);
}

UTEST(vm_create_returns_null_on_alloc_failure) {
    /* NULL alloc_fn — urbi_vm_create must return NULL immediately. */
    struct UVM *vm = urbi_vm_create(NULL, NULL);
    UASSERT_EQ(vm, NULL);
}

UTEST(vm_create_supports_global_realm) {
    struct UVM *vm = urbi_vm_create(opaque_test_alloc, NULL);
    UASSERT_NE(vm, NULL);
    struct URealm *realm = urbi_realm_global(vm);
    UASSERT_NE(realm, NULL);
    urbi_vm_free(vm);
}

UTEST(vm_sizeof_returns_positive) {
    size_t n = urbi_vm_sizeof();
    /* Must be positive. */
    UASSERT(n > 0);
    /* Sanity: in [4 KB, 1 MB] for any realistic build. */
    UASSERT(n > 4096);
    UASSERT(n < (1024 * 1024));
}

UTEST(vm_alignof_returns_power_of_two) {
    size_t a = urbi_vm_alignof();
    UASSERT(a > 0);
    UASSERT((a & (a - 1)) == 0);  /* power of two */
}

UTEST(vm_free_on_null_is_noop) {
    urbi_vm_free(NULL);   /* must not crash */
}

void test_vm_create_opaque_suite(void) {
    utest_run("vm_create_returns_non_null",              vm_create_returns_non_null);
    utest_run("vm_create_returns_null_on_alloc_failure", vm_create_returns_null_on_alloc_failure);
    utest_run("vm_create_supports_global_realm",         vm_create_supports_global_realm);
    utest_run("vm_sizeof_returns_positive",              vm_sizeof_returns_positive);
    utest_run("vm_alignof_returns_power_of_two",         vm_alignof_returns_power_of_two);
    utest_run("vm_free_on_null_is_noop",                 vm_free_on_null_is_noop);
}
