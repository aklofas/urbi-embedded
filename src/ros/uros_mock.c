/* SPDX-License-Identifier: BSD-3-Clause */
/* src/ros/uros_mock.c — in-memory mock ROS2 transport (object-based seam).
 *
 * The mock marshals published UValue objects into internal byte blobs via
 * the codegen marshal functions, and unmarshals them back during spin().
 * This keeps the deterministic test path fully standalone (zero ROS2 dep)
 * while exercising the same object<->struct round-trip the real rcl transport
 * will use in Phase B.
 *
 * Internal blob capacity is fixed (mock-only):
 *   per-publish store: 512 bytes
 *   per-sub FIFO:      8 entries x 512 bytes
 */
#ifdef URBI_ENABLE_ROS2
#include "ros/uros_mock.h"
#include "ros/uros_msg.h"      /* urbi_ros_msg_lookup, URosMsgType */
#include <stdlib.h>
#include <string.h>

#define MOCK_MAX_EP   32
#define MOCK_FIFO     8
#define MOCK_BLOB_CAP 512

typedef struct { unsigned char buf[MOCK_BLOB_CAP]; size_t len; } MockBlob;

typedef struct {
    int      used;
    int      is_sub;     /* 1 = subscriber endpoint; 0 = publisher/client */
    char     type[64];   /* ROS type name stored at create time */
    /* Publisher: last published blob (for uros_mock_last_published). */
    MockBlob last_pub;
    /* Subscriber: FIFO of injected blobs. */
    MockBlob fifo[MOCK_FIFO];
    int      head, tail, count;
    /* Service handler (registered via set_service_handler; unused by mock). */
    URosServeFn  serve_fn;
    void        *serve_ud;
} MockEP;

typedef struct {
    MockEP ep[MOCK_MAX_EP];
    int    n;
    int    poll_cursor;
} MockState;

static uint32_t
mock_alloc_ep(MockState *m, int is_sub, const char *type)
{
    if (m->n >= MOCK_MAX_EP) return UROS_INVALID_HANDLE;
    uint32_t h = (uint32_t)m->n++;
    memset(&m->ep[h], 0, sizeof(m->ep[h]));
    m->ep[h].used   = 1;
    m->ep[h].is_sub = is_sub;
    strncpy(m->ep[h].type, type, sizeof(m->ep[h].type) - 1);
    return h;
}

static int
mock_init(void *self, const char *name)
{
    (void)self; (void)name;
    return 0;
}

static uint32_t
mock_pub(void *self, const char *topic, const char *ty)
{
    (void)topic;
    return mock_alloc_ep((MockState *)self, 0, ty);
}

static uint32_t
mock_sub(void *self, const char *topic, const char *ty)
{
    (void)topic;
    return mock_alloc_ep((MockState *)self, 1, ty);
}

static uint32_t
mock_cli(void *self, const char *svc, const char *ty)
{
    (void)svc;
    return mock_alloc_ep((MockState *)self, 0, ty);
}

static uint32_t
mock_srv(void *self, const char *svc, const char *ty)
{
    (void)svc;
    return mock_alloc_ep((MockState *)self, 1, ty);
}

/* Teardown stubs — no dynamic resources per-endpoint in the mock. */
static void mock_destroy_pub(void *self, uint32_t pub) { (void)self; (void)pub; }
static void mock_destroy_sub(void *self, uint32_t sub) { (void)self; (void)sub; }
static void mock_destroy_client(void *self, uint32_t cli) { (void)self; (void)cli; }
static void mock_destroy_service(void *self, uint32_t svc) { (void)self; (void)svc; }

/* Publish: marshal the UValue object into the endpoint's last_pub blob. */
static int
mock_publish(void *self, struct UVM *vm, uint32_t pub, UValue msg_obj)
{
    MockState *m = (MockState *)self;
    if (pub >= (uint32_t)m->n || !m->ep[pub].used) return -1;
    MockEP *e = &m->ep[pub];
    const URosMsgType *mt = urbi_ros_msg_lookup(e->type);
    if (mt == NULL || mt->c_size > MOCK_BLOB_CAP) return -1;
    memset(e->last_pub.buf, 0, mt->c_size);
    if (mt->marshal(vm, msg_obj, e->last_pub.buf) != 0) return -1;
    e->last_pub.len = mt->c_size;
    return 0;
}

/* Spin: unmarshal each injected blob and call deliver(ud, sub, obj). */
static int
mock_spin(void *self, struct UVM *vm, URosDeliverFn deliver, void *ud)
{
    MockState *m = (MockState *)self;
    int n = 0;
    int i;
    /* Round-robin across all subscriber endpoints. */
    for (i = 0; i < m->n && n < 64; i++) {
        int idx = (m->poll_cursor + i) % (m->n > 0 ? m->n : 1);
        MockEP *e = &m->ep[idx];
        if (e->used && e->is_sub && e->count > 0) {
            const MockBlob *blob = &e->fifo[e->head];
            e->head  = (e->head + 1) % MOCK_FIFO;
            e->count--;
            m->poll_cursor = (idx + 1) % m->n;
            /* Unmarshal the blob into a fresh object and deliver. */
            const URosMsgType *mt = urbi_ros_msg_lookup(e->type);
            if (mt == NULL || blob->len != mt->c_size) continue;
            UValue msg_obj;
            memset(&msg_obj, 0, sizeof msg_obj);
            if (mt->unmarshal(vm, blob->buf, &msg_obj) != 0) continue;
            deliver(ud, (uint32_t)idx, msg_obj);
            n++;
        }
    }
    return 0;
}

