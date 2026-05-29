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
int urbi_ros_msg_register_all(struct UVM *vm);
const URosMsgType *urbi_ros_msg_lookup(const char *name);
struct UObject *urbi_ros_msg_alloc(struct UVM *vm, const char *name);
int urbi_streq(const char *a, const char *b);
void urbi_ros_msg__record(struct UVM *vm, const char *name, struct UObject *o);
void urbi_ros_msg__reset(void);
#endif
#endif
