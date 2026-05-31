/* SPDX-License-Identifier: BSD-3-Clause */
/* driver_service.c — B6+B7 integration driver.  In one process: register an
 * AddTwoInts service whose handler is an urbiscript closure that sums the
 * request fields, then create a client and call it.  The rcl call path spins
 * the executor, which runs the in-process service trampoline, which invokes
 * the urbiscript handler via urbi_ros_invoke_handler.  Exercises B6 (client +
 * call) and B7 (service server + real handler invocation) end-to-end through
 * live DDS.  Prints "SERVICE sum=5" on success. */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "urbi/urbi.h"

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

int
main(void)
{
    struct UVM *vm = urbi_vm_create(drv_alloc, NULL);
    if (vm == NULL) { fprintf(stderr, "SERVICE FAIL: vm\n"); return 1; }
    struct URealm *realm = urbi_realm_global(vm);
    if (realm == NULL) { fprintf(stderr, "SERVICE FAIL: realm\n"); return 1; }

    char buf[256];
    int rc;

    rc = ev(vm, realm, "ros.init(\"svc_node\")", buf, sizeof buf);
    if (rc != 0) { fprintf(stderr, "SERVICE FAIL: init rc=%d %s\n", rc, buf); return 1; }

    /* Register a service whose handler returns a Response with sum = a + b.
     * ros.msg builds a fresh response object; set its sum slot from the req. */
    rc = ev(vm, realm,
            "ros.service(\"/urbi_add\", \"example_interfaces/AddTwoInts\","
            " function (r) { var resp = ros.msg(\"example_interfaces/AddTwoInts_Response\") | "
            " resp.sum = r.a + r.b | resp })",
            buf, sizeof buf);
    if (rc != 0) { fprintf(stderr, "SERVICE FAIL: service-register rc=%d %s\n", rc, buf); return 1; }

    /* Create a client to the same service. */
    rc = ev(vm, realm,
            "var cli = ros.client(\"/urbi_add\", \"example_interfaces/AddTwoInts\")",
            buf, sizeof buf);
    if (rc != 0) { fprintf(stderr, "SERVICE FAIL: client-create rc=%d %s\n", rc, buf); return 1; }

    /* Build a request (a=2, b=3) and call; expect sum=5. */
    rc = ev(vm, realm,
            "var req = ros.msg(\"example_interfaces/AddTwoInts_Request\") | "
            "req.a = 2 | req.b = 3 | "
            "Realm.svc_sum = cli.call(req).sum",
            buf, sizeof buf);
    if (rc != 0) { fprintf(stderr, "SERVICE FAIL: call rc=%d %s\n", rc, buf); return 1; }

    rc = ev(vm, realm, "Realm.svc_sum", buf, sizeof buf);
    if (rc != 0) { fprintf(stderr, "SERVICE FAIL: read rc=%d %s\n", rc, buf); return 1; }

    if (strstr(buf, "5") != NULL) {
        printf("SERVICE sum=5\n");
        urbi_vm_free(vm);
        return 0;
    }
    fprintf(stderr, "SERVICE FAIL: expected sum 5, got '%s'\n", buf);
    urbi_vm_free(vm);
    return 1;
}
