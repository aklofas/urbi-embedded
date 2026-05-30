/* SPDX-License-Identifier: BSD-3-Clause */
/* src/ros/uros_internal.h — bridge state shared between ros sources and tests.
 * Not part of the public API; only included by src/ros/ and unit tests. */
#ifndef UROS_INTERNAL_H
#define UROS_INTERNAL_H
#ifdef URBI_ENABLE_ROS2

#include "ros/uros_transport.h"

struct UEvent; /* forward declaration; defined in event/uevent.h */
struct UVM;    /* forward declaration; defined in vm/uvm.h */

#define UROS_MAX_SUBS 16

typedef struct {
    int           inited;
    /* The VM that called ros.init().  The process-global bridge holds UEvent*
     * and a mock-transport allocation that live in this VM's heap; the pump and
     * shutdown paths gate on owner==vm so a later VM never touches freed state. */
    struct UVM   *owner;
    URosTransport tp;
    struct {
        uint32_t       handle;
        struct UEvent *event;
        const char    *type;
    } subs[UROS_MAX_SUBS];
    int           sub_count;
    struct {
        uint32_t    handle;
        const char *type;
    } services[UROS_MAX_SUBS];
    int           service_count;
} URosBridge;

/* Returns the singleton bridge state. Defined in uros.c. */
URosBridge *urbi_ros_bridge(void);

#endif /* URBI_ENABLE_ROS2 */
#endif /* UROS_INTERNAL_H */
