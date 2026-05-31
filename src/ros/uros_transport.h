/* SPDX-License-Identifier: BSD-3-Clause */
/* src/ros/uros_transport.h — the swappable transport seam.
 * v0.12.0 shipped a byte-oriented seam; v0.12.1 revises to object-based:
 * each transport owns its own marshaling. publish/call receive UValue objects;
 * incoming messages are delivered via a URosDeliverFn callback during spin.
 * v0.12.1 implements the rclc/DDS transport behind URBI_ROS_BACKEND=rcl. */
#ifndef UROS_TRANSPORT_H
#define UROS_TRANSPORT_H
#ifdef URBI_ENABLE_ROS2

#include <stdint.h>
#include "urbi/types.h"

#define UROS_INVALID_HANDLE 0xFFFFFFFFu

struct UVM; /* forward declaration; defined in vm/uvm.h */

/* Callback: called by spin() for each incoming subscription message.
 * ud  — opaque userdata (the bridge pointer).
 * sub_handle — which subscription received the message.
 * msg_obj    — the deserialized message object (transport-owned lifetime until
 *              spin() returns; caller must copy if it needs to survive). */
typedef void (*URosDeliverFn)(void *ud, uint32_t sub_handle, UValue msg_obj);

/* Callback: called by the transport when an inbound service request arrives.
 * ud         — opaque userdata (the bridge pointer).
 * svc_handle — which service received the request.
 * req_obj    — the deserialized request object.
 * resp_obj   — output: the transport expects this to be filled by the handler.
 * Returns 0 on success, -1 on error. */
typedef int (*URosServeFn)(void *ud, uint32_t svc_handle, UValue req_obj,
                           UValue *resp_obj);

typedef struct URosTransport {
    void *self;
    /* Lifecycle */
    int      (*init)(void *self, const char *node_name);
    void     (*fini)(void *self);
    /* Endpoint creation */
    uint32_t (*create_pub)(void *self, const char *topic, const char *type);
    uint32_t (*create_sub)(void *self, const char *topic, const char *type);
    uint32_t (*create_client)(void *self, const char *service, const char *type);
    uint32_t (*create_service)(void *self, const char *service, const char *type);
    /* Endpoint teardown */
    void (*destroy_pub)(void *self, uint32_t pub);
    void (*destroy_sub)(void *self, uint32_t sub);
    void (*destroy_client)(void *self, uint32_t client);
    void (*destroy_service)(void *self, uint32_t svc);
    /* Publish: transport owns marshaling from msg_obj to the wire. */
    int  (*publish)(void *self, struct UVM *vm, uint32_t pub, UValue msg_obj);
    /* Spin: drain incoming messages, call deliver(ud, sub_handle, msg_obj)
     * for each.  Called once per urbi_step (cooperative, non-blocking). */
    int  (*spin)(void *self, struct UVM *vm, URosDeliverFn deliver, void *ud);
    /* Synchronous service call: transport marshals req, sends, waits, fills *resp. */
    int  (*call)(void *self, struct UVM *vm, uint32_t client, UValue req,
                 UValue *resp);
    /* Register the serve callback for an inbound service endpoint. */
    void (*set_service_handler)(void *self, uint32_t svc,
                                URosServeFn serve, void *ud);
} URosTransport;

#endif /* URBI_ENABLE_ROS2 */
#endif /* UROS_TRANSPORT_H */
