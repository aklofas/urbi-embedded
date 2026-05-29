/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ros_mock.c — unit tests for the in-memory mock ROS2 transport. */
#ifdef URBI_ENABLE_ROS2

#include "utest.h"
#include "ros/uros_mock.h"
#include "ros/uros_transport.h"
#include <stdint.h>

static void
ros_mock_publish_is_captured(void)
{
    URosTransport tp;
    uros_mock_init(&tp);

    UASSERT_EQ(tp.init(tp.self, "n"), 0);

    uint32_t pub = tp.create_pub(tp.self, "/t", "std_msgs/Int32");
    UASSERT_NE((long long)pub, (long long)UROS_INVALID_HANDLE);

    int32_t val = 42;
    UASSERT_EQ(tp.publish(tp.self, pub, &val, sizeof(val)), 0);

    const void *cap = NULL;
    size_t len = 0;
    UASSERT_EQ(uros_mock_last_published(tp.self, pub, &cap, &len), 1);
    UASSERT_EQ((long long)len, (long long)sizeof(int32_t));
    UASSERT_EQ(*(const int32_t *)cap, (int32_t)42);

    tp.fini(tp.self);
    uros_mock_free(&tp);
}

static void
ros_mock_inject_then_poll(void)
{
    URosTransport tp;
    uros_mock_init(&tp);

    UASSERT_EQ(tp.init(tp.self, "n"), 0);

    uint32_t sub = tp.create_sub(tp.self, "/t", "std_msgs/Int32");
    UASSERT_NE((long long)sub, (long long)UROS_INVALID_HANDLE);

    int32_t in = 7;
    uros_mock_inject(tp.self, sub, &in, sizeof(in));

    URosIncoming got;
    UASSERT_EQ(tp.poll(tp.self, &got), 1);
    UASSERT_EQ((long long)got.sub_handle, (long long)sub);
    UASSERT_EQ((long long)got.len, (long long)4);
    UASSERT_EQ(*(const int32_t *)got.bytes, (int32_t)7);

    /* Queue should now be empty. */
    UASSERT_EQ(tp.poll(tp.self, &got), 0);

    tp.fini(tp.self);
    uros_mock_free(&tp);
}

void
test_ros_mock_suite(void)
{
    utest_run("ros_mock.publish_is_captured", ros_mock_publish_is_captured);
    utest_run("ros_mock.inject_then_poll",    ros_mock_inject_then_poll);
}

#else  /* !URBI_ENABLE_ROS2 */

void test_ros_mock_suite(void) { /* skipped: URBI_ENABLE_ROS2=0 */ }

#endif /* URBI_ENABLE_ROS2 */
