/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/integration/ros/spike_pubsub.c — rclc pub/sub grounding gate. */
/*
 * Publishes std_msgs/Int32 with data=42 on /spike_topic, subscribes on the
 * same topic, spins until the callback fires, prints "PUBSUB got=42", exits 0.
 * Exits 1 with "PUBSUB FAIL" on timeout.
 */

#include <stdio.h>
#include <string.h>

#include <rclc/rclc.h>
#include <rclc/executor.h>
#include <rcl/rcl.h>
#include <std_msgs/msg/int32.h>

static int g_got = -1;

static void
sub_callback(const void *msg_in)
{
    const std_msgs__msg__Int32 *msg = (const std_msgs__msg__Int32 *)msg_in;
    g_got = (int)msg->data;
}

int
main(void)
{
    rcl_allocator_t alloc = rcl_get_default_allocator();

    /* support */
    rclc_support_t support;
    rcl_ret_t rc = rclc_support_init(&support, 0, NULL, &alloc);
    if (rc != RCL_RET_OK) { fprintf(stderr, "PUBSUB FAIL: support_init %d\n", (int)rc); return 1; }

    /* node */
    rcl_node_t node = rcl_get_zero_initialized_node();
    rc = rclc_node_init_default(&node, "spike_node", "", &support);
    if (rc != RCL_RET_OK) { fprintf(stderr, "PUBSUB FAIL: node_init %d\n", (int)rc); return 1; }

    /* publisher */
    rcl_publisher_t pub = rcl_get_zero_initialized_publisher();
    rc = rclc_publisher_init_default(
        &pub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Int32),
        "/spike_topic");
    if (rc != RCL_RET_OK) { fprintf(stderr, "PUBSUB FAIL: pub_init %d\n", (int)rc); return 1; }

    /* subscription */
    rcl_subscription_t sub = rcl_get_zero_initialized_subscription();
    rc = rclc_subscription_init_default(
        &sub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Int32),
        "/spike_topic");
    if (rc != RCL_RET_OK) { fprintf(stderr, "PUBSUB FAIL: sub_init %d\n", (int)rc); return 1; }

    /* executor */
    rclc_executor_t executor = rclc_executor_get_zero_initialized_executor();
    rc = rclc_executor_init(&executor, &support.context, 1, &alloc);
    if (rc != RCL_RET_OK) { fprintf(stderr, "PUBSUB FAIL: executor_init %d\n", (int)rc); return 1; }

    std_msgs__msg__Int32 scratch;
    memset(&scratch, 0, sizeof(scratch));
    rc = rclc_executor_add_subscription(&executor, &sub, &scratch, &sub_callback, ON_NEW_DATA);
    if (rc != RCL_RET_OK) { fprintf(stderr, "PUBSUB FAIL: add_subscription %d\n", (int)rc); return 1; }

    /* publish */
    std_msgs__msg__Int32 out_msg;
    out_msg.data = 42;
    rc = rcl_publish(&pub, &out_msg, NULL);
    if (rc != RCL_RET_OK) { fprintf(stderr, "PUBSUB FAIL: publish %d\n", (int)rc); return 1; }

    /* spin until callback fires, up to 200 iterations * 10ms = 2s */
    for (int i = 0; i < 200 && g_got < 0; i++) {
        rclc_executor_spin_some(&executor, RCL_MS_TO_NS(10));
    }

    /* teardown */
    rclc_executor_fini(&executor);
    rcl_subscription_fini(&sub, &node);
    rcl_publisher_fini(&pub, &node);
    rcl_node_fini(&node);
    rclc_support_fini(&support);

    if (g_got == 42) {
        printf("PUBSUB got=42\n");
        return 0;
    }
    printf("PUBSUB FAIL (got=%d)\n", g_got);
    return 1;
}
