/* SPDX-License-Identifier: BSD-3-Clause */
/* driver_binding.c — live-DDS facet<->ROS2 binding integration driver.
 *
 * Proves the urobotics Standard-Robotics-API facet<->ROS2 binding contract
 * (Robotics.bindInput / Robotics.bindOutput) over the real rcl/rclc/Fast-DDS
 * transport, not the mock.  In one process:
 *
 *   urbiscript side (the "robot"):
 *     - ros.init brings up the urbi rcl transport node.
 *     - a DistanceSensor facet binds its `distance` slot to /range
 *       (sensor_msgs/Range.range) via Robotics.bindInput.
 *     - a Motor facet binds its `val` slot to /cmd (std_msgs/Float64.data)
 *       via Robotics.bindOutput at a 50ms periodic publish rate.
 *     - a reactive safety behavior: at (sensor.distance < 0.5) motor.val = 0.0.
 *       motor.val starts at 1.0 (full speed) and sensor.distance at 1.0
 *       (clear), so the watcher starts non-firing.
 *
 *   companion side (a separate raw rclc node in the same process):
 *     - publishes a sensor_msgs/Range with range = 0.3 (below threshold) on
 *       /range, periodically, so the urbi subscription receives it over DDS.
 *     - subscribes /cmd (std_msgs/Float64) and records the last value seen.
 *
 * The pump loop drives urbi_step (which spins the urbi transport and fires
 * the periodic publisher + the reactive `at` watcher at a bytecode safepoint)
 * and the companion executor.  Once /range drives sensor.distance below 0.5,
 * the watcher sets motor.val = 0.0; the next periodic publishNow() marshals
 * 0.0 onto /cmd, and the companion observes it.  Asserts the last /cmd value
 * settles to 0.0.  Prints "BINDING ok" on success.
 *
 * std_msgs/Float64.data is a float64; the bindOutput marshaler requires a
 * FLOAT literal (an int marshals as garbage), hence motor.val = 1.0 / 0.0.
 */
#define _POSIX_C_SOURCE 199309L   /* nanosleep / struct timespec under -std=c99 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "urbi/urbi.h"

#include <rclc/rclc.h>
#include <rclc/executor.h>
#include <rcl/rcl.h>
#include <sensor_msgs/msg/range.h>
#include <std_msgs/msg/float64.h>

static void *
drv_alloc(void *ptr, size_t n, void *ud)
{
    (void)ud;
    if (n == 0) { free(ptr); return NULL; }
    return realloc(ptr, n);
}

static int
ev(struct UVM *vm, struct URealm *r, const char *src, char *buf, size_t bn)
{
    return urbi_repl_eval(vm, r, src, strlen(src), buf, bn);
}

/* Companion subscriber state: last /cmd value observed + a seen flag. */
static int    g_cmd_seen = 0;
static double g_cmd_last = -1.0;

static void
cmd_callback(const void *msg_in)
{
    const std_msgs__msg__Float64 *msg = (const std_msgs__msg__Float64 *)msg_in;
    g_cmd_seen = 1;
    g_cmd_last = msg->data;
}

#define FAIL(...) do { fprintf(stderr, "BINDING FAIL: " __VA_ARGS__); return 1; } while (0)

