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

/* === ros.msg(type) ===
 *
 * Build a fresh message object by unmarshaling a zeroed C struct.
 * The unmarshal function recursively creates nested sub-message objects,
 * so nested fields (e.g. t.linear.x) are immediately writable after this
 * call — no nil-field issue. */
static int
ros_msg_method(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
               UValue *out)
{
    (void)self;
    if (nargs != 1) return urbi_raise_arity(vm, "ros.msg", 1, nargs, out);
    if (!urbi_value_is_str(args[0]))
        return urbi_raise_type(vm, "ros.msg: type must be a String", out);
    size_t tl;
    const char *type = urbi_value_as_str(args[0], &tl);
    const URosMsgType *mt = urbi_ros_msg_lookup(type);
    if (mt == NULL)
        return urbi_raise_type(vm, "ros.msg: unknown message type", out);
    unsigned char buf[512];
    if (mt->c_size > sizeof buf)
        return urbi_raise_type(vm, "ros.msg: message too large", out);
    urbi_zero(buf, mt->c_size);
    if (mt->unmarshal(vm, buf, out) != 0)
        return urbi_raise_type(vm, "ros.msg: build failed", out);
    return UEXEC_OK;
}

/* === Publisher proto (file-static, GC-rooted via hidden slot on ros_proto) ===
 *
 * g_publisher_proto is cloned by ros.publisher() for each new publisher.
 * The proto itself is kept alive by a hidden slot installed on the ros_proto
 * during urbi_ros_register. */
static struct UObject *g_publisher_proto;

/* === Publisher.publish(msg) ===
 *
 * Read __handle and __type from self, marshal msg into a C struct, then
 * call the transport's publish function. */
static int
publisher_publish_method(struct UVM *vm, UValue self, UValue *args,
                         uint8_t nargs, UValue *out)
{
    if (nargs != 1)
        return urbi_raise_arity(vm, "Publisher.publish", 1, nargs, out);

    /* Read __handle from self. */
    UValue hv = urbi_make_nil();
    if (urbi_slot_get(vm, self, "__handle", 8, &hv) != URBI_OK
     || !urbi_value_is_int(hv))
        return urbi_raise_type(vm, "Publisher.publish: invalid publisher (missing __handle)", out);
    int64_t handle = urbi_value_as_int(hv);

    /* Read __type from self. */
    UValue tv = urbi_make_nil();
    if (urbi_slot_get(vm, self, "__type", 6, &tv) != URBI_OK
     || !urbi_value_is_str(tv))
        return urbi_raise_type(vm, "Publisher.publish: invalid publisher (missing __type)", out);
    size_t tl;
    const char *type = urbi_value_as_str(tv, &tl);

    const URosMsgType *mt = urbi_ros_msg_lookup(type);
    if (mt == NULL)
        return urbi_raise_type(vm, "Publisher.publish: unknown message type", out);

    unsigned char buf[512];
    if (mt->c_size > sizeof buf)
        return urbi_raise_type(vm, "Publisher.publish: message too large", out);
    urbi_zero(buf, mt->c_size);

    if (mt->marshal(vm, args[0], buf) != 0)
        return urbi_raise_type(vm, "Publisher.publish: marshal failed", out);

    URosBridge *b = urbi_ros_bridge();
    if (!b->inited)
        return urbi_raise_type(vm, "Publisher.publish: ros not initialized", out);
    if (b->tp.publish(b->tp.self, (uint32_t)handle, buf, mt->c_size) != 0)
        return urbi_raise_type(vm, "Publisher.publish: transport error", out);

    *out = urbi_make_nil();
    return UEXEC_OK;
}

/* === ros.publisher(topic, type) ===
 *
 * Create a publisher endpoint on the current transport.  Returns a Publisher
 * object (clone of g_publisher_proto) with hidden slots __handle and __type. */
static int
ros_publisher_method(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                     UValue *out)
{
    (void)self;
    if (nargs != 2) return urbi_raise_arity(vm, "ros.publisher", 2, nargs, out);
    if (!urbi_value_is_str(args[0]))
        return urbi_raise_type(vm, "ros.publisher: topic must be a String", out);
    if (!urbi_value_is_str(args[1]))
        return urbi_raise_type(vm, "ros.publisher: type must be a String", out);

    size_t tl, yl;
    const char *topic = urbi_value_as_str(args[0], &tl);
    const char *type  = urbi_value_as_str(args[1], &yl);

    if (urbi_ros_msg_lookup(type) == NULL)
        return urbi_raise_type(vm, "ros.publisher: unknown message type", out);

    URosBridge *b = urbi_ros_bridge();
    if (!b->inited)
        return urbi_raise_type(vm, "ros.publisher: ros not initialized", out);

    uint32_t h = b->tp.create_pub(b->tp.self, topic, type);
    if (h == UROS_INVALID_HANDLE)
        return urbi_raise_type(vm, "ros.publisher: transport rejected publisher", out);

    if (g_publisher_proto == NULL)
        return urbi_raise_type(vm, "ros.publisher: publisher proto not initialized", out);

    struct UObject *po = urbi_object_clone(vm, g_publisher_proto);
    if (po == NULL)
        return urbi_raise_type(vm, "ros.publisher: OOM cloning publisher proto", out);

    UValue pv = urbi_make_object(po);

    /* Store handle as int. */
    if (urbi_slot_set(vm, pv, "__handle", 8, urbi_make_int((int64_t)h)) != URBI_OK)
        return urbi_raise_type(vm, "ros.publisher: OOM setting __handle", out);

    /* Store type as interned string. */
    UValue tsv = urbi_make_str_interned(vm, type, yl);
    if (tsv.kind == (uint8_t)UVAL_NIL)
        return urbi_raise_type(vm, "ros.publisher: OOM interning type string", out);
    if (urbi_slot_set(vm, pv, "__type", 6, tsv) != URBI_OK)
        return urbi_raise_type(vm, "ros.publisher: OOM setting __type", out);

    *out = pv;
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

    /* Build the Publisher proto: clone the root Object, install publish method,
     * then root it via a hidden slot on the ros proto so GC won't collect it. */
    g_publisher_proto = urbi_object_clone(vm, root);
    if (g_publisher_proto == NULL) return URBI_ERR_OOM;
    if (ros_register_method(vm, g_publisher_proto, "publish",
                            publisher_publish_method) != URBI_OK)
        return URBI_ERR_OOM;

    USymbol *pub_sym = (USymbol *)ustr_intern(vm, "__publisher_proto", 17);
    if (pub_sym == NULL) return URBI_ERR_OOM;
    {
        UValue pub_v = urbi_make_nil();
        pub_v.kind = (uint8_t)UVAL_OBJECT;
        pub_v.v.p  = (void *)g_publisher_proto;
        if (urbi_object_set_local_slot(vm, proto, pub_sym, pub_v) != 0)
            return URBI_ERR_OOM;
    }

    /* Install native methods on the ros proto. */
    UObject *rp = (UObject *)vm->ros_proto;
    if (ros_register_method(vm, rp, "init",      ros_init_method)      != URBI_OK
     || ros_register_method(vm, rp, "inited",    ros_inited_method)    != URBI_OK
     || ros_register_method(vm, rp, "msg",       ros_msg_method)       != URBI_OK
     || ros_register_method(vm, rp, "publisher", ros_publisher_method) != URBI_OK)
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
