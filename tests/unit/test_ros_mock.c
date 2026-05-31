/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ros_mock.c — unit tests for the in-memory mock ROS2 transport.
 * Updated for the v0.12.1 object-based seam: publish/spin take UValue objects;
 * the mock marshals/unmarshals internally via the codegen URosMsgType. */
#ifdef URBI_ENABLE_ROS2

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "ros/uros_mock.h"
#include "ros/uros_transport.h"
#include "ros/uros_msg.h"
#include "ros/generated/ros_msgs.gen.h"
#include "vm/uvm.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void *
mock_alloc_fn(void *ptr, size_t n, void *ud)
{
    (void)ud;
    if (n == 0) { free(ptr); return NULL; }
    return realloc(ptr, n);
}

/* Global capture state used by the spin deliver callback (C99: no closures). */
static struct {
    int      called;
    uint32_t handle;
    int32_t  data_val;
} g_spin_capture;

static void
spin_deliver_cb(void *ud, uint32_t sub_handle, UValue msg_obj)
{
    struct UVM *vm = (struct UVM *)ud;
    g_spin_capture.called++;
    g_spin_capture.handle = sub_handle;
    /* Read .data slot from the Int32 object. */
    UValue dv = urbi_make_nil();
    if (urbi_slot_get(vm, msg_obj, "data", 4, &dv) == 0 && urbi_value_is_int(dv))
        g_spin_capture.data_val = (int32_t)urbi_value_as_int(dv);
}

/* Publish stores the marshaled struct in last_pub; retrieve via
 * uros_mock_last_published and verify the bytes. */
static void
ros_mock_publish_is_captured(void)
{
    struct UVM *vm = urbi_vm_create(mock_alloc_fn, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);
    if (realm == NULL) { urbi_vm_free(vm); return; }

    URosTransport tp;
    uros_mock_init(&tp);
    UASSERT_EQ(tp.init(tp.self, "n"), 0);

    uint32_t pub = tp.create_pub(tp.self, "/t", "std_msgs/Int32");
    UASSERT_NE((long long)pub, (long long)UROS_INVALID_HANDLE);

    /* Build an Int32 object with data=42 using the codegen unmarshal. */
    struct urbi_ros__std_msgs__Int32 s42;
    s42.data = 42;
    UValue msg = urbi_make_nil();
    UASSERT_EQ(urbi_ros_unmarshal__std_msgs__Int32(vm, &s42, &msg), 0);

    /* Publish: mock marshals obj → blob. */
    UASSERT_EQ(tp.publish(tp.self, vm, pub, msg), 0);

    /* Retrieve last published blob and check the marshaled struct. */
    const void *cap = NULL;
    size_t len = 0;
    UASSERT_EQ(uros_mock_last_published(tp.self, pub, &cap, &len), 1);
    UASSERT_EQ((long long)len, (long long)sizeof(struct urbi_ros__std_msgs__Int32));
    if (cap != NULL && len == sizeof(struct urbi_ros__std_msgs__Int32)) {
        const struct urbi_ros__std_msgs__Int32 *got =
            (const struct urbi_ros__std_msgs__Int32 *)cap;
        UASSERT_EQ((int)got->data, 42);
    }

    tp.fini(tp.self);
    uros_mock_free(&tp);
    urbi_vm_free(vm);
}

/* Inject a raw blob, then spin: the deliver callback receives an unmarshaled
 * Int32 object with the correct data value. */
static void
ros_mock_inject_then_spin(void)
{
    struct UVM *vm = urbi_vm_create(mock_alloc_fn, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);
    if (realm == NULL) { urbi_vm_free(vm); return; }

    URosTransport tp;
    uros_mock_init(&tp);
    UASSERT_EQ(tp.init(tp.self, "n"), 0);

    uint32_t sub = tp.create_sub(tp.self, "/t", "std_msgs/Int32");
    UASSERT_NE((long long)sub, (long long)UROS_INVALID_HANDLE);

    /* Inject using the raw-bytes helper (same path as ros_inject_int32_method). */
    struct urbi_ros__std_msgs__Int32 in;
    in.data = 7;
    uros_mock_inject(tp.self, sub, &in, sizeof in);

    /* Reset capture. */
    g_spin_capture.called    = 0;
    g_spin_capture.handle    = UROS_INVALID_HANDLE;
    g_spin_capture.data_val  = 0;

    /* Spin: mock unmarshals the blob → calls spin_deliver_cb. */
    UASSERT_EQ(tp.spin(tp.self, vm, spin_deliver_cb, vm), 0);

    /* Verify callback was called once with the right handle and data. */
    UASSERT_EQ(g_spin_capture.called, 1);
    UASSERT_EQ((long long)g_spin_capture.handle, (long long)sub);
    UASSERT_EQ(g_spin_capture.data_val, 7);

    /* Spin again: queue should now be empty — callback not called again. */
    g_spin_capture.called = 0;
    UASSERT_EQ(tp.spin(tp.self, vm, spin_deliver_cb, vm), 0);
    UASSERT_EQ(g_spin_capture.called, 0);

    tp.fini(tp.self);
    uros_mock_free(&tp);
    urbi_vm_free(vm);
}

/* Call: mock echoes the request object back as the response. */
static void
ros_mock_call_echoes_req(void)
{
    struct UVM *vm = urbi_vm_create(mock_alloc_fn, NULL);
    UASSERT_NE((long long)vm, 0LL);
    if (vm == NULL) return;
    struct URealm *realm = urbi_realm_create(vm);
    UASSERT_NE((long long)realm, 0LL);
    if (realm == NULL) { urbi_vm_free(vm); return; }

    URosTransport tp;
    uros_mock_init(&tp);
    UASSERT_EQ(tp.init(tp.self, "n"), 0);

    uint32_t cli = tp.create_client(tp.self, "/add", "example_interfaces/AddTwoInts_Request");
    UASSERT_NE((long long)cli, (long long)UROS_INVALID_HANDLE);

    /* Build a request object with a=3, b=9. */
    struct urbi_ros__example_interfaces__AddTwoInts_Request req_s;
    req_s.a = 3; req_s.b = 9;
    UValue req_obj = urbi_make_nil();
    UASSERT_EQ(urbi_ros_unmarshal__example_interfaces__AddTwoInts_Request(
                   vm, &req_s, &req_obj), 0);

    /* Call: mock echoes req_obj as resp_obj. */
    UValue resp_obj = urbi_make_nil();
    UASSERT_EQ(tp.call(tp.self, vm, cli, req_obj, &resp_obj), 0);

    /* Verify resp_obj.a == 3, resp_obj.b == 9 (echo). */
    UValue av = urbi_make_nil(), bv = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get(vm, resp_obj, "a", 1, &av), 0);
    UASSERT_EQ(urbi_slot_get(vm, resp_obj, "b", 1, &bv), 0);
    UASSERT_EQ((long long)urbi_value_as_int(av), 3LL);
    UASSERT_EQ((long long)urbi_value_as_int(bv), 9LL);

    tp.fini(tp.self);
    uros_mock_free(&tp);
    urbi_vm_free(vm);
}

void
test_ros_mock_suite(void)
{
    utest_run("ros_mock.publish_is_captured", ros_mock_publish_is_captured);
    utest_run("ros_mock.inject_then_spin",    ros_mock_inject_then_spin);
    utest_run("ros_mock.call_echoes_req",     ros_mock_call_echoes_req);
}

#else  /* !URBI_ENABLE_ROS2 */

void test_ros_mock_suite(void) { /* skipped: URBI_ENABLE_ROS2=0 */ }

#endif /* URBI_ENABLE_ROS2 */
