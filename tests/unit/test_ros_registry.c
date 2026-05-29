/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ros_registry.c — the ROS2 message type registry (URBI_ENABLE_ROS2). */
#ifdef URBI_ENABLE_ROS2

#include "utest.h"
#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "ros/uros_msg.h"
#include <stdint.h>
#include <stdlib.h>

static void *
ros_registry_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

/* urbi_ros_msg_register_all runs during urbi_realm_create (stdlib boot ->
 * urbi_ros_register).  The registry is queryable by ROS message name. */
static void
ros_registry_lookup_by_name(void)
{
    struct UVM *vm = urbi_vm_create(ros_registry_alloc, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);

    /* Idempotent re-register (clears + repopulates the proto table). */
    UASSERT_EQ(urbi_ros_msg_register_all(vm), 0);

    const URosMsgType *t = urbi_ros_msg_lookup("std_msgs/Int32");
    UASSERT_NE((long long)t, 0LL);
    if (t != NULL) {
        UASSERT_EQ((long long)t->c_size, (long long)sizeof(int32_t));
        UASSERT_NE((long long)t->marshal, 0LL);
        UASSERT_NE((long long)t->unmarshal, 0LL);
    }

    /* An unknown name returns NULL. */
    UASSERT_EQ((long long)urbi_ros_msg_lookup("std_msgs/DoesNotExist"), 0LL);

    urbi_vm_free(vm);
}

void
test_ros_registry_suite(void)
{
    utest_run("ros_registry.lookup_by_name", ros_registry_lookup_by_name);
}

#else  /* !URBI_ENABLE_ROS2 */

void test_ros_registry_suite(void) { /* skipped: URBI_ENABLE_ROS2=0 */ }

#endif /* URBI_ENABLE_ROS2 */
