/* SPDX-License-Identifier: BSD-3-Clause */
#ifdef URBI_ENABLE_ROS2
#include "ros/uros_msg.h"
#include "object/uobject.h"
#include "value/uintern.h"
#include "vm/uvm.h"
#include <string.h>
#define UROS_MSG_MAX 32
static struct { const char *name; struct UObject *proto; } g_protos[UROS_MSG_MAX];  /* audit-globals-allow: process-global msg-proto cache; per-VM relocation is v0.13.4-ros-hardening scope (refactor-3 ROS-05/XC-19) */
static int g_protos_n;  /* audit-globals-allow: cross-VM proto cache — moves onto UVM at v0.13.4 (refactor-3 ROS-05/GC-11/XC-19) */
static int g_overflow;  /* audit-globals-allow: sticky cap-exceeded flag; reset by urbi_ros_msg__reset */
int urbi_streq(const char *a, const char *b){ return strcmp(a,b)==0; }
void urbi_ros_msg__reset(void){ g_protos_n = 0; g_overflow = 0; }
void urbi_ros_msg__record(struct UVM *vm, const char *name, struct UObject *o){
    if (g_protos_n < UROS_MSG_MAX){
        g_protos[g_protos_n].name = name;
        g_protos[g_protos_n].proto = o;
        g_protos_n++;
    } else {
        g_overflow = 1;
        if (vm != NULL && vm->host_log_fn != NULL)
            vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_ERROR,
                            "ros: msg-proto registry full; registration of '%s' dropped",
                            name);
    }
}
struct UObject *urbi_ros_msg_alloc(struct UVM *vm, const char *name){
    int i; for (i=0;i<g_protos_n;i++) if (urbi_streq(name,g_protos[i].name)) return urbi_object_clone(vm, g_protos[i].proto);
    if (g_overflow) return NULL;  /* name absent because registry overflowed; propagate */
    return urbi_object_clone(vm, urbi_object_root(vm));
}
#else
typedef int uros_translation_unit_not_empty;
#endif /* URBI_ENABLE_ROS2 */
