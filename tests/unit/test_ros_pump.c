/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ros_pump.c — urbi_ros_pump drains an injected mock message, unmarshals
 * it, and emits onto the subscription's UEvent so an `at (sub?(var m))` watcher
 * body fires.  The robust proof: explicit pump + explicit step, then read the
 * value the watcher recorded back out of C. */
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

#define UTEST(name) static void name(void)

static void *
ros_pump_alloc(void *ptr, size_t nbytes, void *ud)
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

/* An injected Int32(11) on the subscription, pumped + stepped, fires the
 * `at (s?(var m))` watcher which writes Realm.got = m.data. */
UTEST(ros_pump_fires_watcher_with_payload)
{
    reset_bridge();

    struct UVM *vm = urbi_vm_create(ros_pump_alloc, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);
    if (realm == NULL) { urbi_vm_free(vm); return; }

    char errbuf[512];
#define EVAL(src) \
    UASSERT_EQ(urbi_repl_eval(vm, realm, (src), strlen(src), \
                              errbuf, sizeof(errbuf)), URBI_OK); \
    drain_vm_pump(vm)

    EVAL("ros.init(\"n\")");
    EVAL("var Realm.got = 0");
    EVAL("var s = ros.subscribe(\"/c\", \"std_msgs/Int32\")");
    EVAL("at (s?(var m)) Realm.got = m.data");

#undef EVAL

    /* Inject a mock Int32(11) onto subscription handle 0 (first endpoint). */
    struct urbi_ros__std_msgs__Int32 in;
    in.data = 11;
    uros_mock_inject(urbi_ros_bridge()->tp.self, 0, &in, sizeof in);

    /* Explicit pump: drains the message, unmarshals, emits onto the event
     * (spawns the watcher body coroutine with m bound in R[0]). */
    urbi_ros_pump(vm);

    /* Step: run the spawned watcher body to completion. */
    uint64_t wake = 0;
    urbi_step(vm, 100000, &wake);
    drain_vm_pump(vm);

    /* Read Realm.got back: the watcher wrote it as a global slot. */
    UValue got = urbi_make_nil();
    UASSERT_EQ(urbi_realm_get_global(vm, realm, "got", 3, &got), URBI_OK);
    UASSERT_EQ((int)urbi_value_is_int(got), 1);
    UASSERT_EQ((long long)urbi_value_as_int(got), 11LL);

    urbi_vm_free(vm);
}

void
test_ros_pump_suite(void)
{
    utest_run("ros_pump.fires_watcher_with_payload",
              ros_pump_fires_watcher_with_payload);
}

#else  /* !URBI_ENABLE_ROS2 */

void test_ros_pump_suite(void) { /* skipped: URBI_ENABLE_ROS2=0 */ }

#endif /* URBI_ENABLE_ROS2 */
