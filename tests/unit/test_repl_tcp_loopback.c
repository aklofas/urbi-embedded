/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_tcp_loopback.c — end-to-end TCP loopback drive.
 *
 * Phase 3 Task 16 verification.  Opens a real 127.0.0.1:<kernel-assigned>
 * TCP listener via UREPL_TCP_TRANSPORT, connects from the test thread,
 * exercises the full pipeline:
 *
 *   client connect → listener accepts → reader subthread spawned →
 *   server emits hello envelope → reader flushes hello to socket →
 *   test reads hello → test sends "eval" NDJSON → reader parses + pushes
 *   job → VM thread (driven manually via urbi_step in the test) drains
 *   queue + dispatches → response envelope lands in session output →
 *   reader flushes to socket → test reads result.
 *
 * The test is conservative on timing: 1 s wait windows are plenty for
 * loopback, and we sequence reads behind explicit ringbuf flushes via
 * urbi_step (which calls the drain hook). */
#include "utest.h"

#ifdef URBI_ENABLE_REPL

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "repl/urepl.h"
#include "repl/urepl_transport_tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define UTEST(name) static void name(void)

/* Connect to 127.0.0.1:port and return the connected fd, or -1. */
static int
connect_loopback(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Read up to `expect_bytes` bytes from fd with a 1 s timeout.  Returns
 * bytes actually read (may be < expect_bytes if the peer flushed less). */
static ssize_t
recv_with_timeout(int fd, char *buf, size_t cap, int timeout_ms)
{
    size_t total = 0;
    while (total < cap) {
        struct pollfd pf = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pf, 1, timeout_ms);
        if (pr <= 0) break;
        if (!(pf.revents & POLLIN)) break;
        ssize_t n = recv(fd, buf + total, cap - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        /* Stop at newline boundary — NDJSON line-delimited. */
        if (memchr(buf, '\n', total) != NULL) break;
    }
    return (ssize_t)total;
}

/* Drive urbi_step a handful of times so the dispatch-drain hook fires
 * and the listener's wake_eventfd is signaled.  Each call also yields
 * the kernel scheduler to give the reader thread a slot. */
static void
drive_vm_steps(UVM *vm, int iters)
{
    for (int i = 0; i < iters; ++i) {
        urbi_step(vm, 100, NULL);
        /* Tiny sleep so the reader subthread gets CPU. */
        struct timespec ts = { 0, 5 * 1000 * 1000 };  /* 5 ms */
        nanosleep(&ts, NULL);
    }
}

UTEST(tcp_listener_hello_envelope_round_trip)
{
    UVM *vm = (UVM *)calloc(1, sizeof(UVM));
    UASSERT_EQ(urbi_vm_init(vm, NULL, NULL), URBI_OK);

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = 0;   /* kernel-assigned */
    int err = 0;
    UReplServer *server = urbi_repl_serve(vm, &cfg, &err);
    UASSERT(server != NULL);

    UTcpListener *listener = urepl_tcp_listener_create("127.0.0.1", 0);
    UASSERT(listener != NULL);
    UASSERT(listener->port > 0);

    int rc = urbi_repl_register_transport(server,
                                          &UREPL_TCP_TRANSPORT, listener);
    UASSERT_EQ(rc, URBI_OK);

    /* Client connect.  Brief sleep so the listener pthread has a
     * chance to enter its accept loop (we connect after register_
     * transport schedules the thread). */
    struct timespec ts = { 0, 50 * 1000 * 1000 };  /* 50 ms */
    nanosleep(&ts, NULL);
    int client_fd = connect_loopback(listener->port);
    UASSERT(client_fd >= 0);

    /* Drive a few steps so the listener accepts + reader flushes the
     * hello envelope. */
    drive_vm_steps(vm, 20);

    char buf[1024] = {0};
    ssize_t n = recv_with_timeout(client_fd, buf, sizeof(buf) - 1, 1000);
    UASSERT(n > 0);
    buf[n] = '\0';
    /* Hello envelope must contain the kind, version, lobby, and
     * auth_required:false (no token configured). */
    UASSERT(strstr(buf, "\"kind\":\"hello\"") != NULL);
    UASSERT(strstr(buf, "\"version\":\"v0.9.1\"") != NULL);
    UASSERT(strstr(buf, "\"lobby\":") != NULL);
    UASSERT(strstr(buf, "\"auth_required\":false") != NULL);

    close(client_fd);

    /* Tear down — must join all threads cleanly. */
    urbi_repl_stop(server);
    urepl_tcp_listener_destroy(listener);
    urbi_vm_destroy(vm);
    free(vm);
}

UTEST(tcp_listener_eval_op_round_trip)
{
    UVM *vm = (UVM *)calloc(1, sizeof(UVM));
    UASSERT_EQ(urbi_vm_init(vm, NULL, NULL), URBI_OK);

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = 0;
    int err = 0;
    UReplServer *server = urbi_repl_serve(vm, &cfg, &err);
    UASSERT(server != NULL);

    UTcpListener *listener = urepl_tcp_listener_create("127.0.0.1", 0);
    UASSERT(listener != NULL);
    UASSERT_EQ(urbi_repl_register_transport(server, &UREPL_TCP_TRANSPORT,
                                            listener), URBI_OK);

    struct timespec ts50 = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts50, NULL);
    int client_fd = connect_loopback(listener->port);
    UASSERT(client_fd >= 0);

    /* Drain the hello envelope. */
    drive_vm_steps(vm, 20);
    char buf[2048] = {0};
    ssize_t n = recv_with_timeout(client_fd, buf, sizeof(buf) - 1, 1000);
    UASSERT(n > 0);

    /* Send an eval op: {"id":1,"op":"eval","code":"1+2"} */
    const char *req = "{\"id\":1,\"op\":\"eval\",\"code\":\"1+2\"}\n";
    ssize_t w = send(client_fd, req, strlen(req), 0);
    UASSERT_EQ(w, (ssize_t)strlen(req));

    /* Drive enough steps to: read+parse the NDJSON, push the job,
     * dispatch it (compiles + runs "1+2"), emit result+done envelopes,
     * flush them back to the socket. */
    drive_vm_steps(vm, 50);

    memset(buf, 0, sizeof(buf));
    n = 0;
    /* Multi-line read — both "result" and "done" envelopes. */
    for (int attempt = 0; attempt < 5 && n < (ssize_t)(sizeof(buf) - 16); ++attempt) {
        ssize_t r = recv_with_timeout(client_fd, buf + n,
                                      sizeof(buf) - 1 - (size_t)n, 500);
        if (r <= 0) break;
        n += r;
        if (strstr(buf, "\"kind\":\"done\"") != NULL) break;
        drive_vm_steps(vm, 10);
    }
    UASSERT(n > 0);
    UASSERT(strstr(buf, "\"kind\":\"result\"") != NULL);
    UASSERT(strstr(buf, "\"value\":\"3\"") != NULL);
    UASSERT(strstr(buf, "\"kind\":\"done\"") != NULL);
    UASSERT(strstr(buf, "\"id\":1") != NULL);

    close(client_fd);
    urbi_repl_stop(server);
    urepl_tcp_listener_destroy(listener);
    urbi_vm_destroy(vm);
    free(vm);
}

