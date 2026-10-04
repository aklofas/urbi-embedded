/* SPDX-License-Identifier: BSD-3-Clause */
/* The session output buffer: allocated on first write, grown as output
 * arrives, capped by the server config, and a write the cap cannot hold
 * is reported dropped rather than truncated or written past the end. */
#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/repl.h"
#include "repl/urepl.h"
#include "repl/urepl_buffer_transport.h"
#include <stdio.h>
#include <string.h>

typedef struct { UVM *vm; UReplServer *srv; UBufferTransport *bt; } OutFix;

static void outfix_open(OutFix *fx, size_t cap)
{
    fx->vm = urbi_open(utest_alloc, NULL, NULL);
    UReplConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.output_buf_cap = cap;
    UASSERT_EQ(urbi_repl_serve_init(fx->vm, &cfg, &fx->srv), URBI_OK);
    fx->bt = urepl_buffer_transport_create();
    UTransport t; urepl_buffer_transport_vtable(fx->bt, &t);
    UASSERT_EQ(urbi_repl_register_transport(fx->srv, &t), URBI_OK);
}
static void outfix_close(OutFix *fx)
{
    urbi_repl_serve_shutdown(fx->srv);
    urepl_buffer_transport_destroy(fx->bt);
    urbi_close(fx->vm);
}
/* Feed one request line and sweep until the service has nothing more to
 * say; returns everything the client read. */
static size_t roundtrip(OutFix *fx, const char *line, char *out, size_t cap)
{
    urepl_buffer_client_write(fx->bt, line, strlen(line));
    size_t total = 0;
    for (int i = 0; i < 8; i++) {
        urbi_repl_serve_step(fx->srv, 0);
        (void)urbi_step(fx->vm, 0, NULL);
        urbi_repl_serve_step(fx->srv, 0);
        size_t n = urepl_buffer_client_read(fx->bt, out + total, cap - 1 - total);
        total += n;
    }
    out[total] = '\0';
    return total;
}

static void the_buffer_is_not_allocated_before_the_first_write(void)
{
    OutFix fx; outfix_open(&fx, 4096);
    UASSERT(fx.srv->sessions != NULL);
    UASSERT(fx.srv->sessions->out.buf == NULL);
    UASSERT_EQ(fx.srv->sessions->out.cap, (size_t)0);
    outfix_close(&fx);
}
static void a_small_result_arrives_through_a_small_cap(void)
{
    OutFix fx; outfix_open(&fx, 512);
    char out[2048];
    roundtrip(&fx, "{\"id\":1,\"op\":\"eval\",\"code\":\"1+2\"}\n", out, sizeof out);
    UASSERT(strstr(out, "\"value\":\"3\"") != NULL);
    UASSERT(fx.srv->sessions->out.cap <= 512);
    outfix_close(&fx);
}
static void a_first_write_larger_than_the_cap_is_reported_dropped(void)
{
    /* 128 bytes holds the dropped report and the `done`, but not the
     * result envelope of a 200-character string. */
    OutFix fx; outfix_open(&fx, 128);
    char line[512];
    char str[201];
    memset(str, 'a', 200);
    str[200] = '\0';
    (void)snprintf(line, sizeof line,
                   "{\"id\":1,\"op\":\"eval\",\"code\":\"\\\"%s\\\"\"}\n", str);
    char out[4096];
    roundtrip(&fx, line, out, sizeof out);
    UASSERT(strstr(out, "\"code\":\"output_dropped\"") != NULL);
    UASSERT(strstr(out, str) == NULL);
    UASSERT(strstr(out, "\"kind\":\"done\"") != NULL);
    UASSERT(fx.srv->sessions->out.cap <= 128);
    outfix_close(&fx);
}
void test_repl_outbuf_suite(void)
{
    utest_run("the_buffer_is_not_allocated_before_the_first_write", the_buffer_is_not_allocated_before_the_first_write);
    utest_run("a_small_result_arrives_through_a_small_cap", a_small_result_arrives_through_a_small_cap);
    utest_run("a_first_write_larger_than_the_cap_is_reported_dropped", a_first_write_larger_than_the_cap_is_reported_dropped);
}