int
main(void)
{
    char buf[256];
    int rc;

    /* === urbiscript robot side === */
    struct UVM *vm = urbi_vm_create(drv_alloc, NULL);
    if (vm == NULL) FAIL("vm\n");
    struct URealm *realm = urbi_realm_global(vm);
    if (realm == NULL) FAIL("realm\n");

    rc = ev(vm, realm, "ros.init(\"urbi_robot\")", buf, sizeof buf);
    if (rc != 0) FAIL("ros.init rc=%d %s\n", rc, buf);

    /* DistanceSensor + Motor facets; both slots start "clear" / "full speed".
     * Mirror the chk fixture's `;`-separated statement form. */
    rc = ev(vm, realm,
            "var sensor = Robotics.DistanceSensor.clone(); sensor.distance = 1.0; "
            "var motor = Robotics.Motor.clone(); motor.val = 1.0;",
            buf, sizeof buf);
    if (rc != 0) FAIL("facets rc=%d %s\n", rc, buf);

    /* bindInput: /range (sensor_msgs/Range.range) -> sensor.distance. */
    rc = ev(vm, realm,
            "var insub = Robotics.bindInput(sensor, \"distance\", \"/range\","
            " \"sensor_msgs/Range\", \"range\");",
            buf, sizeof buf);
    if (rc != 0) FAIL("bindInput rc=%d %s\n", rc, buf);

    /* bindOutput: motor.val -> /cmd (std_msgs/Float64.data), 50ms periodic. */
    rc = ev(vm, realm,
            "var out = Robotics.bindOutput(motor, \"val\", \"/cmd\","
            " \"std_msgs/Float64\", \"data\", 50ms);",
            buf, sizeof buf);
    if (rc != 0) FAIL("bindOutput rc=%d %s\n", rc, buf);

    /* Reactive safety behavior: stop the motor when an obstacle is close. */
    rc = ev(vm, realm, "at (sensor.distance < 0.5) motor.val = 0.0", buf, sizeof buf);
    if (rc != 0) FAIL("at-watcher rc=%d %s\n", rc, buf);

    /* === companion raw rclc node === */
    rcl_allocator_t alloc = rcl_get_default_allocator();
    rclc_support_t support;
    rcl_ret_t r = rclc_support_init(&support, 0, NULL, &alloc);
    if (r != RCL_RET_OK) FAIL("support_init %d\n", (int)r);

    rcl_node_t node = rcl_get_zero_initialized_node();
    r = rclc_node_init_default(&node, "binding_companion", "", &support);
    if (r != RCL_RET_OK) FAIL("node_init %d\n", (int)r);

    rcl_publisher_t range_pub = rcl_get_zero_initialized_publisher();
    r = rclc_publisher_init_default(
        &range_pub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(sensor_msgs, msg, Range),
        "/range");
    if (r != RCL_RET_OK) FAIL("range_pub_init %d\n", (int)r);

    rcl_subscription_t cmd_sub = rcl_get_zero_initialized_subscription();
    r = rclc_subscription_init_default(
        &cmd_sub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Float64),
        "/cmd");
    if (r != RCL_RET_OK) FAIL("cmd_sub_init %d\n", (int)r);

    rclc_executor_t executor = rclc_executor_get_zero_initialized_executor();
    r = rclc_executor_init(&executor, &support.context, 1, &alloc);
    if (r != RCL_RET_OK) FAIL("executor_init %d\n", (int)r);

    std_msgs__msg__Float64 cmd_scratch;
    memset(&cmd_scratch, 0, sizeof cmd_scratch);
    r = rclc_executor_add_subscription(&executor, &cmd_sub, &cmd_scratch,
                                       &cmd_callback, ON_NEW_DATA);
    if (r != RCL_RET_OK) FAIL("add_subscription %d\n", (int)r);

    /* Outgoing Range message: range = 0.3, below the 0.5 threshold.  Use the
     * rosidl init/fini pair so the nested Header string is managed safely. */
    sensor_msgs__msg__Range range_msg;
    if (!sensor_msgs__msg__Range__init(&range_msg)) FAIL("range_msg_init\n");
    range_msg.range = 0.3f;

    /* === pump loop ===
     * Each iteration: companion publishes /range, urbi_step pumps the urbi
     * transport (delivers /range to bindInput, fires the periodic publisher +
     * the reactive `at`), then spin the companion executor to catch /cmd.
     * Bounded at 200 iterations * 20ms = 4s.  We require not just that a 0.0
     * was seen once, but that the command stream SETTLES to 0.0: once the
     * watcher has fired, every subsequent periodic publish reads the stopped
     * motor.val, so the last value observed after the loop must be 0.0. */
    int settled = 0;
    int i;
    for (i = 0; i < 200; i++) {
        (void)rcl_publish(&range_pub, &range_msg, NULL);

        uint64_t wake;
        (void)urbi_step(vm, 100000, &wake);

        rclc_executor_spin_some(&executor, RCL_MS_TO_NS(5));

        /* Settled = the most recent /cmd value is 0.0 and we have observed it
         * over several consecutive cycles (debounce DDS in-flight ordering). */
        if (g_cmd_seen && fabs(g_cmd_last) < 1e-9) {
            settled++;
            if (settled >= 5) break;
        } else {
            settled = 0;
        }

        struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 20 * 1000 * 1000;
        nanosleep(&ts, NULL);
    }

    int ok = (g_cmd_seen && fabs(g_cmd_last) < 1e-9 && settled >= 5);

    /* teardown */
    sensor_msgs__msg__Range__fini(&range_msg);
    rclc_executor_fini(&executor);
    rcl_subscription_fini(&cmd_sub, &node);
    rcl_publisher_fini(&range_pub, &node);
    rcl_node_fini(&node);
    rclc_support_fini(&support);
    urbi_vm_free(vm);

    if (ok) {
        printf("BINDING ok (settled to /cmd=0.0 after %d iters)\n", i);
        return 0;
    }
    fprintf(stderr, "BINDING FAIL: cmd not settled to 0.0 (seen=%d last=%g settled=%d iters=%d)\n",
            g_cmd_seen, g_cmd_last, settled, i);
    return 1;
}
