/* SPDX-License-Identifier: BSD-3-Clause */
/* src/ros/uros_rcl.c — real rcl/rclc/Fast-DDS transport backend.
 *
 * Compiled only in the container build (URBI_ROS_BACKEND_RCL); the host build
 * compiles this to an empty translation unit and uses the mock backend.
 *
 * B2: node init/fini.  B4: publisher + publish.  B5: subscriber + executor
 * spin.  B6/B7 (client/service) land next. */
#if defined(URBI_ENABLE_ROS2) && defined(URBI_ROS_BACKEND_RCL)

#define _POSIX_C_SOURCE 199309L   /* nanosleep / struct timespec under -std=c99 */
#include <time.h>

#include "ros/uros_rcl.h"
#include "ros/generated/ros_msgs_rcl.gen.h"
#include <stdlib.h>
#include <string.h>

#include <rcl/rcl.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>

#define RCL_MAX_PUBS     16
#define RCL_MAX_SUBS     16
#define RCL_MAX_CLIENTS   8
#define RCL_MAX_SVCS      8

typedef struct {
    int                 used;
    rcl_publisher_t     pub;
    const URosRclMsgType *mt;
} RclPub;

typedef struct {
    int                  used;
    rcl_subscription_t   sub;
    const URosRclMsgType *mt;
    void                *scratch;   /* heap rosidl struct the executor writes into */
    uint32_t             handle;    /* this sub's bridge-facing handle (== index) */
} RclSub;

typedef struct {
    int                  used;
    rcl_client_t         client;
    const URosRclSrvType *st;
} RclClient;

typedef struct {
    int                  used;
    rcl_service_t        svc;
    const URosRclSrvType *st;
    void                *req_scratch;   /* executor-owned request buffer */
    void                *resp_scratch;  /* executor-owned response buffer */
    uint32_t             handle;
    URosServeFn          serve;
    void                *serve_ud;
} RclSvc;

typedef struct URosRclState {
    rcl_allocator_t allocator;
    rclc_support_t  support;
    rcl_node_t      node;
    rclc_executor_t executor;
    int             executor_inited;
    int             inited;

    RclPub    pubs[RCL_MAX_PUBS];
    int       pub_count;
    RclSub    subs[RCL_MAX_SUBS];
    int       sub_count;
    RclClient clients[RCL_MAX_CLIENTS];
    int       client_count;
    RclSvc    svcs[RCL_MAX_SVCS];
    int       svc_count;

    /* Spin context — set on each spin()/call() so the per-sub deliver
     * trampoline and the per-svc serve trampoline have a VM + callbacks. */
    struct UVM    *spin_vm;
    URosDeliverFn  spin_deliver;
    void          *spin_ud;
} URosRclState;

/* The executor callback receives only the message pointer, so we need a way to
 * recover which subscription + which state fired.  rclc passes the message
 * buffer we registered (the sub's scratch); we keep a single global state
 * pointer (one bridge/VM at a time, matching the bridge's owner-VM model) and
 * scan subs by matching the scratch pointer. */
static URosRclState *g_active_state;

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

    /* One executor sized for all subs + services. */
    if (rclc_executor_init(&s->executor, &s->support.context,
                           RCL_MAX_SUBS + RCL_MAX_SVCS, &s->allocator) != RCL_RET_OK)
        return -1;
    s->executor_inited = 1;

    s->inited = 1;
    g_active_state = s;
    return 0;
}

