/* SPDX-License-Identifier: BSD-3-Clause */
#include <string.h>
#include "smoke_transport.h"

#define BUF_BYTES 2048u

static char   in_buf[BUF_BYTES], out_buf[BUF_BYTES];
static size_t in_len, in_pos, out_len, out_pos;

static int t_read(void *ctx, void *buf, size_t n)
{
    (void)ctx;
    size_t k = in_len - in_pos;
    if (k > n) k = n;
    memcpy(buf, in_buf + in_pos, k);
    in_pos += k;
    return (int)k;
}
static int t_write(void *ctx, const void *buf, size_t n)
{
    (void)ctx;
    size_t k = BUF_BYTES - out_len;
    if (k > n) k = n;
    memcpy(out_buf + out_len, buf, k);
    out_len += k;
    return (int)k;
}
static void t_close(void *ctx) { (void)ctx; }

void smoke_transport_vtable(UTransport *out)
{
    out->ctx = NULL; out->read = t_read; out->write = t_write; out->close = t_close;
}
void smoke_transport_feed(const char *s, size_t n)
{
    if (n > BUF_BYTES - in_len) n = BUF_BYTES - in_len;
    memcpy(in_buf + in_len, s, n);
    in_len += n;
}
size_t smoke_transport_take(char *buf, size_t cap)
{
    size_t k = out_len - out_pos;
    if (k > cap) k = cap;
    memcpy(buf, out_buf + out_pos, k);
    out_pos += k;
    return k;
}
