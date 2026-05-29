/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ros_subscribe.c — ros.subscribe creates a first-class Event, records it
 * in the bridge subs[] table, GC-roots it, and returns it. */
#ifdef URBI_ENABLE_ROS2

#include "utest.h"
#include "urbi/urbi.h"
#include "ros/uros_internal.h"
#include "ros/uros_mock.h"
#include "vm/uvm.h"
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

static void *
ros_sub_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

static void
drain_vm_sub(struct UVM *vm)
{
    int i;
    for (i = 0; i < 200; i++) {
        UStepResult r = urbi_step(vm, 500, NULL);
        if (r == URBI_STEP_QUIESCENT || r == URBI_STEP_WAKE_AT) break;
    }
}

/* Helper: reset the singleton bridge so each test starts clean. */
static void
reset_bridge(void)
{
    URosBridge *b = urbi_ros_bridge();
    if (b->inited) {
        b->tp.fini(b->tp.self);
        uros_mock_free(&b->tp);
        memset(b, 0, sizeof(*b));
    }
}

/* ros.subscribe records the subscription in the bridge and sets event != NULL. */
UTEST(ros_subscribe_records_in_bridge)
{
    reset_bridge();

    struct UVM *vm = urbi_vm_create(ros_sub_alloc, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);
    if (realm == NULL) { urbi_vm_free(vm); return; }

    char errbuf[512];
#define EVAL(src) \
    UASSERT_EQ(urbi_repl_eval(vm, realm, (src), strlen(src), \
                              errbuf, sizeof(errbuf)), URBI_OK); \
    drain_vm_sub(vm)

    EVAL("ros.init(\"n\")");
    EVAL("ros.subscribe(\"/scan\", \"std_msgs/Int32\")");

#undef EVAL

    URosBridge *b = urbi_ros_bridge();
    UASSERT_EQ(b->sub_count, 1);
    UASSERT_NE((long long)(b->subs[0].event), 0LL);

    urbi_vm_free(vm);
}

/* A second subscribe increments sub_count to 2 with distinct events. */
UTEST(ros_subscribe_two_distinct_entries)
{
    reset_bridge();

    struct UVM *vm = urbi_vm_create(ros_sub_alloc, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);
    if (realm == NULL) { urbi_vm_free(vm); return; }

    char errbuf[512];
#define EVAL(src) \
    UASSERT_EQ(urbi_repl_eval(vm, realm, (src), strlen(src), \
                              errbuf, sizeof(errbuf)), URBI_OK); \
    drain_vm_sub(vm)

    EVAL("ros.init(\"n\")");
    EVAL("ros.subscribe(\"/scan\", \"std_msgs/Int32\")");
    EVAL("ros.subscribe(\"/cmd\", \"std_msgs/Int32\")");

#undef EVAL

    URosBridge *b = urbi_ros_bridge();
    UASSERT_EQ(b->sub_count, 2);
    UASSERT_NE((long long)(b->subs[0].event), 0LL);
    UASSERT_NE((long long)(b->subs[1].event), 0LL);
    /* Events must be distinct objects. */
    UASSERT_NE((long long)(b->subs[0].event), (long long)(b->subs[1].event));

    urbi_vm_free(vm);
}

void
test_ros_subscribe_suite(void)
{
    utest_run("ros_subscribe.records_in_bridge",
              ros_subscribe_records_in_bridge);
    utest_run("ros_subscribe.two_distinct_entries",
              ros_subscribe_two_distinct_entries);
}

#else  /* !URBI_ENABLE_ROS2 */

void test_ros_subscribe_suite(void) { /* skipped: URBI_ENABLE_ROS2=0 */ }

#endif /* URBI_ENABLE_ROS2 */