static void
rcl_be_fini(void *self)
{
    URosRclState *s = (URosRclState *)self;
    int i;
    if (!s->inited) return;
    for (i = 0; i < s->pub_count; i++)
        if (s->pubs[i].used) {
            (void)rcl_publisher_fini(&s->pubs[i].pub, &s->node);
            s->pubs[i].used = 0;
        }
    for (i = 0; i < s->sub_count; i++)
        if (s->subs[i].used) {
            (void)rcl_subscription_fini(&s->subs[i].sub, &s->node);
            if (s->subs[i].mt && s->subs[i].scratch) s->subs[i].mt->fini(s->subs[i].scratch);
            free(s->subs[i].scratch);
            s->subs[i].scratch = NULL;
            s->subs[i].used = 0;
        }
    for (i = 0; i < s->client_count; i++)
        if (s->clients[i].used) {
            (void)rcl_client_fini(&s->clients[i].client, &s->node);
            s->clients[i].used = 0;
        }
    for (i = 0; i < s->svc_count; i++)
        if (s->svcs[i].used) {
            (void)rcl_service_fini(&s->svcs[i].svc, &s->node);
            if (s->svcs[i].st) {
                if (s->svcs[i].req_scratch)  s->svcs[i].st->req_fini(s->svcs[i].req_scratch);
                if (s->svcs[i].resp_scratch) s->svcs[i].st->resp_fini(s->svcs[i].resp_scratch);
            }
            free(s->svcs[i].req_scratch);
            free(s->svcs[i].resp_scratch);
            s->svcs[i].req_scratch = s->svcs[i].resp_scratch = NULL;
            s->svcs[i].used = 0;
        }
    if (s->executor_inited) {
        (void)rclc_executor_fini(&s->executor);
        s->executor_inited = 0;
    }
    (void)rcl_node_fini(&s->node);
    (void)rclc_support_fini(&s->support);
    s->inited = 0;
    if (g_active_state == s) g_active_state = NULL;
}

/* --- Publisher (B4) --- */
static uint32_t
rcl_be_create_pub(void *self, const char *topic, const char *type)
{
    URosRclState *s = (URosRclState *)self;
    if (s->pub_count >= RCL_MAX_PUBS) return UROS_INVALID_HANDLE;
    const URosRclMsgType *mt = urbi_ros_rcl_msg_lookup(type);
    if (mt == NULL) return UROS_INVALID_HANDLE;
    RclPub *p = &s->pubs[s->pub_count];
    p->pub = rcl_get_zero_initialized_publisher();
    if (rclc_publisher_init_default(&p->pub, &s->node, mt->ts(), topic) != RCL_RET_OK)
        return UROS_INVALID_HANDLE;
    p->mt   = mt;
    p->used = 1;
    return (uint32_t)s->pub_count++;
}

static int
rcl_be_publish(void *self, struct UVM *vm, uint32_t pub, UValue msg)
{
    URosRclState *s = (URosRclState *)self;
    if (pub >= (uint32_t)s->pub_count || !s->pubs[pub].used) return -1;
    RclPub *p = &s->pubs[pub];
    void *scratch = calloc(1, p->mt->c_size);
    if (scratch == NULL) return -1;
    p->mt->init(scratch);
    int rc = -1;
    if (p->mt->marshal(vm, msg, scratch) == 0 &&
        rcl_publish(&p->pub, scratch, NULL) == RCL_RET_OK)
        rc = 0;
    p->mt->fini(scratch);
    free(scratch);
    return rc;
}

/* --- Subscriber + executor spin (B5) --- */
static void
rcl_sub_trampoline(const void *msgin)
{
    URosRclState *s = g_active_state;
    int i;
    if (s == NULL || s->spin_deliver == NULL) return;
    for (i = 0; i < s->sub_count; i++) {
        if (s->subs[i].used && s->subs[i].scratch == msgin) {
            UValue obj = urbi_make_nil();
            if (s->subs[i].mt->unmarshal(s->spin_vm, msgin, &obj) == 0)
                s->spin_deliver(s->spin_ud, s->subs[i].handle, obj);
            return;
        }
    }
}

static uint32_t
rcl_be_create_sub(void *self, const char *topic, const char *type)
{
    URosRclState *s = (URosRclState *)self;
    if (s->sub_count >= RCL_MAX_SUBS) return UROS_INVALID_HANDLE;
    const URosRclMsgType *mt = urbi_ros_rcl_msg_lookup(type);
    if (mt == NULL) return UROS_INVALID_HANDLE;
    RclSub *sb = &s->subs[s->sub_count];
    sb->sub = rcl_get_zero_initialized_subscription();
    if (rclc_subscription_init_default(&sb->sub, &s->node, mt->ts(), topic) != RCL_RET_OK)
        return UROS_INVALID_HANDLE;
    sb->scratch = calloc(1, mt->c_size);
    if (sb->scratch == NULL) {
        (void)rcl_subscription_fini(&sb->sub, &s->node);
        return UROS_INVALID_HANDLE;
    }
    mt->init(sb->scratch);
    if (rclc_executor_add_subscription(&s->executor, &sb->sub, sb->scratch,
                                       &rcl_sub_trampoline, ON_NEW_DATA) != RCL_RET_OK) {
        mt->fini(sb->scratch);
        free(sb->scratch);
        (void)rcl_subscription_fini(&sb->sub, &s->node);
        return UROS_INVALID_HANDLE;
    }
    sb->mt     = mt;
    sb->handle = (uint32_t)s->sub_count;
    sb->used   = 1;
    return (uint32_t)s->sub_count++;
}

