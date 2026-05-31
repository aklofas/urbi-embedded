/* SPDX-License-Identifier: BSD-3-Clause */
/* driver_marshal.c — B3 integration driver.  Exercises the rosidl-targeting
 * codegen (marshal_rcl / unmarshal_rcl) against the REAL rosidl structs:
 * builds an object, marshal_rcl -> rosidl struct, unmarshal_rcl -> object,
 * asserts the round-trip for Int32 (scalar), String (rosidl String), and
 * LaserScan (float Sequence + nested Header/Time).  Prints "RCLMARSHAL ok". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "urbi/object.h"
#include "object/uobject.h"
#include "value/ulist_build.h"
#include "ros/uros_msg.h"
#include "ros/generated/ros_msgs_rcl.gen.h"

#include <rosidl_runtime_c/string_functions.h>
#include <rosidl_runtime_c/primitives_sequence_functions.h>
#include <std_msgs/msg/int32.h>
#include <std_msgs/msg/string.h>
#include <sensor_msgs/msg/laser_scan.h>

int urbi_ros_marshal_rcl__std_msgs__Int32(struct UVM *, UValue, std_msgs__msg__Int32 *);
int urbi_ros_unmarshal_rcl__std_msgs__Int32(struct UVM *, const std_msgs__msg__Int32 *, UValue *);
int urbi_ros_marshal_rcl__std_msgs__String(struct UVM *, UValue, std_msgs__msg__String *);
int urbi_ros_unmarshal_rcl__std_msgs__String(struct UVM *, const std_msgs__msg__String *, UValue *);
int urbi_ros_marshal_rcl__sensor_msgs__LaserScan(struct UVM *, UValue, sensor_msgs__msg__LaserScan *);
int urbi_ros_unmarshal_rcl__sensor_msgs__LaserScan(struct UVM *, const sensor_msgs__msg__LaserScan *, UValue *);

static void *
drv_alloc(void *ptr, size_t n, void *ud)
{
    (void)ud;
    if (n == 0) { free(ptr); return NULL; }
    return realloc(ptr, n);
}

#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "RCLMARSHAL FAIL: %s\n", msg); return 1; } } while (0)

int
main(void)
{
    struct UVM *vm = urbi_vm_create(drv_alloc, NULL);
    CHECK(vm != NULL, "vm_create");
    struct URealm *realm = urbi_realm_global(vm);
    CHECK(realm != NULL, "realm");
    /* Register the message protos so unmarshal_rcl's urbi_ros_msg_alloc works. */
    CHECK(urbi_ros_msg_register_all(vm) == 0, "msg_register_all");

    /* --- Int32 scalar round-trip --- */
    {
        UObject *o = urbi_object_clone(vm, urbi_object_root(vm));
        CHECK(o != NULL, "int32 clone");
        UValue obj = urbi_make_object(o);
        CHECK(urbi_slot_set(vm, obj, "data", 4, urbi_make_int(123)) == 0, "int32 set");

        std_msgs__msg__Int32 c;
        std_msgs__msg__Int32__init(&c);
        CHECK(urbi_ros_marshal_rcl__std_msgs__Int32(vm, obj, &c) == 0, "int32 marshal");
        CHECK(c.data == 123, "int32 value");

        UValue back = urbi_make_nil();
        CHECK(urbi_ros_unmarshal_rcl__std_msgs__Int32(vm, &c, &back) == 0, "int32 unmarshal");
        UValue dv = urbi_make_nil();
        CHECK(urbi_slot_get(vm, back, "data", 4, &dv) == 0, "int32 get-back");
        CHECK(urbi_value_as_int(dv) == 123, "int32 roundtrip");
        std_msgs__msg__Int32__fini(&c);
    }

    /* --- String round-trip via rosidl String --- */
    {
        UObject *o = urbi_object_clone(vm, urbi_object_root(vm));
        CHECK(o != NULL, "string clone");
        UValue obj = urbi_make_object(o);
        UValue sv = urbi_make_str_interned(vm, "hello rcl", 9);
        CHECK(urbi_slot_set(vm, obj, "data", 4, sv) == 0, "string set");

        std_msgs__msg__String c;
        std_msgs__msg__String__init(&c);
        CHECK(urbi_ros_marshal_rcl__std_msgs__String(vm, obj, &c) == 0, "string marshal");
        CHECK(c.data.data != NULL && strcmp(c.data.data, "hello rcl") == 0, "string value");

        UValue back = urbi_make_nil();
        CHECK(urbi_ros_unmarshal_rcl__std_msgs__String(vm, &c, &back) == 0, "string unmarshal");
        UValue dv = urbi_make_nil();
        CHECK(urbi_slot_get(vm, back, "data", 4, &dv) == 0, "string get-back");
        size_t sl = 0; const char *s = urbi_value_as_str(dv, &sl);
        CHECK(s != NULL && strcmp(s, "hello rcl") == 0, "string roundtrip");
        std_msgs__msg__String__fini(&c);
    }

    /* --- LaserScan: float Sequence + nested Header.frame_id string + Time --- */
    {
        sensor_msgs__msg__LaserScan c0;
        sensor_msgs__msg__LaserScan__init(&c0);
        rosidl_runtime_c__String__assign(&c0.header.frame_id, "laser");
        c0.header.stamp.sec = 7;
        c0.header.stamp.nanosec = 250000000U;
        rosidl_runtime_c__float__Sequence__fini(&c0.ranges);
        rosidl_runtime_c__float__Sequence__init(&c0.ranges, 3);
        c0.ranges.data[0] = 1.0f; c0.ranges.data[1] = 2.5f; c0.ranges.data[2] = 3.75f;

        UValue obj = urbi_make_nil();
        CHECK(urbi_ros_unmarshal_rcl__sensor_msgs__LaserScan(vm, &c0, &obj) == 0, "scan unmarshal");

        UValue hdr = urbi_make_nil();
        CHECK(urbi_slot_get(vm, obj, "header", 6, &hdr) == 0, "scan header");
        UValue fid = urbi_make_nil();
        CHECK(urbi_slot_get(vm, hdr, "frame_id", 8, &fid) == 0, "scan frame_id");
        size_t fl = 0; const char *fs = urbi_value_as_str(fid, &fl);
        CHECK(fs != NULL && strcmp(fs, "laser") == 0, "scan frame_id value");

        UValue rng = urbi_make_nil();
        CHECK(urbi_slot_get(vm, obj, "ranges", 6, &rng) == 0, "scan ranges");
        CHECK(urbi_list_len(vm, rng) == 3, "scan ranges len");
        CHECK(urbi_value_as_float(urbi_list_get(vm, rng, 2)) == 3.75, "scan ranges[2]");

        /* Marshal back into a fresh rosidl struct and verify the sequence. */
        sensor_msgs__msg__LaserScan c1;
        sensor_msgs__msg__LaserScan__init(&c1);
        CHECK(urbi_ros_marshal_rcl__sensor_msgs__LaserScan(vm, obj, &c1) == 0, "scan marshal");
        CHECK(c1.ranges.size == 3, "scan marshal ranges size");
        CHECK(c1.ranges.data[1] == 2.5f, "scan marshal ranges[1]");
        CHECK(strcmp(c1.header.frame_id.data, "laser") == 0, "scan marshal frame_id");
        sensor_msgs__msg__LaserScan__fini(&c1);
        sensor_msgs__msg__LaserScan__fini(&c0);
    }

    printf("RCLMARSHAL ok\n");
    urbi_vm_free(vm);
    return 0;
}
