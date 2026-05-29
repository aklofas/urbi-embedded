/* SPDX-License-Identifier: BSD-3-Clause */
/* src/ros/uros.c — ROS2 bridge core (optional, URBI_ENABLE_ROS2). */
#ifdef URBI_ENABLE_ROS2

#include "urbi/ros.h"

/* Stub bodies; filled in by later tasks. Present so the gate + archive
 * membership can be verified now. */
int
urbi_ros_register(struct UVM *vm)
{
    (void)vm;
    return 0; /* URBI_OK */
}

int
urbi_ros_register_globals(struct UVM *vm, struct URealm *realm)
{
    (void)vm; (void)realm;
    return 0; /* URBI_OK */
}

void
urbi_ros_pump(struct UVM *vm)
{
    (void)vm;
}

#endif /* URBI_ENABLE_ROS2 */
