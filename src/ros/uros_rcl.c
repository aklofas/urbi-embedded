/* SPDX-License-Identifier: BSD-3-Clause */
/* src/ros/uros_rcl.c — real rcl/rclc/Fast-DDS transport backend.
 *
 * Compiled only in the container build (URBI_ROS_BACKEND_RCL); the host build
 * compiles this to an empty translation unit and uses the mock backend.
 *
 * B2 implements node init/fini.  Publisher/subscriber/client/service and the
 * spin/marshal paths are filled by B4-B7. */
#if defined(URBI_ENABLE_ROS2) && defined(URBI_ROS_BACKEND_RCL)

#include "ros/uros_rcl.h"
#include <stdlib.h>

#include <rcl/rcl.h>
#include <rclc/rclc.h>

typedef struct URosRclState {
    rcl_allocator_t allocator;
    rclc_support_t  support;
    rcl_node_t      node;
    int             inited;
} URosRclState;

static int
rcl_be_init(void *self, const char *node_name)
{
    URosRclState *s = (URosRclState *)self;
    s->allocator = rcl_get_default_allocator();

    rcl_init_options_t opts = rcl_get_zero_initialized_init_options();
    if (rcl_init_options_init(&opts, s->allocator) != RCL_RET_OK)
        return -1;
    rcl_ret_t rc = rclc_support_init_with_options(&s->support, 0, NULL,
                                                  &opts, &s->allocator);
    (void)rcl_init_options_fini(&opts);
    if (rc != RCL_RET_OK)
        return -1;

    if (rclc_node_init_default(&s->node, node_name, "", &s->support) != RCL_RET_OK)
        return -1;

    s->inited = 1;
    return 0;
}

static void
rcl_be_fini(void *self)
{
    URosRclState *s = (URosRclState *)self;
    if (s->inited) {
        (void)rcl_node_fini(&s->node);
        (void)rclc_support_fini(&s->support);
        s->inited = 0;
    }
}

/* Endpoint + transfer ops — stubs until B4-B7. */
static uint32_t rcl_be_create_pub(void *self, const char *t, const char *ty)
{ (void)self; (void)t; (void)ty; return UROS_INVALID_HANDLE; }
static uint32_t rcl_be_create_sub(void *self, const char *t, const char *ty)
{ (void)self; (void)t; (void)ty; return UROS_INVALID_HANDLE; }
static uint32_t rcl_be_create_client(void *self, const char *s, const char *ty)
{ (void)self; (void)s; (void)ty; return UROS_INVALID_HANDLE; }
static uint32_t rcl_be_create_service(void *self, const char *s, const char *ty)
{ (void)self; (void)s; (void)ty; return UROS_INVALID_HANDLE; }
static void rcl_be_destroy_pub(void *self, uint32_t h) { (void)self; (void)h; }
static void rcl_be_destroy_sub(void *self, uint32_t h) { (void)self; (void)h; }
static void rcl_be_destroy_client(void *self, uint32_t h) { (void)self; (void)h; }
static void rcl_be_destroy_service(void *self, uint32_t h) { (void)self; (void)h; }
static int rcl_be_publish(void *self, struct UVM *vm, uint32_t pub, UValue msg)
{ (void)self; (void)vm; (void)pub; (void)msg; return -1; }
static int rcl_be_spin(void *self, struct UVM *vm, URosDeliverFn deliver, void *ud)
{ (void)self; (void)vm; (void)deliver; (void)ud; return 0; }
static int rcl_be_call(void *self, struct UVM *vm, uint32_t cli, UValue req, UValue *resp)
{ (void)self; (void)vm; (void)cli; (void)req; (void)resp; return -1; }
static void rcl_be_set_service_handler(void *self, uint32_t svc,
                                       URosServeFn serve, void *ud)
{ (void)self; (void)svc; (void)serve; (void)ud; }

void
uros_rcl_init(URosTransport *tp)
{
    URosRclState *s = (URosRclState *)calloc(1, sizeof *s);
    tp->self                = s;
    tp->init                = rcl_be_init;
    tp->fini                = rcl_be_fini;
    tp->create_pub          = rcl_be_create_pub;
    tp->create_sub          = rcl_be_create_sub;
    tp->create_client       = rcl_be_create_client;
    tp->create_service      = rcl_be_create_service;
    tp->destroy_pub         = rcl_be_destroy_pub;
    tp->destroy_sub         = rcl_be_destroy_sub;
    tp->destroy_client      = rcl_be_destroy_client;
    tp->destroy_service     = rcl_be_destroy_service;
    tp->publish             = rcl_be_publish;
    tp->spin                = rcl_be_spin;
    tp->call                = rcl_be_call;
    tp->set_service_handler = rcl_be_set_service_handler;
}

void
uros_rcl_free(URosTransport *tp)
{
    free(tp->self);
    tp->self = NULL;
}

#else
/* Empty-TU guard for the host build (URBI_ROS_BACKEND_RCL undefined). */
typedef int uros_rcl_translation_unit_not_empty;
#endif /* URBI_ENABLE_ROS2 && URBI_ROS_BACKEND_RCL */
