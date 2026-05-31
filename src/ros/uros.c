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
#include "watcher/uwatcher.h"  /* urbi_run_closure_on_scratch_with_payload */
#include "value/uintern.h"     /* ustr_intern */
#include "stdlib/object_root.h" /* urbi_native_closure_create, urbi_raise_arity, urbi_raise_type, urbi_raise_oom */
#include "ros/uros_internal.h" /* URosBridge, urbi_ros_bridge */
#include "ros/uros_mock.h"     /* uros_mock_init, uros_mock_inject */
#include "ros/uros_rcl.h"      /* uros_rcl_init/_free (container build only) */
#include "ros/uros_msg.h"      /* urbi_ros_msg_register_all */
#include "ros/generated/ros_msgs.gen.h" /* struct urbi_ros__std_msgs__Int32 */
#include "event/uevent.h"      /* urbi_event_create */
#include "event/uevent_native.h" /* uvalue_from_event */
#include "event/uevent_emit.h" /* c_event_emit_async */

#include <stddef.h> /* size_t */
#include <stdio.h>  /* snprintf */

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

/* Forward decl: mock-only test hook, defined near urbi_ros_pump below. */
static int ros_inject_int32_method(struct UVM *vm, UValue self, UValue *args,
                                   uint8_t nargs, UValue *out);

/* Backend factory — selects the transport implementation at compile time.
 * The container build (URBI_ROS_BACKEND_RCL) uses the real rcl/DDS transport;
 * the host build uses the in-memory mock. */
static void
ros_make_transport(URosTransport *tp)
{
#ifdef URBI_ROS_BACKEND_RCL
    uros_rcl_init(tp);
#else
    uros_mock_init(tp);
#endif
}

/* Free the transport state allocated by ros_make_transport. */
static void
ros_free_transport(URosTransport *tp)
{
#ifdef URBI_ROS_BACKEND_RCL
    uros_rcl_free(tp);
#else
    uros_mock_free(tp);
#endif
}

/* === ros.init(name) ===
 *
 * Initialise the transport (mock or rcl per build) with the given node name.
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
        ros_make_transport(&b->tp);
        size_t nlen;
        const char *nm = urbi_value_as_str(args[0], &nlen);
        (void)nlen;
        if (b->tp.init(b->tp.self, nm) != 0)
            return urbi_raise_type(vm, "ros.init: transport init failed", out);
        b->owner  = vm;
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

/* === ros.subscribe(topic, type) ===
 *
 * Create a subscriber endpoint on the current transport.  Allocates a fresh
 * UEvent, records it in the bridge subs[] table, and GC-roots it by storing
 * it as a hidden slot on vm->ros_proto keyed by handle.  Returns the UEvent
 * value so the caller can use `at (sub?(var m)) { ... }`. */
static int
ros_subscribe_method(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                     UValue *out)
{
    (void)self;
    if (nargs != 2) return urbi_raise_arity(vm, "ros.subscribe", 2, nargs, out);
    if (!urbi_value_is_str(args[0]) || !urbi_value_is_str(args[1]))
        return urbi_raise_type(vm, "ros.subscribe: topic and type must be Strings", out);
    URosBridge *b = urbi_ros_bridge();
    if (!b->inited)
        return urbi_raise_type(vm, "ros.subscribe: call ros.init first", out);
    if (b->sub_count >= UROS_MAX_SUBS)
        return urbi_raise_type(vm, "ros.subscribe: too many subscriptions", out);
    size_t topl, typl;
    const char *topic = urbi_value_as_str(args[0], &topl);
    const char *type  = urbi_value_as_str(args[1], &typl);
    (void)topl;
    const URosMsgType *mt = urbi_ros_msg_lookup(type);
    if (mt == NULL)
        return urbi_raise_type(vm, "ros.subscribe: unknown message type", out);
    uint32_t h = b->tp.create_sub(b->tp.self, topic, type);
    if (h == UROS_INVALID_HANDLE)
        return urbi_raise_type(vm, "ros.subscribe: transport error", out);
    /* Destroy the just-created transport endpoint on any later failure. */
    UEvent *e = urbi_event_create(vm);
    if (e == NULL) { b->tp.destroy_sub(b->tp.self, h); return urbi_raise_oom(vm, out); }
    b->subs[b->sub_count].handle = h;
    b->subs[b->sub_count].event  = e;
    b->subs[b->sub_count].type   = mt->name;
    b->sub_count++;
    /* GC-root the event via a hidden slot on ros_proto keyed by handle. */
    char slot[24];
    int sl = snprintf(slot, sizeof slot, "__sub_%u", h);
    USymbol *sy = (USymbol *)ustr_intern(vm, slot, (size_t)sl);
    if (sy == NULL) { b->tp.destroy_sub(b->tp.self, h); return urbi_raise_oom(vm, out); }
    if (urbi_object_set_local_slot(vm, (UObject *)vm->ros_proto, sy,
                                   uvalue_from_event(e)) != 0) {
        b->tp.destroy_sub(b->tp.self, h);
        return urbi_raise_oom(vm, out);
    }
    *out = uvalue_from_event(e);
    return UEXEC_OK;
}

