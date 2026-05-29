/* SPDX-License-Identifier: BSD-3-Clause */
/* src/ros/uros_internal.h — bridge state shared between ros sources and tests.
 * Not part of the public API; only included by src/ros/ and unit tests. */
#ifndef UROS_INTERNAL_H
#define UROS_INTERNAL_H
#ifdef URBI_ENABLE_ROS2

#include "ros/uros_transport.h"

struct UEvent; /* forward declaration; defined in event/uevent.h */

#define UROS_MAX_SUBS 16

typedef struct {
    int           inited;
    URosTransport tp;
    struct {
        uint32_t       handle;
        struct UEvent *event;
        const char    *type;
    } subs[UROS_MAX_SUBS];
    int           sub_count;
} URosBridge;

/* Returns the singleton bridge state. Defined in uros.c. */
URosBridge *urbi_ros_bridge(void);

#endif /* URBI_ENABLE_ROS2 */
#endif /* UROS_INTERNAL_H */
