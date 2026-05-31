/* SPDX-License-Identifier: BSD-3-Clause */
/* driver_init.c — B2 integration driver.  Links liburbi built with the rcl
 * backend and confirms ros.init() brings up a real rcl node: evaluates
 * ros.init("spike_node") then ros.inited() and checks the formatted result is
 * "true".  Prints "ROSINIT ok" + exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "urbi/urbi.h"

static void *
drv_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

static int
eval_line(struct UVM *vm, struct URealm *realm, const char *src,
          char *out, size_t out_sz)
{
    return urbi_repl_eval(vm, realm, src, strlen(src), out, out_sz);
}

int
main(void)
{
    struct UVM *vm = urbi_vm_create(drv_alloc, NULL);
    if (vm == NULL) { fprintf(stderr, "ROSINIT FAIL: vm_create\n"); return 1; }
    /* Use the global realm — trusted host code, no compile budget. */
    struct URealm *realm = urbi_realm_global(vm);
    if (realm == NULL) { fprintf(stderr, "ROSINIT FAIL: realm\n"); return 1; }

    char buf[256];
    int rc = eval_line(vm, realm, "ros.init(\"spike_node\")", buf, sizeof buf);
    if (rc != 0) {
        fprintf(stderr, "ROSINIT FAIL: init eval rc=%d buf=%s\n", rc, buf);
        urbi_vm_free(vm);
        return 1;
    }

    rc = eval_line(vm, realm, "ros.inited()", buf, sizeof buf);
    if (rc != 0) {
        fprintf(stderr, "ROSINIT FAIL: inited eval rc=%d buf=%s\n", rc, buf);
        urbi_vm_free(vm);
        return 1;
    }

    int ok = (strstr(buf, "true") != NULL);
    if (ok) printf("ROSINIT ok\n");
    else    fprintf(stderr, "ROSINIT FAIL: ros.inited() returned '%s'\n", buf);

    urbi_vm_free(vm);
    return ok ? 0 : 1;
}