/* === Client proto (file-static, GC-rooted via hidden slot on ros_proto) ===
 *
 * g_client_proto is cloned by ros.client() for each new client.
 * The proto itself is kept alive by a hidden slot installed on the ros_proto
 * during urbi_ros_register. */
static struct UObject *g_client_proto;

/* === Client.call(req[, respTypeIgnored]) ===
 *
 * Read __handle from self.  Pass req to the transport's call() function which
 * owns marshaling.  The transport fills *resp (the response object).
 * A second argument (response type name) is accepted for backward compatibility
 * with v0.12.0 script fixtures but is ignored — the transport knows the type. */
static int
client_call_method(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                   UValue *out)
{
    /* Accept 1 or 2 args: (req) or (req, respType) — respType is now ignored
     * because the transport owns marshaling and knows the service type. */
    if (nargs < 1 || nargs > 2)
        return urbi_raise_arity(vm, "Client.call", 1, nargs, out);

    /* Read __handle. */
    UValue hv = urbi_make_nil();
    if (urbi_slot_get(vm, self, "__handle", 8, &hv) != URBI_OK
     || !urbi_value_is_int(hv))
        return urbi_raise_type(vm, "Client.call: invalid client (missing __handle)", out);
    int64_t handle = urbi_value_as_int(hv);

    URosBridge *b = urbi_ros_bridge();
    if (!b->inited)
        return urbi_raise_type(vm, "Client.call: ros not initialized", out);

    UValue resp = urbi_make_nil();
    if (b->tp.call(b->tp.self, vm, (uint32_t)handle, args[0], &resp) != 0)
        return urbi_raise_type(vm, "Client.call: transport call failed", out);

    *out = resp;
    return UEXEC_OK;
}

/* === ros.client(service, type) ===
 *
 * Create a client endpoint on the current transport.  Returns a Client object
 * (clone of g_client_proto) with hidden slots __handle and __type. */
static int
ros_client_method(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                  UValue *out)
{
    (void)self;
    if (nargs != 2) return urbi_raise_arity(vm, "ros.client", 2, nargs, out);
    if (!urbi_value_is_str(args[0]))
        return urbi_raise_type(vm, "ros.client: service must be a String", out);
    if (!urbi_value_is_str(args[1]))
        return urbi_raise_type(vm, "ros.client: type must be a String", out);

    size_t sl, tl;
    const char *service = urbi_value_as_str(args[0], &sl);
    const char *type    = urbi_value_as_str(args[1], &tl);

    URosBridge *b = urbi_ros_bridge();
    if (!b->inited)
        return urbi_raise_type(vm, "ros.client: ros not initialized", out);

    /* The transport owns type resolution and rejects unknown types. */
    uint32_t h = b->tp.create_client(b->tp.self, service, type);
    if (h == UROS_INVALID_HANDLE)
        return urbi_raise_type(vm, "ros.client: unknown type or transport rejected", out);

    /* Destroy the just-created transport endpoint on any later failure. */
    if (g_client_proto == NULL) {
        b->tp.destroy_client(b->tp.self, h);
        return urbi_raise_type(vm, "ros.client: client proto not initialized", out);
    }

    struct UObject *co = urbi_object_clone(vm, g_client_proto);
    if (co == NULL) {
        b->tp.destroy_client(b->tp.self, h);
        return urbi_raise_type(vm, "ros.client: OOM cloning client proto", out);
    }

    UValue cv = urbi_make_object(co);

    if (urbi_slot_set(vm, cv, "__handle", 8, urbi_make_int((int64_t)h)) != URBI_OK) {
        b->tp.destroy_client(b->tp.self, h);
        return urbi_raise_type(vm, "ros.client: OOM setting __handle", out);
    }

    UValue tsv = urbi_make_str_interned(vm, type, tl);
    if (tsv.kind == (uint8_t)UVAL_NIL) {
        b->tp.destroy_client(b->tp.self, h);
        return urbi_raise_type(vm, "ros.client: OOM interning type string", out);
    }
    if (urbi_slot_set(vm, cv, "__type", 6, tsv) != URBI_OK) {
        b->tp.destroy_client(b->tp.self, h);
        return urbi_raise_type(vm, "ros.client: OOM setting __type", out);
    }

    *out = cv;
    return UEXEC_OK;
}

