/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ros_marshal.c — message-object <-> C-struct marshaling (URBI_ENABLE_ROS2). */
#ifdef URBI_ENABLE_ROS2

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "urbi/object.h"        /* UObject, urbi_object_root */
#include "object/uobject.h"     /* urbi_object_clone */
#include "value/ulist_build.h"  /* urbi_list_create, urbi_list_append, urbi_list_len, urbi_list_get */
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
int urbi_ros_unmarshal__std_msgs__String(struct UVM *,
                                          const struct urbi_ros__std_msgs__String *,
                                          UValue *);
int urbi_ros_marshal__std_msgs__String(struct UVM *, UValue,
                                        struct urbi_ros__std_msgs__String *);
int urbi_ros_unmarshal__sensor_msgs__LaserScan(struct UVM *,
                                                const struct urbi_ros__sensor_msgs__LaserScan *,
                                                UValue *);
int urbi_ros_marshal__sensor_msgs__LaserScan(struct UVM *, UValue,
                                              struct urbi_ros__sensor_msgs__LaserScan *);
int urbi_ros_unmarshal__sensor_msgs__Range(struct UVM *,
                                            const struct urbi_ros__sensor_msgs__Range *,
                                            UValue *);
int urbi_ros_marshal__sensor_msgs__Range(struct UVM *, UValue,
                                          struct urbi_ros__sensor_msgs__Range *);

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

/* std_msgs/String round-trip: marshal string slot -> C struct, then unmarshal
 * back -> object, assert the string value is preserved. */
static void
ros_marshal_string_roundtrip(void)
{
    struct UVM *vm = urbi_vm_create(ros_marshal_alloc, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);

    /* Build a String message object with data = "hello" */
    UObject *o = urbi_object_clone(vm, urbi_object_root(vm));
    UASSERT_NE((long long)o, 0LL);
    UValue obj = urbi_make_object(o);
    UValue sv  = urbi_make_str_interned(vm, "hello", 5);
    UASSERT_EQ(urbi_slot_set(vm, obj, "data", 4, sv), 0);

    /* Marshal object -> C struct */
    struct urbi_ros__std_msgs__String cs;
    memset(&cs, 0, sizeof(cs));
    UASSERT_EQ(urbi_ros_marshal__std_msgs__String(vm, obj, &cs), 0);
    UASSERT_EQ(strcmp(cs.data, "hello"), 0);

    /* Unmarshal C struct -> object */
    UValue obj2 = urbi_make_nil();
    UASSERT_EQ(urbi_ros_unmarshal__std_msgs__String(vm, &cs, &obj2), 0);
    UValue dv = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get(vm, obj2, "data", 4, &dv), 0);
    UASSERT(urbi_value_is_str(dv));
    { size_t slen = 0;
      const char *s = urbi_value_as_str(dv, &slen);
      UASSERT_EQ(strcmp(s, "hello"), 0);
      (void)slen; }

    urbi_vm_free(vm);
}

/* sensor_msgs/LaserScan round-trip: nested Header.frame_id string + float32[]
 * sequence in ranges.  Build the C struct, unmarshal to object, verify
 * frame_id string and ranges list length + element values, then marshal back. */
static void
ros_marshal_laserscan_roundtrip(void)
{
    struct UVM *vm = urbi_vm_create(ros_marshal_alloc, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);

    /* Build a LaserScan C struct */
    struct urbi_ros__sensor_msgs__LaserScan scan0;
    memset(&scan0, 0, sizeof(scan0));
    strncpy(scan0.header.frame_id, "laser_frame", 255);
    scan0.header.stamp.sec     = 42;
    scan0.header.stamp.nanosec = 500000000U;
    scan0.angle_min       = -1.5f;
    scan0.angle_max       =  1.5f;
    scan0.range_min       =  0.1f;
    scan0.range_max       = 30.0f;
    scan0.ranges.data[0]  = 1.0f;
    scan0.ranges.data[1]  = 2.5f;
    scan0.ranges.data[2]  = 3.75f;
    scan0.ranges.size     = 3U;
    /* intensities: leave empty (size=0) */

    /* Unmarshal C struct -> object */
    UValue obj = urbi_make_nil();
    UASSERT_EQ(urbi_ros_unmarshal__sensor_msgs__LaserScan(vm, &scan0, &obj), 0);

    /* Verify header.frame_id */
    UValue hdr_v = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get(vm, obj, "header", 6, &hdr_v), 0);
    UValue fid_v = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get(vm, hdr_v, "frame_id", 8, &fid_v), 0);
    UASSERT(urbi_value_is_str(fid_v));
    { size_t slen = 0;
      const char *s = urbi_value_as_str(fid_v, &slen);
      UASSERT_EQ(strcmp(s, "laser_frame"), 0);
      (void)slen; }

    /* Verify ranges list length and element values */
    UValue rng_v = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get(vm, obj, "ranges", 6, &rng_v), 0);
    UASSERT_EQ(urbi_list_len(vm, rng_v), 3);
    { UValue e0 = urbi_list_get(vm, rng_v, 0);
      UValue e1 = urbi_list_get(vm, rng_v, 1);
      UValue e2 = urbi_list_get(vm, rng_v, 2);
      UASSERT(urbi_value_as_float(e0) == 1.0);
      UASSERT(urbi_value_as_float(e1) == 2.5);
      UASSERT(urbi_value_as_float(e2) == 3.75); }

    /* Verify intensities list is empty */
    UValue int_v = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get(vm, obj, "intensities", 11, &int_v), 0);
    UASSERT_EQ(urbi_list_len(vm, int_v), 0);

    /* Marshal object back -> C struct and check ranges round-trip */
    struct urbi_ros__sensor_msgs__LaserScan scan1;
    memset(&scan1, 0, sizeof(scan1));
    UASSERT_EQ(urbi_ros_marshal__sensor_msgs__LaserScan(vm, obj, &scan1), 0);
    UASSERT_EQ(strcmp(scan1.header.frame_id, "laser_frame"), 0);
    UASSERT_EQ((int)scan1.ranges.size, 3);
    UASSERT(scan1.ranges.data[0] == 1.0f);
    UASSERT(scan1.ranges.data[1] == 2.5f);
    UASSERT(scan1.ranges.data[2] == 3.75f);
    UASSERT_EQ((int)scan1.intensities.size, 0);

    urbi_vm_free(vm);
}

