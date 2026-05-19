/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_buffer_transport.c — in-process loopback transport. */
#include "utest.h"

#ifdef URBI_ENABLE_REPL

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "repl/urepl_buffer_transport.h"
#include "repl/urepl.h"

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

UTEST(buffer_transport_accept_once_then_blocks)
{
    UBufferTransportState *st = urepl_buffer_transport_create();
    UASSERT(st != NULL);
    int fd = -1;
    int rc = UREPL_BUFFER_TRANSPORT.accept_fn(st, &fd);
    UASSERT_EQ(rc, 0);
    UASSERT_EQ(fd, 0);
    /* Second accept must signal "no client / would block". */
    rc = UREPL_BUFFER_TRANSPORT.accept_fn(st, &fd);
    UASSERT(rc != 0);
    urepl_buffer_transport_destroy(st);
}

UTEST(buffer_transport_reset_accept_lets_us_reaccept)
{
    UBufferTransportState *st = urepl_buffer_transport_create();
    int fd = -1;
    UASSERT_EQ(UREPL_BUFFER_TRANSPORT.accept_fn(st, &fd), 0);
    urepl_buffer_transport_reset_accept(st);
    UASSERT_EQ(UREPL_BUFFER_TRANSPORT.accept_fn(st, &fd), 0);
    urepl_buffer_transport_destroy(st);
}

UTEST(buffer_transport_client_to_server_roundtrip)
{
    UBufferTransportState *st = urepl_buffer_transport_create();
    int fd = -1;
    UASSERT_EQ(UREPL_BUFFER_TRANSPORT.accept_fn(st, &fd), 0);

    /* Client writes bytes; server reads them. */
    size_t n = urepl_buffer_client_write(st, "hello", 5);
    UASSERT_EQ(n, 5);
    char buf[16];
    int r = UREPL_BUFFER_TRANSPORT.read_fn(fd, buf, sizeof(buf));
    UASSERT_EQ(r, 5);
    UASSERT(memcmp(buf, "hello", 5) == 0);

    urepl_buffer_transport_destroy(st);
}

UTEST(buffer_transport_server_to_client_roundtrip)
{
    UBufferTransportState *st = urepl_buffer_transport_create();
    int fd = -1;
    UASSERT_EQ(UREPL_BUFFER_TRANSPORT.accept_fn(st, &fd), 0);

    /* Server writes; client reads. */
    int w = UREPL_BUFFER_TRANSPORT.write_fn(fd, "world", 5);
    UASSERT_EQ(w, 5);
    char buf[16];
    size_t n = urepl_buffer_client_read(st, buf, sizeof(buf));
    UASSERT_EQ(n, 5);
    UASSERT(memcmp(buf, "world", 5) == 0);

    urepl_buffer_transport_destroy(st);
}

UTEST(buffer_transport_bidirectional)
{
    UBufferTransportState *st = urepl_buffer_transport_create();
    int fd = -1;
    UASSERT_EQ(UREPL_BUFFER_TRANSPORT.accept_fn(st, &fd), 0);

    urepl_buffer_client_write(st, "ping", 4);
    UREPL_BUFFER_TRANSPORT.write_fn(fd, "pong", 4);

    char buf[16];
    int r = UREPL_BUFFER_TRANSPORT.read_fn(fd, buf, sizeof(buf));
    UASSERT_EQ(r, 4);
    UASSERT(memcmp(buf, "ping", 4) == 0);

    size_t n = urepl_buffer_client_read(st, buf, sizeof(buf));
    UASSERT_EQ(n, 4);
    UASSERT(memcmp(buf, "pong", 4) == 0);

    urepl_buffer_transport_destroy(st);
}

UTEST(buffer_transport_register_with_server)
{
    /* Verify the vtable can be registered against a server (Phase 3
     * will use this; in v0.9.1 Phase 2 we just confirm the API
     * surface is wired up). */
    UVM *vm = (UVM *)calloc(1, sizeof(UVM));
    urbi_vm_init(vm, NULL, NULL);
    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port = -1;
    int err = 0;
    UReplServer *server = urbi_repl_serve(vm, &cfg, &err);
    UASSERT(server != NULL);

    UBufferTransportState *st = urepl_buffer_transport_create();
    UASSERT(st != NULL);
    int rc = urbi_repl_register_transport(server, &UREPL_BUFFER_TRANSPORT, st);
    UASSERT_EQ(rc, URBI_OK);

    urepl_buffer_transport_destroy(st);
    urbi_repl_stop(server);
    urbi_vm_destroy(vm);
    free(vm);
}

UTEST(buffer_transport_read_empty_returns_zero)
{
    UBufferTransportState *st = urepl_buffer_transport_create();
    int fd = -1;
    UASSERT_EQ(UREPL_BUFFER_TRANSPORT.accept_fn(st, &fd), 0);
    char buf[8];
    int r = UREPL_BUFFER_TRANSPORT.read_fn(fd, buf, sizeof(buf));
    UASSERT_EQ(r, 0);
    urepl_buffer_transport_destroy(st);
}

void
test_repl_buffer_transport_suite(void)
{
    printf("test_repl_buffer_transport\n");
    utest_run("buffer_transport_accept_once_then_blocks",
              buffer_transport_accept_once_then_blocks);
    utest_run("buffer_transport_reset_accept_lets_us_reaccept",
              buffer_transport_reset_accept_lets_us_reaccept);
    utest_run("buffer_transport_client_to_server_roundtrip",
              buffer_transport_client_to_server_roundtrip);
    utest_run("buffer_transport_server_to_client_roundtrip",
              buffer_transport_server_to_client_roundtrip);
    utest_run("buffer_transport_bidirectional",
              buffer_transport_bidirectional);
    utest_run("buffer_transport_register_with_server",
              buffer_transport_register_with_server);
    utest_run("buffer_transport_read_empty_returns_zero",
              buffer_transport_read_empty_returns_zero);
}

#else  /* !URBI_ENABLE_REPL */

void test_repl_buffer_transport_suite(void) { /* skipped: URBI_ENABLE_REPL=0 */ }

#endif
