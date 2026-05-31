/* SPDX-License-Identifier: BSD-3-Clause */
/* driver_pubsub.c — B4+B5 integration driver.  Exercises the real rcl
 * transport end-to-end at the transport-seam level: create a publisher and a
 * subscriber on the same topic, publish an Int32, spin the executor, and
 * confirm the deliver callback fires with the right value through live DDS.
 * Prints "PUBSUB42 ok" on success. */
#define _POSIX_C_SOURCE 199309L   /* nanosleep / struct timespec under -std=c99 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "urbi/object.h"
#include "object/uobject.h"
#include "ros/uros_transport.h"
#include "ros/uros_msg.h"

/* The transport factory is internal to uros_rcl.c; pull its init directly. */
#include "ros/uros_rcl.h"

static void *
drv_alloc(void *ptr, size_t n, void *ud)
{
    (void)ud;
    if (n == 0) { free(ptr); return NULL; }
    return realloc(ptr, n);
}

static int g_got = -1;

static void
on_deliver(void *ud, uint32_t sub_handle, UValue msg_obj)
{
    struct UVM *vm = (struct UVM *)ud;
    UValue dv = urbi_make_nil();
    (void)sub_handle;
    if (urbi_slot_get(vm, msg_obj, "data", 4, &dv) == 0)
        g_got = (int)urbi_value_as_int(dv);
}

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "PUBSUB42 FAIL: %s\n", m); return 1; } } while (0)

int
main(void)
{
    struct UVM *vm = urbi_vm_create(drv_alloc, NULL);
    CHECK(vm != NULL, "vm");
    struct URealm *realm = urbi_realm_global(vm);
    CHECK(realm != NULL, "realm");
    CHECK(urbi_ros_msg_register_all(vm) == 0, "msg_register");

    URosTransport tp;
    memset(&tp, 0, sizeof tp);
    uros_rcl_init(&tp);
    CHECK(tp.self != NULL, "transport alloc");
    CHECK(tp.init(tp.self, "pubsub_node") == 0, "node init");

    uint32_t pub = tp.create_pub(tp.self, "/urbi_b4b5", "std_msgs/Int32");
    CHECK(pub != UROS_INVALID_HANDLE, "create_pub");
    uint32_t sub = tp.create_sub(tp.self, "/urbi_b4b5", "std_msgs/Int32");
    CHECK(sub != UROS_INVALID_HANDLE, "create_sub");

    /* Build an Int32 message object with data = 42. */
    UObject *o = urbi_object_clone(vm, urbi_object_root(vm));
    CHECK(o != NULL, "msg clone");
    UValue obj = urbi_make_object(o);
    CHECK(urbi_slot_set(vm, obj, "data", 4, urbi_make_int(42)) == 0, "msg set");

    /* DDS discovery is async; publish + spin in a bounded loop until delivered. */
    int i;
    for (i = 0; i < 100 && g_got < 0; i++) {
        CHECK(tp.publish(tp.self, vm, pub, obj) == 0, "publish");
        tp.spin(tp.self, vm, on_deliver, vm);
        if (g_got < 0) {
            struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 50 * 1000 * 1000;
            nanosleep(&ts, NULL);
        }
    }

    int ok = (g_got == 42);
    tp.fini(tp.self);
    uros_rcl_free(&tp);
    if (ok) printf("PUBSUB42 ok\n");
    else    fprintf(stderr, "PUBSUB42 FAIL: got=%d\n", g_got);
    urbi_vm_free(vm);
    return ok ? 0 : 1;
}