/* sensor_msgs/Range round-trip: nested Header.frame_id string + scalar fields
 * (including uint8 radiation_type).  Build the C struct, unmarshal to object,
 * verify radiation_type + range value, then marshal back. */
static void
ros_marshal_range_roundtrip(void)
{
    struct UVM *vm = urbi_vm_create(ros_marshal_alloc, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);

    /* Build a Range C struct */
    struct urbi_ros__sensor_msgs__Range rng0;
    memset(&rng0, 0, sizeof(rng0));
    strncpy(rng0.header.frame_id, "sonar_frame", 255);
    rng0.header.stamp.sec      = 10;
    rng0.header.stamp.nanosec  = 0U;
    rng0.radiation_type        = 1U;   /* ULTRASOUND = 1 */
    rng0.field_of_view         = 0.5f;
    rng0.min_range             = 0.02f;
    rng0.max_range             = 4.0f;
    rng0.range                 = 1.5f;

    /* Unmarshal C struct -> object */
    UValue obj = urbi_make_nil();
    UASSERT_EQ(urbi_ros_unmarshal__sensor_msgs__Range(vm, &rng0, &obj), 0);

    /* Verify header.frame_id */
    UValue hdr_v = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get(vm, obj, "header", 6, &hdr_v), 0);
    UValue fid_v = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get(vm, hdr_v, "frame_id", 8, &fid_v), 0);
    UASSERT(urbi_value_is_str(fid_v));
    { size_t slen = 0;
      const char *s = urbi_value_as_str(fid_v, &slen);
      UASSERT_EQ(strcmp(s, "sonar_frame"), 0);
      (void)slen; }

    /* Verify radiation_type and range scalar fields */
    UValue rt_v = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get(vm, obj, "radiation_type", 14, &rt_v), 0);
    UASSERT_EQ(urbi_value_as_int(rt_v), 1LL);

    UValue range_v = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get(vm, obj, "range", 5, &range_v), 0);
    UASSERT(urbi_value_as_float(range_v) == 1.5);

    /* Marshal object back -> C struct and check round-trip */
    struct urbi_ros__sensor_msgs__Range rng1;
    memset(&rng1, 0, sizeof(rng1));
    UASSERT_EQ(urbi_ros_marshal__sensor_msgs__Range(vm, obj, &rng1), 0);
    UASSERT_EQ(strcmp(rng1.header.frame_id, "sonar_frame"), 0);
    UASSERT_EQ((int)rng1.radiation_type, 1);
    UASSERT(rng1.range == 1.5f);
    UASSERT(rng1.min_range == 0.02f);
    UASSERT(rng1.max_range == 4.0f);

    urbi_vm_free(vm);
}

void
test_ros_marshal_suite(void)
{
    utest_run("ros_marshal.int32_reads_slot",     ros_marshal_int32_reads_slot);
    utest_run("ros_marshal.twist_roundtrip",      ros_marshal_twist_roundtrip);
    utest_run("ros_marshal.string_roundtrip",     ros_marshal_string_roundtrip);
    utest_run("ros_marshal.laserscan_roundtrip",  ros_marshal_laserscan_roundtrip);
    utest_run("ros_marshal.range_roundtrip",      ros_marshal_range_roundtrip);
}

#else  /* !URBI_ENABLE_ROS2 */

void test_ros_marshal_suite(void) { /* skipped: URBI_ENABLE_ROS2=0 */ }

#endif /* URBI_ENABLE_ROS2 */
