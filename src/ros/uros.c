/* SPDX-License-Identifier: BSD-3-Clause */
/* src/ros/uros.c — ROS2 bridge core (optional, URBI_ENABLE_ROS2). */
#ifdef URBI_ENABLE_ROS2

#include "urbi/ros.h"
#include "urbi/urbi.h"         /* URBI_OK, URBI_ERR_* */
#include "urbi/object.h"       /* URBIAtomFamily, URBI_ATOM_OBJECT, urbi_object_root */
#include "vm/uvm.h"            /* UVM */
#include "object/uobject.h"    /* urbi_object_alloc, urbi_object_set_protos_single */

/* urbi_ros_register: allocate vm->ros_proto as a root-Object-family UObject
 * and cache it on the VM.  Called from urbi_stdlib_boot (gated).
 * Idempotent: subsequent calls return URBI_OK immediately.
 * Returns URBI_OK / URBI_ERR_INVALID_ARG / URBI_ERR_OOM. */
int
urbi_ros_register(struct UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    if (vm->ros_proto != NULL) return URBI_OK; /* idempotent */

    UObject *proto = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
    if (proto == NULL) return URBI_ERR_OOM;

    UObject *root = urbi_object_root(vm);
    if (root == NULL) return URBI_ERR_OOM;
    urbi_object_set_protos_single(vm, proto, root);

    vm->ros_proto = (void *)proto;
    return URBI_OK;
}

int
urbi_ros_register_globals(struct UVM *vm, struct URealm *realm)
{
    (void)vm; (void)realm;
    return 0; /* URBI_OK — stub; filled in by later tasks */
}

void
urbi_ros_pump(struct UVM *vm)
{
    (void)vm;
}

#endif /* URBI_ENABLE_ROS2 */
