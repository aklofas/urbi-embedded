/* SPDX-License-Identifier: BSD-3-Clause */
/* src/ros/uros_transport.h — the swappable transport seam.
 * v0.12.0 ships only the mock implementation; v0.12.1 adds the rclc/DDS one. */
#ifndef UROS_TRANSPORT_H
#define UROS_TRANSPORT_H
#ifdef URBI_ENABLE_ROS2

#include <stddef.h>
#include <stdint.h>

#define UROS_INVALID_HANDLE 0xFFFFFFFFu

/* One incoming message dequeued by poll(): which subscription + the raw
 * serialized struct bytes (transport-owned until the next poll). */
typedef struct {
    uint32_t    sub_handle;
    const void *bytes;
    size_t      len;
} URosIncoming;

typedef struct URosTransport {
    void *self;
    int  (*init)(void *self, const char *node_name);
    uint32_t (*create_pub)(void *self, const char *topic, const char *type);
    uint32_t (*create_sub)(void *self, const char *topic, const char *type);
    uint32_t (*create_client)(void *self, const char *service, const char *type);
    uint32_t (*create_service)(void *self, const char *service, const char *type);
    int  (*publish)(void *self, uint32_t pub, const void *bytes, size_t len);
    int  (*poll)(void *self, URosIncoming *out);
    int  (*call)(void *self, uint32_t client, const void *req, size_t req_len,
                 void *resp, size_t resp_cap, size_t *resp_len);
    void (*fini)(void *self);
} URosTransport;

#endif /* URBI_ENABLE_ROS2 */
#endif /* UROS_TRANSPORT_H */