/* === bridge_serve + urbi_ros_invoke_handler ===
 *
 * bridge_serve: called by the transport when an inbound service request arrives.
 * It looks up the registered handler closure for the service handle and invokes
 * urbi_ros_invoke_handler.
 *
 * urbi_ros_invoke_handler (B7): synchronously run the stored urbiscript handler
 * closure with the request object as its first argument (R[0]) on a transient
 * scratch frame, and take its return value as the response object.  Returns 0
 * on a clean return, -1 if the handler is not a closure, threw, or the VM is
 * unavailable. */
static int
urbi_ros_invoke_handler(struct UVM *vm, UValue handler,
                        UValue req_obj, UValue *resp_obj)
{
    if (vm == NULL || handler.kind != (uint8_t)UVAL_CLOSURE) return -1;
    UValue result = urbi_make_nil();
    int    threw  = 0;
    int rc = urbi_run_closure_on_scratch_with_payload(
                 vm, (UClosure *)handler.v.p, req_obj, &result, &threw);
    if (rc != 0 || threw) return -1;
    *resp_obj = result;
    return 0;
}

static int
bridge_serve(void *ud, uint32_t svc_handle, UValue req_obj, UValue *resp_obj)
{
    URosBridge *b = (URosBridge *)ud;
    int i;
    for (i = 0; i < b->service_count; i++) {
        if (b->services[i].handle == svc_handle) {
            UValue handler = b->services[i].handler;
            return urbi_ros_invoke_handler(b->deliver_vm, handler,
                                           req_obj, resp_obj);
        }
    }
    return -1;
}

/* === ros.service(name, type, closure) ===
 *
 * Register a service endpoint and GC-root the handler closure.
 * The handler closure is stored in services[].handler and also rooted via a
 * hidden slot on ros_proto.  The transport is notified via set_service_handler
 * so it can route inbound requests to bridge_serve. */