static int
rcl_be_spin(void *self, struct UVM *vm, URosDeliverFn deliver, void *ud)
{
    URosRclState *s = (URosRclState *)self;
    if (!s->inited || !s->executor_inited) return 0;
    s->spin_vm      = vm;
    s->spin_deliver = deliver;
    s->spin_ud      = ud;
    g_active_state  = s;
    /* Non-blocking: drain whatever DDS has delivered, return immediately. */
    (void)rclc_executor_spin_some(&s->executor, 0);
    return 0;
}

/* --- Service client + call (B6) --- */
static uint32_t
rcl_be_create_client(void *self, const char *service, const char *type)
{
    URosRclState *s = (URosRclState *)self;
    if (s->client_count >= RCL_MAX_CLIENTS) return UROS_INVALID_HANDLE;
    const URosRclSrvType *st = urbi_ros_rcl_srv_lookup(type);
    if (st == NULL) return UROS_INVALID_HANDLE;
    RclClient *c = &s->clients[s->client_count];
    c->client = rcl_get_zero_initialized_client();
    if (rclc_client_init_default(&c->client, &s->node, st->ts(), service) != RCL_RET_OK)
        return UROS_INVALID_HANDLE;
    c->st   = st;
    c->used = 1;
    return (uint32_t)s->client_count++;
}

static int
rcl_be_call(void *self, struct UVM *vm, uint32_t cli, UValue req, UValue *resp)
{
    URosRclState *s = (URosRclState *)self;
    int rc = -1, tries;
    if (cli >= (uint32_t)s->client_count || !s->clients[cli].used) return -1;
    RclClient *c = &s->clients[cli];
    void *reqs  = calloc(1, c->st->req_size);
    void *resps = calloc(1, c->st->resp_size);
    if (reqs == NULL || resps == NULL) { free(reqs); free(resps); return -1; }
    c->st->req_init(reqs);
    c->st->resp_init(resps);
    /* The service may live in this same process (single-threaded); spin the
     * executor between send and take so its trampoline can run. */
    s->spin_vm = vm;
    g_active_state = s;
    int64_t seq = 0;
    if (c->st->marshal_req(vm, req, reqs) == 0 &&
        rcl_send_request(&c->client, reqs, &seq) == RCL_RET_OK) {
        for (tries = 0; tries < 400; tries++) {
            rmw_request_id_t hdr;
            (void)rclc_executor_spin_some(&s->executor, 0);
            if (rcl_take_response(&c->client, &hdr, resps) == RCL_RET_OK) {
                if (c->st->unmarshal_resp(vm, resps, resp) == 0) rc = 0;
                break;
            }
            { struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 5 * 1000 * 1000;
              nanosleep(&ts, NULL); }
        }
    }
    c->st->req_fini(reqs);  free(reqs);
    c->st->resp_fini(resps); free(resps);
    return rc;
}

/* --- Service server + handler invocation (B7) --- */
static void
rcl_svc_trampoline(const void *req, void *resp)
{
    URosRclState *s = g_active_state;
    int i;
    if (s == NULL) return;
    for (i = 0; i < s->svc_count; i++) {
        if (s->svcs[i].used && s->svcs[i].req_scratch == req && s->svcs[i].serve) {
            UValue req_obj  = urbi_make_nil();
            UValue resp_obj = urbi_make_nil();
            if (s->svcs[i].st->unmarshal_req(s->spin_vm, req, &req_obj) == 0 &&
                s->svcs[i].serve(s->svcs[i].serve_ud, s->svcs[i].handle,
                                 req_obj, &resp_obj) == 0)
                (void)s->svcs[i].st->marshal_resp(s->spin_vm, resp_obj, resp);
            return;
        }
    }
}

