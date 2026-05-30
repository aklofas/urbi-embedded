/* SPDX-License-Identifier: BSD-3-Clause */
#ifdef URBI_ENABLE_ROS2
#include "ros/uros_mock.h"
#include <stdlib.h>
#include <string.h>

#define MOCK_MAX_EP   32
#define MOCK_FIFO     8
#define MOCK_BLOB_CAP 512

typedef struct { unsigned char buf[MOCK_BLOB_CAP]; size_t len; } MockBlob;
typedef struct {
    int      used, is_sub;
    MockBlob last_pub;
    MockBlob fifo[MOCK_FIFO];
    int      head, tail, count;
} MockEP;
typedef struct { MockEP ep[MOCK_MAX_EP]; int n; int poll_cursor; } MockState;

static uint32_t mock_alloc_ep(MockState *m, int is_sub) {
    if (m->n >= MOCK_MAX_EP) return UROS_INVALID_HANDLE;
    uint32_t h = (uint32_t)m->n++;
    m->ep[h].used = 1; m->ep[h].is_sub = is_sub;
    m->ep[h].head = m->ep[h].tail = m->ep[h].count = 0;
    return h;
}
static int mock_init(void *self, const char *name){ (void)self;(void)name; return 0; }
static uint32_t mock_pub(void *self, const char *t, const char *ty){ (void)t;(void)ty; return mock_alloc_ep((MockState*)self,0); }
static uint32_t mock_sub(void *self, const char *t, const char *ty){ (void)t;(void)ty; return mock_alloc_ep((MockState*)self,1); }
static uint32_t mock_cli(void *self, const char *s, const char *ty){ (void)s;(void)ty; return mock_alloc_ep((MockState*)self,0); }
static uint32_t mock_srv(void *self, const char *s, const char *ty){ (void)s;(void)ty; return mock_alloc_ep((MockState*)self,1); }
static int mock_publish(void *self, uint32_t pub, const void *b, size_t len){
    MockState *m=(MockState*)self;
    if (pub>=(uint32_t)m->n || len>MOCK_BLOB_CAP) return -1;
    memcpy(m->ep[pub].last_pub.buf,b,len); m->ep[pub].last_pub.len=len; return 0;
}
static int mock_poll(void *self, URosIncoming *out){
    MockState *m=(MockState*)self;
    for (int i=0;i<m->n;i++){
        int idx=(m->poll_cursor+i)%(m->n>0?m->n:1);
        MockEP *e=&m->ep[idx];
        if (e->used && e->is_sub && e->count>0){
            const MockBlob *blob=&e->fifo[e->head];
            e->head=(e->head+1)%MOCK_FIFO; e->count--;
            m->poll_cursor=(idx+1)%m->n;
            out->sub_handle=(uint32_t)idx; out->bytes=blob->buf; out->len=blob->len;
            return 1;
        }
    }
    return 0;
}
static int mock_call(void *self, uint32_t cli, const void *req, size_t rl,
                     void *resp, size_t cap, size_t *rlen){
    (void)self;(void)cli; size_t n=rl<cap?rl:cap; memcpy(resp,req,n); *rlen=n; return 0;
}
static void mock_fini(void *self){ (void)self; }

void uros_mock_init(URosTransport *tp){
    MockState *m=(MockState*)calloc(1,sizeof(MockState));
    tp->self=m; tp->init=mock_init; tp->create_pub=mock_pub; tp->create_sub=mock_sub;
    tp->create_client=mock_cli; tp->create_service=mock_srv;
    tp->publish=mock_publish; tp->poll=mock_poll; tp->call=mock_call; tp->fini=mock_fini;
}
void uros_mock_free(URosTransport *tp){ free(tp->self); tp->self=NULL; }
void uros_mock_inject(void *self, uint32_t sub, const void *bytes, size_t len){
    MockState *m=(MockState*)self;
    if (sub>=(uint32_t)m->n || len>MOCK_BLOB_CAP) return;
    MockEP *e=&m->ep[sub]; if (e->count>=MOCK_FIFO) return;
    memcpy(e->fifo[e->tail].buf,bytes,len); e->fifo[e->tail].len=len;
    e->tail=(e->tail+1)%MOCK_FIFO; e->count++;
}
int uros_mock_last_published(void *self, uint32_t pub, const void **out_bytes, size_t *out_len){
    MockState *m=(MockState*)self;
    if (pub>=(uint32_t)m->n) return 0;
    *out_bytes=m->ep[pub].last_pub.buf; *out_len=m->ep[pub].last_pub.len;
    return m->ep[pub].last_pub.len>0?1:0;
}
#else
/* Avoid ISO C "empty translation unit" (-Wpedantic) when this gated file is
 * compiled flag-free into build/host for the stdlib bake tool (TARGET != host). */
typedef int uros_translation_unit_not_empty;
#endif /* URBI_ENABLE_ROS2 */
