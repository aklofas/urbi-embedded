/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ulist_build.c — unit tests for the internal List C-builder API.
 *
 * Gated by URBI_ENABLE_ROS2: these helpers are used by the ROS2 bridge
 * to construct List objects from incoming sequence fields. */
#ifdef URBI_ENABLE_ROS2

#include "utest.h"
#include "urbi/urbi.h"
#include "value/ulist_build.h"
#include <stdlib.h>

static void *
ulist_test_alloc(void *ptr, size_t n, void *ud)
{
    (void)ud;
    if (n == 0) { free(ptr); return NULL; }
    return realloc(ptr, n);
}

/* Create a list, append two integer values, verify length == 2. */
static void
ulist_build_create_append_roundtrip(void)
{
    struct UVM *vm = urbi_vm_create(ulist_test_alloc, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);
    if (realm == NULL) { urbi_vm_free(vm); return; }

    UValue lst = urbi_list_create(vm);
    UASSERT(lst.kind != 0);   /* must not be nil */

    UASSERT_EQ(urbi_list_append(vm, lst, urbi_make_int(7)), 0);
    UASSERT_EQ(urbi_list_append(vm, lst, urbi_make_int(8)), 0);
    UASSERT_EQ(urbi_list_len(vm, lst), 2);

    urbi_vm_free(vm);
}

void
test_ulist_build_suite(void)
{
    utest_run("ulist_build.create_append_roundtrip",
              ulist_build_create_append_roundtrip);
}

#else  /* !URBI_ENABLE_ROS2 */

void test_ulist_build_suite(void) { /* skipped: URBI_ENABLE_ROS2=0 */ }

#endif /* URBI_ENABLE_ROS2 */
