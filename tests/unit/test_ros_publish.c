/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ros_publish.c — ros.msg + ros.publisher + Publisher.publish end-to-end. */
#ifdef URBI_ENABLE_ROS2

#include "utest.h"
#include "urbi/urbi.h"
#include "ros/uros_internal.h"
#include "ros/uros_mock.h"
#include "ros/generated/ros_msgs.gen.h"
#include "vm/uvm.h"
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

static void *
ros_pub_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

/* Drive VM to quiescence after async activity. */
static void
drain_vm_ros(struct UVM *vm)
{
    int i;
    for (i = 0; i < 200; i++) {
        UStepResult r = urbi_step(vm, 500, NULL);
        if (r == URBI_STEP_QUIESCENT || r == URBI_STEP_WAKE_AT) break;
    }
}

/* ros.msg + ros.publisher + Publisher.publish round-trips linear.x = 3.0
 * through the mock transport and the Twist C struct. */
UTEST(ros_publish_twist_linear_x_roundtrips)
{
    struct UVM *vm = urbi_vm_create(ros_pub_alloc, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);
    if (realm == NULL) { urbi_vm_free(vm); return; }

    /* Reset bridge so this test is isolated even if another test already
     * called ros.init() and left the static singleton initialised. */
    URosBridge *b = urbi_ros_bridge();
    if (b->inited) {
        b->tp.fini(b->tp.self);
        uros_mock_free(&b->tp);
        memset(b, 0, sizeof(*b));
    }

    char errbuf[512];
#define EVAL(src) \
    UASSERT_EQ(urbi_repl_eval(vm, realm, (src), strlen(src), \
                              errbuf, sizeof(errbuf)), URBI_OK); \
    drain_vm_ros(vm)

    /* Step 1: init the ROS node. */
    EVAL("ros.init(\"n\")");

    /* Step 2: create a publisher on /cmd_vel for geometry_msgs/Twist. */
    EVAL("Realm.p = ros.publisher(\"/cmd_vel\", \"geometry_msgs/Twist\")");

    /* Step 3: create a zeroed Twist message object. */
    EVAL("Realm.t = ros.msg(\"geometry_msgs/Twist\")");

    /* Step 4: set the nested linear.x slot to 3.0. */
    EVAL("Realm.t.linear.x = 3.0");

    /* Step 5: publish. */
    EVAL("Realm.p.publish(Realm.t)");

#undef EVAL

    /* Verify the mock captured the struct with linear.x == 3.0. */
    UASSERT_EQ(b->inited, 1);

    const void *cap = NULL;
    size_t len = 0;
    /* Publisher handle 0 = first endpoint created after init. */
    UASSERT_EQ(uros_mock_last_published(b->tp.self, 0, &cap, &len), 1);
    UASSERT_EQ((long long)len, (long long)sizeof(struct urbi_ros__geometry_msgs__Twist));

    if (cap != NULL && len == sizeof(struct urbi_ros__geometry_msgs__Twist)) {
        const struct urbi_ros__geometry_msgs__Twist *tw =
            (const struct urbi_ros__geometry_msgs__Twist *)cap;
        UASSERT(tw->linear.x == 3.0);
    }

    urbi_vm_free(vm);
}

void
test_ros_publish_suite(void)
{
    utest_run("ros_publish.twist_linear_x_roundtrips",
              ros_publish_twist_linear_x_roundtrips);
}

#else  /* !URBI_ENABLE_ROS2 */

void test_ros_publish_suite(void) { /* skipped: URBI_ENABLE_ROS2=0 */ }

#endif /* URBI_ENABLE_ROS2 */
