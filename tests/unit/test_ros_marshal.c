/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ros_marshal.c — message-object <-> C-struct marshaling (URBI_ENABLE_ROS2). */
#ifdef URBI_ENABLE_ROS2

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "urbi/object.h"        /* UObject, urbi_object_root */
#include "object/uobject.h"     /* urbi_object_clone */
#include "ros/generated/ros_msgs.gen.h"
#include <stdlib.h>
#include <string.h>

/* Forward decls of the generated marshal/unmarshal entry points used here. */
int urbi_ros_marshal__std_msgs__Int32(struct UVM *, UValue,
                                       struct urbi_ros__std_msgs__Int32 *);
int urbi_ros_unmarshal__geometry_msgs__Twist(struct UVM *,
                                             const struct urbi_ros__geometry_msgs__Twist *,
                                             UValue *);
int urbi_ros_marshal__geometry_msgs__Twist(struct UVM *, UValue,
                                            struct urbi_ros__geometry_msgs__Twist *);

static void *
ros_marshal_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

/* A scalar slot read by the generated marshal lands in the C struct. */
static void
ros_marshal_int32_reads_slot(void)
{
    struct UVM *vm = urbi_vm_create(ros_marshal_alloc, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);

    UObject *o = urbi_object_clone(vm, urbi_object_root(vm));
    UASSERT_NE((long long)o, 0LL);
    UValue obj = urbi_make_object(o);
    UASSERT_EQ(urbi_slot_set(vm, obj, "data", 4, urbi_make_int(99)), 0);

    struct urbi_ros__std_msgs__Int32 c;
    c.data = 0;
    UASSERT_EQ(urbi_ros_marshal__std_msgs__Int32(vm, obj, &c), 0);
    UASSERT_EQ((long long)c.data, 99LL);

    urbi_vm_free(vm);
}

/* unmarshal (C -> object) then marshal (object -> C) round-trips a nested
 * message (Twist contains two Vector3). */
static void
ros_marshal_twist_roundtrip(void)
{
    struct UVM *vm = urbi_vm_create(ros_marshal_alloc, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);

    struct urbi_ros__geometry_msgs__Twist t0;
    memset(&t0, 0, sizeof(t0));
    t0.linear.x = 1.5;
    t0.angular.z = 9.0;

    UValue obj = urbi_make_nil();
    UASSERT_EQ(urbi_ros_unmarshal__geometry_msgs__Twist(vm, &t0, &obj), 0);

    struct urbi_ros__geometry_msgs__Twist t1;
    memset(&t1, 0, sizeof(t1));
    UASSERT_EQ(urbi_ros_marshal__geometry_msgs__Twist(vm, obj, &t1), 0);

    UASSERT(t1.linear.x == 1.5);
    UASSERT(t1.angular.z == 9.0);

    urbi_vm_free(vm);
}

void
test_ros_marshal_suite(void)
{
    utest_run("ros_marshal.int32_reads_slot", ros_marshal_int32_reads_slot);
    utest_run("ros_marshal.twist_roundtrip",  ros_marshal_twist_roundtrip);
}

#else  /* !URBI_ENABLE_ROS2 */

void test_ros_marshal_suite(void) { /* skipped: URBI_ENABLE_ROS2=0 */ }

#endif /* URBI_ENABLE_ROS2 */