static uint32_t
rcl_be_create_service(void *self, const char *service, const char *type)
{
    URosRclState *s = (URosRclState *)self;
    if (s->svc_count >= RCL_MAX_SVCS) return UROS_INVALID_HANDLE;
    const URosRclSrvType *st = urbi_ros_rcl_srv_lookup(type);
    if (st == NULL) return UROS_INVALID_HANDLE;
    RclSvc *sv = &s->svcs[s->svc_count];
    sv->svc = rcl_get_zero_initialized_service();
    if (rclc_service_init_default(&sv->svc, &s->node, st->ts(), service) != RCL_RET_OK)
        return UROS_INVALID_HANDLE;
    sv->req_scratch  = calloc(1, st->req_size);
    sv->resp_scratch = calloc(1, st->resp_size);
    if (sv->req_scratch == NULL || sv->resp_scratch == NULL) {
        free(sv->req_scratch); free(sv->resp_scratch);
        (void)rcl_service_fini(&sv->svc, &s->node);
        return UROS_INVALID_HANDLE;
    }
    st->req_init(sv->req_scratch);
    st->resp_init(sv->resp_scratch);
    if (rclc_executor_add_service(&s->executor, &sv->svc, sv->req_scratch,
                                  sv->resp_scratch, &rcl_svc_trampoline) != RCL_RET_OK) {
        st->req_fini(sv->req_scratch);  free(sv->req_scratch);
        st->resp_fini(sv->resp_scratch); free(sv->resp_scratch);
        (void)rcl_service_fini(&sv->svc, &s->node);
        return UROS_INVALID_HANDLE;
    }
    sv->st     = st;
    sv->handle = (uint32_t)s->svc_count;
    sv->used   = 1;
    return (uint32_t)s->svc_count++;
}

static void
rcl_be_set_service_handler(void *self, uint32_t svc,
                           URosServeFn serve, void *ud)
{
    URosRclState *s = (URosRclState *)self;
    if (svc < (uint32_t)s->svc_count && s->svcs[svc].used) {
        s->svcs[svc].serve    = serve;
        s->svcs[svc].serve_ud = ud;
    }
}

static void
rcl_be_destroy_pub(void *self, uint32_t h)
{
    URosRclState *s = (URosRclState *)self;
    if (h < (uint32_t)s->pub_count && s->pubs[h].used) {
        (void)rcl_publisher_fini(&s->pubs[h].pub, &s->node);
        s->pubs[h].used = 0;
    }
}
/* NOTE: destroy_sub/destroy_service fini the rcl entity + free scratch, but do
 * NOT remove it from the rclc executor (rclc has no stable per-entry remove
 * across versions).  Destroy is intended for error-unwind (create succeeded,
 * a later step raised — the executor is torn down right after via fini) and for
 * shutdown.  Destroying an endpoint and then continuing to spin the executor
 * will log "subscription's implementation is invalid"; steady-state
 * destroy-then-recreate is a v0.12.1 follow-up. */
static void
rcl_be_destroy_sub(void *self, uint32_t h)
{
    URosRclState *s = (URosRclState *)self;
    if (h < (uint32_t)s->sub_count && s->subs[h].used) {
        (void)rcl_subscription_fini(&s->subs[h].sub, &s->node);
        if (s->subs[h].mt && s->subs[h].scratch) s->subs[h].mt->fini(s->subs[h].scratch);
        free(s->subs[h].scratch);
        s->subs[h].scratch = NULL;
        s->subs[h].used = 0;
    }
}
static void
rcl_be_destroy_client(void *self, uint32_t h)
{
    URosRclState *s = (URosRclState *)self;
    if (h < (uint32_t)s->client_count && s->clients[h].used) {
        (void)rcl_client_fini(&s->clients[h].client, &s->node);
        s->clients[h].used = 0;
    }
}
static void
rcl_be_destroy_service(void *self, uint32_t h)
{
    URosRclState *s = (URosRclState *)self;
    if (h < (uint32_t)s->svc_count && s->svcs[h].used) {
        (void)rcl_service_fini(&s->svcs[h].svc, &s->node);
        if (s->svcs[h].st) {
            if (s->svcs[h].req_scratch)  s->svcs[h].st->req_fini(s->svcs[h].req_scratch);
            if (s->svcs[h].resp_scratch) s->svcs[h].st->resp_fini(s->svcs[h].resp_scratch);
        }
        free(s->svcs[h].req_scratch);
        free(s->svcs[h].resp_scratch);
        s->svcs[h].req_scratch = s->svcs[h].resp_scratch = NULL;
        s->svcs[h].used = 0;
    }
}

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
/* Empty-TU guard for the host build. */
typedef int uros_rcl_translation_unit_not_empty;
#endif /* URBI_ENABLE_ROS2 && URBI_ROS_BACKEND_RCL */