UTEST(tcp_listener_clean_shutdown_with_no_clients)
{
    /* Start + immediately stop a TCP listener.  Listener thread must
     * join cleanly within the test budget (no thread leak). */
    UVM *vm = (UVM *)calloc(1, sizeof(UVM));
    UASSERT_EQ(urbi_vm_init(vm, NULL, NULL), URBI_OK);

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = 0;
    int err = 0;
    UReplServer *server = urbi_repl_serve(vm, &cfg, &err);
    UASSERT(server != NULL);

    UTcpListener *listener = urepl_tcp_listener_create("127.0.0.1", 0);
    UASSERT(listener != NULL);
    UASSERT_EQ(urbi_repl_register_transport(server, &UREPL_TCP_TRANSPORT,
                                            listener), URBI_OK);

    /* No client connect; stop immediately. */
    urbi_repl_stop(server);
    urepl_tcp_listener_destroy(listener);
    urbi_vm_destroy(vm);
    free(vm);
}

UTEST(tcp_listener_client_disconnect_reaps_reader)
{
    /* Open a client, drain the hello, then close the client side.
     * Reader subthread should exit on EOF; urbi_repl_stop joins
     * cleanly without complaints. */
    UVM *vm = (UVM *)calloc(1, sizeof(UVM));
    UASSERT_EQ(urbi_vm_init(vm, NULL, NULL), URBI_OK);

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = 0;
    UReplServer *server = urbi_repl_serve(vm, &cfg, NULL);
    UASSERT(server != NULL);
    UTcpListener *l = urepl_tcp_listener_create("127.0.0.1", 0);
    UASSERT(l != NULL);
    UASSERT_EQ(urbi_repl_register_transport(server, &UREPL_TCP_TRANSPORT,
                                            l), URBI_OK);

    struct timespec ts50 = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts50, NULL);
    int c = connect_loopback(l->port);
    UASSERT(c >= 0);
    drive_vm_steps(vm, 10);
    char buf[256];
    (void)recv_with_timeout(c, buf, sizeof(buf), 1000);
    close(c);

    /* Give the reader thread a beat to notice EOF. */
    struct timespec ts100 = { 0, 100 * 1000 * 1000 };
    nanosleep(&ts100, NULL);

    urbi_repl_stop(server);
    urepl_tcp_listener_destroy(l);
    urbi_vm_destroy(vm);
    free(vm);
}

void
test_repl_tcp_loopback_suite(void)
{
    printf("test_repl_tcp_loopback\n");
    utest_run("tcp_listener_hello_envelope_round_trip",
              tcp_listener_hello_envelope_round_trip);
    utest_run("tcp_listener_eval_op_round_trip",
              tcp_listener_eval_op_round_trip);
    utest_run("tcp_listener_clean_shutdown_with_no_clients",
              tcp_listener_clean_shutdown_with_no_clients);
    utest_run("tcp_listener_client_disconnect_reaps_reader",
              tcp_listener_client_disconnect_reaps_reader);
}

#else  /* !URBI_ENABLE_REPL */

void test_repl_tcp_loopback_suite(void) { /* skipped: URBI_ENABLE_REPL=0 */ }

#endif
