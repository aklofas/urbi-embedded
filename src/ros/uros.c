/* SPDX-License-Identifier: BSD-3-Clause */
/* src/ros/uros.c — ROS2 bridge core (optional, URBI_ENABLE_ROS2). */
#ifdef URBI_ENABLE_ROS2

#include "urbi/ros.h"
#include "urbi/urbi.h"         /* URBI_OK, URBI_ERR_* */
#include "urbi/object.h"       /* URBIAtomFamily, URBI_ATOM_OBJECT, urbi_object_root */
#include "urbi/types.h"        /* UValue, UVAL_OBJECT, urbi_make_nil, urbi_make_bool */
#include "vm/uvm.h"            /* UVM */
#include "object/uobject.h"    /* urbi_object_alloc, urbi_object_set_protos_single, urbi_object_set_local_slot */
#include "realm/urealm.h"      /* URealm, urbi_realm_set_global */
#include "runtime/umacros.h"   /* urbi_zero, urbi_strlen */
#include "runtime/uclosure.h"  /* UClosure, urbi_native_method_fn */
#include "value/uintern.h"     /* ustr_intern */
#include "stdlib/object_root.h" /* urbi_native_closure_create, urbi_raise_arity, urbi_raise_type */
#include "ros/uros_internal.h" /* URosBridge, urbi_ros_bridge */
#include "ros/uros_mock.h"     /* uros_mock_init */
#include "ros/uros_msg.h"      /* urbi_ros_msg_register_all */

#include <stddef.h> /* size_t */

/* === ros_register_method ===
 *
 * Install a UVAL_CLOSURE slot named `name` on `proto`, where the closure's
 * native_fn pointer holds `fn`.  Mirrors the same pattern used in
 * src/event/uevent_native.c (register_native_method). */
static int
ros_register_method(struct UVM *vm, struct UObject *proto,
                    const char *name, urbi_native_method_fn fn)
{
    UClosure *cl = urbi_native_closure_create(vm, fn);
    if (cl == NULL) return URBI_ERR_OOM;

    USymbol *sym = (USymbol *)ustr_intern(vm, name, urbi_strlen(name));
    if (sym == NULL) return URBI_ERR_OOM;

    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_CLOSURE;
    v.v.p = (void *)cl;
    if (urbi_object_set_local_slot(vm, proto, sym, v) != 0)
        return URBI_ERR_OOM;
    return URBI_OK;
}

/* === ros.init(name) ===
 *
 * Initialise the mock transport with the given node name string.
 * Idempotent: subsequent calls after the first are silently ignored. */
static int
ros_init_method(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                UValue *out)
{
    (void)self;
    if (nargs != 1) return urbi_raise_arity(vm, "ros.init", 1, nargs, out);
    if (!urbi_value_is_str(args[0]))
        return urbi_raise_type(vm, "ros.init: node name must be a String", out);
    URosBridge *b = urbi_ros_bridge();
    if (!b->inited) {
        uros_mock_init(&b->tp);
        size_t nlen;
        const char *nm = urbi_value_as_str(args[0], &nlen);
        (void)nlen;
        if (b->tp.init(b->tp.self, nm) != 0)
            return urbi_raise_type(vm, "ros.init: transport init failed", out);
        b->inited = 1;
    }
    *out = urbi_make_nil();
    return UEXEC_OK;
}

/* === ros.inited() ===
 *
 * Return true if ros.init() has been called successfully, false otherwise. */
static int
ros_inited_method(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                  UValue *out)
{
    (void)self; (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "ros.inited", 0, nargs, out);
    *out = urbi_make_bool(urbi_ros_bridge()->inited != 0);
    return UEXEC_OK;
}

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

    /* Install message protos + populate the marshaling type registry. */
    if (urbi_ros_msg_register_all(vm) != 0) return URBI_ERR_OOM;

    /* Install native methods on the ros proto. */
    UObject *rp = (UObject *)vm->ros_proto;
    if (ros_register_method(vm, rp, "init",   ros_init_method)   != URBI_OK
     || ros_register_method(vm, rp, "inited", ros_inited_method) != URBI_OK)
        return URBI_ERR_OOM;

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
