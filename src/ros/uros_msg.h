/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UROS_MSG_H
#define UROS_MSG_H
#ifdef URBI_ENABLE_ROS2
#include <stddef.h>
#include "urbi/types.h"
struct UVM; struct UObject;
typedef int (*urbi_ros_marshal_fn)(struct UVM *, UValue, void *);
typedef int (*urbi_ros_unmarshal_fn)(struct UVM *, const void *, UValue *);
typedef struct {
    const char *name; size_t c_size;
    urbi_ros_marshal_fn marshal; urbi_ros_unmarshal_fn unmarshal;
} URosMsgType;

/* Register all built-in ROS message type descriptors on the VM. */
int urbi_ros_msg_register_all(struct UVM *vm);

/* Look up a message type descriptor by ROS type name (e.g. "std_msgs/Int32"). */
const URosMsgType *urbi_ros_msg_lookup(const char *name);

/* Allocate a fresh zeroed message object by type name; returns NULL on unknown type. */
struct UObject *urbi_ros_msg_alloc(struct UVM *vm, const char *name);

/* String equality helper used across the generated message descriptors. */
int urbi_streq(const char *a, const char *b);

/* Internal: record a message object keyed by name in the codegen cache. */
void urbi_ros_msg__record(struct UVM *vm, const char *name, struct UObject *o);

/* Internal: reset the codegen cache (used in tests). */
void urbi_ros_msg__reset(void);
#endif
#endif
