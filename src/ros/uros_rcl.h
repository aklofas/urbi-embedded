/* SPDX-License-Identifier: BSD-3-Clause */
/* src/ros/uros_rcl.h — real rcl/rclc/DDS transport backend (container-only).
 * Compiled only when URBI_ROS_BACKEND_RCL is defined; the host build uses the
 * mock backend (uros_mock.c). */
#ifndef UROS_RCL_H
#define UROS_RCL_H
#if defined(URBI_ENABLE_ROS2) && defined(URBI_ROS_BACKEND_RCL)

#include "ros/uros_transport.h"

/* Populate tp with the rcl transport vtable + freshly allocated state. */
void uros_rcl_init(URosTransport *tp);
/* Free the rcl transport state (call after tp->fini). */
void uros_rcl_free(URosTransport *tp);

#endif /* URBI_ENABLE_ROS2 && URBI_ROS_BACKEND_RCL */
#endif /* UROS_RCL_H */
