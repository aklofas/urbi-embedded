/* SPDX-License-Identifier: BSD-3-Clause */
/* src/ros/uros.c — ROS2 bridge core (optional, URBI_ENABLE_ROS2). */
#ifdef URBI_ENABLE_ROS2

#include "urbi/ros.h"
#include "urbi/urbi.h"         /* URBI_OK, URBI_ERR_* */
#include "urbi/object.h"       /* URBIAtomFamily, URBI_ATOM_OBJECT, urbi_object_root */
#include "urbi/types.h"        /* UValue, UVAL_OBJECT */
#include "vm/uvm.h"            /* UVM */
#include "object/uobject.h"    /* urbi_object_alloc, urbi_object_set_protos_single */
#include "realm/urealm.h"      /* URealm, urbi_realm_set_global */
#include "runtime/umacros.h"   /* urbi_zero */
#include "ros/uros_internal.h" /* URosBridge, urbi_ros_bridge */

/* Singleton bridge state (zero-initialized). */
static URosBridge g_bridge;

URosBridge *
urbi_ros_bridge(void)
{
    return &g_bridge;
}

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

/* urbi_ros_register_globals: bind "ros" as a realm global pointing at
 * vm->ros_proto.  Called per-realm from urbi_populate_realm_globals
 * (gated on URBI_ENABLE_ROS2) AFTER urbi_stdlib_boot populates ros_proto.
 *
 * `import ros` is not a keyword in this runtime (no import-table surface
 * yet — see module_load_isolation.chk "blocked" annotation); the `ros`
 * realm global is directly accessible as a bare identifier without any
 * import statement.  No module-table registration is needed. */
int
urbi_ros_register_globals(struct UVM *vm, struct URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;
    if (vm->ros_proto == NULL) return URBI_ERR_INVALID_STATE;

    UValue v;
    urbi_zero(&v, sizeof(v));
    v.kind = (uint8_t)UVAL_OBJECT;
    v.v.p  = vm->ros_proto;
    return urbi_realm_set_global(vm, realm, "ros", 3, v);
}

void
urbi_ros_pump(struct UVM *vm)
{
    (void)vm;
}

#else
/* Avoid ISO C "empty translation unit" (-Wpedantic) when this gated file is
 * compiled flag-free into build/host for the stdlib bake tool (TARGET != host). */
typedef int uros_translation_unit_not_empty;
#endif /* URBI_ENABLE_ROS2 */