static int
ros_service_method(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                   UValue *out)
{
    (void)self;
    if (nargs != 3) return urbi_raise_arity(vm, "ros.service", 3, nargs, out);
    if (!urbi_value_is_str(args[0]))
        return urbi_raise_type(vm, "ros.service: name must be a String", out);
    if (!urbi_value_is_str(args[1]))
        return urbi_raise_type(vm, "ros.service: type must be a String", out);
    if (args[2].kind != (uint8_t)UVAL_CLOSURE)
        return urbi_raise_type(vm, "ros.service: handler must be a closure", out);

    size_t nl, tl;
    const char *name = urbi_value_as_str(args[0], &nl);
    const char *type = urbi_value_as_str(args[1], &tl);

    URosBridge *b = urbi_ros_bridge();
    if (!b->inited)
        return urbi_raise_type(vm, "ros.service: ros not initialized", out);
    if (b->service_count >= UROS_MAX_SUBS)
        return urbi_raise_type(vm, "ros.service: too many services", out);

    /* The transport owns type resolution (message registry for the mock, srv
     * registry for rcl) and rejects unknown types via UROS_INVALID_HANDLE. */
    (void)tl;
    uint32_t h = b->tp.create_service(b->tp.self, name, type);
    if (h == UROS_INVALID_HANDLE)
        return urbi_raise_type(vm, "ros.service: unknown type or transport rejected", out);

    /* The transport tracks its own per-endpoint type; the bridge no longer
     * needs the type for marshaling (post object-seam). */
    b->services[b->service_count].handle  = h;
    b->services[b->service_count].type    = "";
    b->services[b->service_count].handler = args[2];
    b->service_count++;

    /* Notify the transport of the handler so it can route inbound requests. */
    if (b->tp.set_service_handler)
        b->tp.set_service_handler(b->tp.self, h, bridge_serve, b);

    /* GC-root the handler closure via a hidden slot on ros_proto keyed by handle.
     * On failure, roll back the bridge record + destroy the live DDS service. */
    char slot[32];
    int sl2 = snprintf(slot, sizeof slot, "__service_%u", h);
    USymbol *sy = (USymbol *)ustr_intern(vm, slot, (size_t)sl2);
    if (sy == NULL) {
        b->service_count--;
        b->tp.destroy_service(b->tp.self, h);
        return urbi_raise_oom(vm, out);
    }
    if (urbi_object_set_local_slot(vm, (UObject *)vm->ros_proto, sy, args[2]) != 0) {
        b->service_count--;
        b->tp.destroy_service(b->tp.self, h);
        return urbi_raise_oom(vm, out);
    }

    *out = urbi_make_nil();
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
 * Read __handle from self, then pass msg directly to the transport's publish()
 * function which owns marshaling.  The transport converts the UValue object
 * to wire format internally. */
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

    URosBridge *b = urbi_ros_bridge();
    if (!b->inited)
        return urbi_raise_type(vm, "Publisher.publish: ros not initialized", out);

    /* Transport owns marshaling from msg_obj to wire. */
    if (b->tp.publish(b->tp.self, vm, (uint32_t)handle, args[0]) != 0)
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

    /* From here, any failure must destroy the just-created transport endpoint
     * (it owns a live DDS publisher) before raising — else it leaks. */
    if (g_publisher_proto == NULL) {
        b->tp.destroy_pub(b->tp.self, h);
        return urbi_raise_type(vm, "ros.publisher: publisher proto not initialized", out);
    }

    struct UObject *po = urbi_object_clone(vm, g_publisher_proto);
    if (po == NULL) {
        b->tp.destroy_pub(b->tp.self, h);
        return urbi_raise_type(vm, "ros.publisher: OOM cloning publisher proto", out);
    }

    UValue pv = urbi_make_object(po);

    /* Store handle as int. */
    if (urbi_slot_set(vm, pv, "__handle", 8, urbi_make_int((int64_t)h)) != URBI_OK) {
        b->tp.destroy_pub(b->tp.self, h);
        return urbi_raise_type(vm, "ros.publisher: OOM setting __handle", out);
    }

    /* Store type as interned string (kept for diagnostics). */
    UValue tsv = urbi_make_str_interned(vm, type, yl);
    if (tsv.kind == (uint8_t)UVAL_NIL) {
        b->tp.destroy_pub(b->tp.self, h);
        return urbi_raise_type(vm, "ros.publisher: OOM interning type string", out);
    }
    if (urbi_slot_set(vm, pv, "__type", 6, tsv) != URBI_OK) {
        b->tp.destroy_pub(b->tp.self, h);
        return urbi_raise_type(vm, "ros.publisher: OOM setting __type", out);
    }

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

/* urbi_ros_shutdown: tear down the process-global bridge IFF it belongs to
 * `vm`.  Called from urbi_vm_destroy (gated) before the VM heap is reclaimed,
 * so a later VM never pumps this VM's freed UEvent* and the mock transport's
 * heap allocation is freed instead of leaked.  No-op if the bridge is
 * uninitialised or owned by a different VM. */
void
urbi_ros_shutdown(struct UVM *vm)
{
    URosBridge *b = urbi_ros_bridge();
    if (!b->inited || b->owner != vm) return;   /* only tear down our own */
    if (b->tp.fini) b->tp.fini(b->tp.self);
    ros_free_transport(&b->tp);                   /* frees the transport state */
    urbi_zero(b, sizeof *b);                      /* inited=0, owner=NULL, tables cleared */
    /* Drop the process-global proto caches that pointed into this VM's heap. */
    g_publisher_proto = NULL;
    g_client_proto = NULL;
    urbi_ros_msg__reset();                        /* clears uros_msg.c g_protos[] */
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

    /* Build the Client proto: clone root Object, install call method,
     * root via __client_proto slot on ros_proto. */
    g_client_proto = urbi_object_clone(vm, root);
    if (g_client_proto == NULL) return URBI_ERR_OOM;
    if (ros_register_method(vm, g_client_proto, "call",
                            client_call_method) != URBI_OK)
        return URBI_ERR_OOM;

    USymbol *cli_sym = (USymbol *)ustr_intern(vm, "__client_proto", 14);
    if (cli_sym == NULL) return URBI_ERR_OOM;
    {
        UValue cli_v = urbi_make_nil();
        cli_v.kind = (uint8_t)UVAL_OBJECT;
        cli_v.v.p  = (void *)g_client_proto;
        if (urbi_object_set_local_slot(vm, proto, cli_sym, cli_v) != 0)
            return URBI_ERR_OOM;
    }

    /* Install native methods on the ros proto. */
    UObject *rp = (UObject *)vm->ros_proto;
    if (ros_register_method(vm, rp, "init",      ros_init_method)      != URBI_OK
     || ros_register_method(vm, rp, "inited",    ros_inited_method)    != URBI_OK
     || ros_register_method(vm, rp, "msg",       ros_msg_method)       != URBI_OK
     || ros_register_method(vm, rp, "publisher", ros_publisher_method) != URBI_OK
     || ros_register_method(vm, rp, "subscribe", ros_subscribe_method) != URBI_OK
     || ros_register_method(vm, rp, "client",    ros_client_method)    != URBI_OK
     || ros_register_method(vm, rp, "service",   ros_service_method)   != URBI_OK
     || ros_register_method(vm, rp, "__injectInt32", ros_inject_int32_method) != URBI_OK)
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

/* === bridge_deliver callback ===
 *
 * Called by the transport's spin() for each incoming subscription message.
 * Finds the matching sub entry and emits the object onto its UEvent. */
static void
bridge_deliver(void *ud, uint32_t sub_handle, UValue msg_obj)
{
    URosBridge *b = (URosBridge *)ud;
    int i;
    for (i = 0; i < b->sub_count; i++) {
        if (b->subs[i].handle == sub_handle) {
            c_event_emit_async(b->deliver_vm, b->subs[i].event, msg_obj);
            return;
        }
    }
}

/* urbi_ros_pump: invoke the transport's spin() once, delivering incoming
 * messages onto their subscription UEvents via bridge_deliver.  Called once
 * per urbi_step (gated) and directly by unit tests.  No-op if ros.init() was
 * never called.  The deliver_vm field is set here so bridge_deliver and
 * bridge_serve can emit/invoke on the correct VM. */
void
urbi_ros_pump(struct UVM *vm)
{
    URosBridge *b = urbi_ros_bridge();
    /* Owner-scope: only pump the bridge for the VM that called ros.init().
     * A stale bridge left over from a freed VM (or a different live VM) must
     * never be polled — its UEvent* point into the other VM's heap. */
    if (vm == NULL || !b->inited || b->owner != vm) return;
    b->deliver_vm = vm;   /* used by bridge_deliver and bridge_serve */
    b->tp.spin(b->tp.self, vm, bridge_deliver, b);
}

/* === ros.__injectInt32(subHandle, value) ===
 *
 * Mock-only test hook: synthesize a std_msgs/Int32 message and inject it into
 * the mock transport's incoming queue for the given subscription handle so a
 * `.chk` fixture can drive the reactive loopback path without a C harness.
 * Uses the raw-bytes inject path because the generated C struct is immediately
 * available here; the bridge_deliver+spin path will unmarshal it via the
 * registered URosMsgType.
 * Absent once the real rclc/DDS transport lands (it will receive real wire
 * traffic instead). */
static int
ros_inject_int32_method(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                        UValue *out)
{
    (void)self;
    if (nargs != 2)
        return urbi_raise_arity(vm, "ros.__injectInt32", 2, nargs, out);
    if (!urbi_value_is_int(args[0]) || !urbi_value_is_int(args[1]))
        return urbi_raise_type(vm, "ros.__injectInt32: args must be Ints", out);
    URosBridge *b = urbi_ros_bridge();
    if (!b->inited)
        return urbi_raise_type(vm, "ros.__injectInt32: ros not initialized", out);
    struct urbi_ros__std_msgs__Int32 s;
    s.data = (int32_t)urbi_value_as_int(args[1]);
    uros_mock_inject(b->tp.self, (uint32_t)urbi_value_as_int(args[0]),
                     &s, sizeof s);
    *out = urbi_make_nil();
    return UEXEC_OK;
}

#else
/* Avoid ISO C "empty translation unit" (-Wpedantic) when this gated file is
 * compiled flag-free into build/host for the stdlib bake tool (TARGET != host). */
typedef int uros_translation_unit_not_empty;
#endif /* URBI_ENABLE_ROS2 */
