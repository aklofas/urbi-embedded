/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ros_proto.c — the `ros` namespace proto is installed under URBI_ENABLE_ROS2. */
#ifdef URBI_ENABLE_ROS2

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/ros.h"
#include "vm/uvm.h"
#include <stdlib.h>

/* Standard realloc-style allocator used by urbi_vm_create. */
static void *
ros_test_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

static void
ros_proto_registered(void)
{
    struct UVM *vm = urbi_vm_create(ros_test_alloc, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);
    UASSERT_NE((long long)vm->ros_proto, 0LL);
    urbi_vm_free(vm);
}

void
test_ros_proto_suite(void)
{
    utest_run("ros_proto.registered", ros_proto_registered);
}

#else  /* !URBI_ENABLE_ROS2 */

void test_ros_proto_suite(void) { /* skipped: URBI_ENABLE_ROS2=0 */ }

#endif /* URBI_ENABLE_ROS2 */