/* Synchronous call: echo req object back as resp (mock transport has no
 * real service server; this simulates the identity response). */
static int
mock_call(void *self, struct UVM *vm, uint32_t cli, UValue req, UValue *resp)
{
    (void)self; (void)vm; (void)cli;
    /* Echo: the response is the same object as the request. */
    *resp = req;
    return 0;
}

/* Store the service handler (unused by the mock — no inbound request path). */
static void
mock_set_service_handler(void *self, uint32_t svc,
                         URosServeFn serve, void *ud)
{
    MockState *m = (MockState *)self;
    if (svc < (uint32_t)m->n && m->ep[svc].used) {
        m->ep[svc].serve_fn = serve;
        m->ep[svc].serve_ud = ud;
    }
}

static void
mock_fini(void *self)
{
    (void)self;
}

/* Mock vtable — all function-pointer fields; self is set at runtime. */
static const URosTransport MOCK_VTABLE = {
    .init               = mock_init,
    .fini               = mock_fini,
    .create_pub         = mock_pub,
    .create_sub         = mock_sub,
    .create_client      = mock_cli,
    .create_service     = mock_srv,
    .destroy_pub        = mock_destroy_pub,
    .destroy_sub        = mock_destroy_sub,
    .destroy_client     = mock_destroy_client,
    .destroy_service    = mock_destroy_service,
    .publish            = mock_publish,
    .spin               = mock_spin,
    .call               = mock_call,
    .set_service_handler = mock_set_service_handler,
};

void
uros_mock_init(URosTransport *tp)
{
    MockState *m = (MockState *)calloc(1, sizeof(MockState));
    *tp       = MOCK_VTABLE;
    tp->self  = m;
}

void
uros_mock_free(URosTransport *tp)
{
    free(tp->self);
    tp->self = NULL;
}

/* uros_mock_inject_obj: marshal msg_obj into an internal blob and enqueue it
 * for delivery to the given subscription handle during the next spin(). */
void
uros_mock_inject_obj(void *self, struct UVM *vm,
                     uint32_t sub, const char *type, UValue msg_obj)
{
    MockState *m = (MockState *)self;
    if (sub >= (uint32_t)m->n || !m->ep[sub].used) return;
    MockEP *e = &m->ep[sub];
    if (e->count >= MOCK_FIFO) return;
    const URosMsgType *mt = urbi_ros_msg_lookup(type);
    if (mt == NULL || mt->c_size > MOCK_BLOB_CAP) return;
    MockBlob *slot = &e->fifo[e->tail];
    memset(slot->buf, 0, mt->c_size);
    if (mt->marshal(vm, msg_obj, slot->buf) != 0) return;
    slot->len = mt->c_size;
    e->tail   = (e->tail + 1) % MOCK_FIFO;
    e->count++;
}

/* uros_mock_inject: raw-bytes inject (used by ros_inject_int32_method which
 * builds the struct itself via the generated C type).  Prefer
 * uros_mock_inject_obj for new code. */
void
uros_mock_inject(void *self, uint32_t sub, const void *bytes, size_t len)
{
    MockState *m = (MockState *)self;
    if (sub >= (uint32_t)m->n || len > MOCK_BLOB_CAP) return;
    MockEP *e = &m->ep[sub];
    if (e->count >= MOCK_FIFO) return;
    memcpy(e->fifo[e->tail].buf, bytes, len);
    e->fifo[e->tail].len = len;
    e->tail  = (e->tail + 1) % MOCK_FIFO;
    e->count++;
}

int
uros_mock_last_published(void *self, uint32_t pub,
                         const void **out_bytes, size_t *out_len)
{
    MockState *m = (MockState *)self;
    if (pub >= (uint32_t)m->n) return 0;
    *out_bytes = m->ep[pub].last_pub.buf;
    *out_len   = m->ep[pub].last_pub.len;
    return m->ep[pub].last_pub.len > 0 ? 1 : 0;
}

const char *
uros_mock_pub_type(void *self, uint32_t pub)
{
    MockState *m = (MockState *)self;
    if (pub >= (uint32_t)m->n) return NULL;
    return m->ep[pub].type;
}

const char *
uros_mock_sub_type(void *self, uint32_t sub)
{
    MockState *m = (MockState *)self;
    if (sub >= (uint32_t)m->n) return NULL;
    return m->ep[sub].type;
}

#else
/* Avoid ISO C "empty translation unit" (-Wpedantic) when this gated file is
 * compiled flag-free into build/host for the stdlib bake tool (TARGET != host). */
typedef int uros_translation_unit_not_empty;
#endif /* URBI_ENABLE_ROS2 */
