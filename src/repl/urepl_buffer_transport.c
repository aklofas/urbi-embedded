/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_buffer_transport.c — see repl/urepl_buffer_transport.h. */

#include "repl/urepl_buffer_transport.h"

#include "urbi/urbi.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* One direction of the loopback: a growable byte queue, consumed from the
 * front and appended at the back.  Growth is by doubling and the front is
 * reclaimed on the next append, which keeps a long session from growing
 * without bound while never memmoving in the common drain-it-all case. */
typedef struct {
    char  *buf;
    size_t cap, fill, off;
} Queue;

struct UBufferTransport {
    Queue to_service;    /* client writes here, the service reads */
    Queue to_client;     /* the service writes here, the client reads */
    bool  client_done;   /* no more requests are coming */
    bool  closed;        /* the service has closed its end */
};

static bool queue_push(Queue *q, const char *bytes, size_t n)
{
    if (q->off > 0 && q->fill + n > q->cap) {
        memmove(q->buf, q->buf + q->off, q->fill - q->off);
        q->fill -= q->off;
        q->off = 0;
    }
    if (q->fill + n > q->cap) {
        size_t cap = q->cap ? q->cap : 256u;
        while (cap < q->fill + n) cap *= 2u;
        char *nb = (char *)realloc(q->buf, cap);
        if (nb == NULL) return false;
        q->buf = nb;
        q->cap = cap;
    }
    memcpy(q->buf + q->fill, bytes, n);
    q->fill += n;
    return true;
}

static size_t queue_take(Queue *q, char *dst, size_t cap)
{
    size_t have = q->fill - q->off;
    size_t n = have < cap ? have : cap;
    if (n > 0) {
        memcpy(dst, q->buf + q->off, n);
        q->off += n;
    }
    if (q->off >= q->fill) { q->off = 0; q->fill = 0; }
    return n;
}

/* ---- the vtable --------------------------------------------------------- */

static int bt_read(void *ctx, void *buf, size_t n)
{
    UBufferTransport *bt = (UBufferTransport *)ctx;
    size_t got = queue_take(&bt->to_service, (char *)buf, n);
    if (got > 0) return (int)got;
    /* Empty.  That is "nothing yet" while the client may still write, and
     * end of stream once it has said it will not. */
    return bt->client_done ? URBI_ERR_INVALID_STATE : 0;
}

static int bt_write(void *ctx, const void *buf, size_t n)
{
    UBufferTransport *bt = (UBufferTransport *)ctx;
    if (!queue_push(&bt->to_client, (const char *)buf, n)) return URBI_ERR_OOM;
    return (int)n;
}

static void bt_close(void *ctx)
{
    UBufferTransport *bt = (UBufferTransport *)ctx;
    bt->closed = true;
}

void urepl_buffer_transport_vtable(UBufferTransport *bt, UTransport *out)
{
    if (out == NULL) return;
    out->ctx = bt;
    out->read = bt_read;
    out->write = bt_write;
    out->close = bt_close;
}

/* ---- lifecycle and the client side -------------------------------------- */

UBufferTransport *urepl_buffer_transport_create(void)
{
    return (UBufferTransport *)calloc(1, sizeof(UBufferTransport));
}

void urepl_buffer_transport_destroy(UBufferTransport *bt)
{
    if (bt == NULL) return;
    free(bt->to_service.buf);
    free(bt->to_client.buf);
    free(bt);
}

size_t urepl_buffer_client_write(UBufferTransport *bt, const void *bytes, size_t n)
{
    if (bt == NULL || bytes == NULL) return 0;
    return queue_push(&bt->to_service, (const char *)bytes, n) ? n : 0;
}

size_t urepl_buffer_client_read(UBufferTransport *bt, void *buf, size_t cap)
{
    if (bt == NULL || buf == NULL) return 0;
    return queue_take(&bt->to_client, (char *)buf, cap);
}

void urepl_buffer_client_finish(UBufferTransport *bt)
{
    if (bt != NULL) bt->client_done = true;
}
