/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ros_service.c — ros.service registers a service endpoint, records it
 * in bridge.services[], and GC-roots the handler closure. */
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
ros_svc_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

static void
drain_vm_svc(struct UVM *vm)
{
    int i;
    for (i = 0; i < 200; i++) {
        UStepResult r = urbi_step(vm, 500, NULL);
        if (r == URBI_STEP_QUIESCENT || r == URBI_STEP_WAKE_AT) break;
    }
}

static void
reset_bridge_svc(void)
{
    URosBridge *b = urbi_ros_bridge();
    if (b->inited) {
        b->tp.fini(b->tp.self);
        uros_mock_free(&b->tp);
        memset(b, 0, sizeof(*b));
    }
}

/* ros.service registers the service in bridge.services[] with count == 1. */
UTEST(ros_service_records_in_bridge)
{
    reset_bridge_svc();

    struct UVM *vm = urbi_vm_create(ros_svc_alloc, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);
    if (realm == NULL) { urbi_vm_free(vm); return; }

    char errbuf[512];
#define EVAL(src) \
    UASSERT_EQ(urbi_repl_eval(vm, realm, (src), strlen(src), \
                              errbuf, sizeof(errbuf)), URBI_OK); \
    drain_vm_svc(vm)

    EVAL("ros.init(\"n\")");
    EVAL("ros.service(\"/set_mode\", \"std_msgs/Int32\", function (r) { r })");

#undef EVAL

    URosBridge *b = urbi_ros_bridge();
    UASSERT_EQ(b->service_count, 1);

    urbi_vm_free(vm);
}

void
test_ros_service_suite(void)
{
    utest_run("ros_service.records_in_bridge",
              ros_service_records_in_bridge);
}

#else  /* !URBI_ENABLE_ROS2 */

void test_ros_service_suite(void) { /* skipped: URBI_ENABLE_ROS2=0 */ }

#endif /* URBI_ENABLE_ROS2 */
