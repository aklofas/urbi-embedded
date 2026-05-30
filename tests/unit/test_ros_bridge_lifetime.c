/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ros_bridge_lifetime.c — the process-global ROS bridge must be scoped to
 * the VM that called ros.init().  A VM that inits the bridge, then is freed,
 * must reset the bridge so a subsequent VM never pumps the prior VM's freed
 * UEvent* (cross-VM use-after-free) or polls the leaked mock transport.
 *
 * Mirrors the harness shape of test_ros_pump.c: single-arg UTEST + utest_run
 * registration, allocator-based urbi_vm_create. */
#ifdef URBI_ENABLE_ROS2

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "ros/uros_internal.h"
#include "ros/uros_mock.h"
#include "ros/generated/ros_msgs.gen.h"
#include "vm/uvm.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

static void *
ros_life_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

/* Drive VM to quiescence after async activity. */
static void
drain_vm_pump(struct UVM *vm)
{
    int i;
    for (i = 0; i < 200; i++) {
        UStepResult r = urbi_step(vm, 500, NULL);
        if (r == URBI_STEP_QUIESCENT || r == URBI_STEP_WAKE_AT) break;
    }
}

/* Reset the singleton bridge so the test starts from a clean transport. */
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

/* VM A inits the bridge + subscribes + injects a pending message but never
 * pumps.  Freeing A must reset the bridge (inited==0), so a fresh VM B can
 * urbi_step without touching A's freed UEvent* or the leaked mock state. */
UTEST(ros_bridge_lifetime_no_cross_vm_uaf)
{
    reset_bridge();

    /* --- VM A --- */
    struct UVM *vmA = urbi_vm_create(ros_life_alloc, NULL);
    UASSERT_NE((long long)vmA, 0LL);
    if (vmA == NULL) return;
    struct URealm *realmA = urbi_realm_create(vmA);
    UASSERT_NE((long long)realmA, 0LL);
    if (realmA == NULL) { urbi_vm_free(vmA); return; }

    char errbuf[512];
#define EVAL_A(src) \
    UASSERT_EQ(urbi_repl_eval(vmA, realmA, (src), strlen(src), \
                              errbuf, sizeof(errbuf)), URBI_OK); \
    drain_vm_pump(vmA)

    EVAL_A("ros.init(\"a\")");
    EVAL_A("var Realm.got = 0");
    EVAL_A("var s = ros.subscribe(\"/c\", \"std_msgs/Int32\")");
    EVAL_A("at (s?(var m)) Realm.got = m.data");
#undef EVAL_A

    /* The bridge belongs to A. */
    UASSERT_EQ((int)(urbi_ros_bridge()->inited != 0), 1);
    UASSERT_EQ((long long)(intptr_t)urbi_ros_bridge()->owner,
               (long long)(intptr_t)vmA);

    /* Inject a pending Int32(7) so a *future* pump WOULD emit onto A's event.
     * Do NOT pump A — the message stays queued in the mock transport. */
    struct urbi_ros__std_msgs__Int32 in;
    in.data = 7;
    uros_mock_inject(urbi_ros_bridge()->tp.self, 0, &in, sizeof in);

    /* Free A.  This MUST reset the bridge (drop the dangling UEvent* + free the
     * leaked mock state). */
    urbi_vm_free(vmA);

    /* Load-bearing post-fix assertion: shutdown ran, bridge is clean. */
    UASSERT_EQ((int)urbi_ros_bridge()->inited, 0);

    /* --- VM B --- */
    struct UVM *vmB = urbi_vm_create(ros_life_alloc, NULL);
    UASSERT_NE((long long)vmB, 0LL);
    if (vmB == NULL) return;
    struct URealm *realmB = urbi_realm_create(vmB);
    UASSERT_NE((long long)realmB, 0LL);
    if (realmB == NULL) { urbi_vm_free(vmB); return; }

    /* Step B.  urbi_step calls urbi_ros_pump(B); with the owner-check the stale
     * (now reset) bridge must NOT be polled, and B must NOT touch A's freed
     * event.  Under ASan this is where the cross-VM UAF would fire. */
    {
        uint64_t wake = 0;
        urbi_step(vmB, 100000, &wake);
        urbi_step(vmB, 100000, &wake);
        drain_vm_pump(vmB);
    }

    /* B can init the bridge fresh; it now owns it. */
    char errbufB[512];
    const char *initB = "ros.init(\"b\")";
    UASSERT_EQ(urbi_repl_eval(vmB, realmB, initB, strlen(initB),
                              errbufB, sizeof(errbufB)), URBI_OK);
    drain_vm_pump(vmB);
    UASSERT_EQ((int)(urbi_ros_bridge()->inited != 0), 1);
    UASSERT_EQ((long long)(intptr_t)urbi_ros_bridge()->owner,
               (long long)(intptr_t)vmB);

    urbi_vm_free(vmB);

    /* Freeing B also resets the bridge. */
    UASSERT_EQ((int)urbi_ros_bridge()->inited, 0);
}

void
test_ros_bridge_lifetime_suite(void)
{
    utest_run("ros_bridge_lifetime.no_cross_vm_uaf",
              ros_bridge_lifetime_no_cross_vm_uaf);
}

#else  /* !URBI_ENABLE_ROS2 */

void test_ros_bridge_lifetime_suite(void) { /* skipped: URBI_ENABLE_ROS2=0 */ }

#endif /* URBI_ENABLE_ROS2 */
