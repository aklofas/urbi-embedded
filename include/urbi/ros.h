/* SPDX-License-Identifier: BSD-3-Clause */
/* include/urbi/ros.h — optional ROS2 bridge (URBI_ENABLE_ROS2).
 *
 * The bridge is a self-contained optional component. The core VM has no
 * reference to it. All symbols here exist only in URBI_ENABLE_ROS2 builds. */
#ifndef URBI_ROS_H
#define URBI_ROS_H

#ifdef URBI_ENABLE_ROS2

#include <stddef.h>
#include <stdint.h>

struct UVM;
struct URealm;

/* Allocate + install the `ros` native namespace proto on the VM.
 * Idempotent (no-op if vm->ros_proto already set). Called from stdlib boot. */
int urbi_ros_register(struct UVM *vm);

/* Bind `ros` as a realm global pointing at the cached proto.
 * Called from urbi_populate_realm_globals (post-bake hook). */
int urbi_ros_register_globals(struct UVM *vm, struct URealm *realm);

/* Drain the transport's incoming queue once and emit events. Called once
 * per urbi_step (see later task). No-op if ros.init() was never called. */
void urbi_ros_pump(struct UVM *vm);

#endif /* URBI_ENABLE_ROS2 */
#endif /* URBI_ROS_H */
