/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_uart_pty.c — pty-pair UART harness end-to-end.
 *
 * Phase 7 Task 30 verification.  Mirrors the TCP loopback drive in
 * test_repl_tcp_loopback.c but over an openpty()-allocated (master,
 * slave) fd pair.  The slave fd plays the role of a "client already
 * connected" (single-client UART semantics — accept returns it once,
 * -1 forever after); the master fd is what the test thread drives,
 * writing NDJSON requests and reading response envelopes.
 *
 * The CI value of this is exercising the UART transport pathway with
 * no real hardware: the same listener thread + reader subthread + drain
 * hook that runs on Pico/ESP32/Linux-UART deployments runs against the
 * pty here. */
#include "utest.h"

#ifdef URBI_ENABLE_REPL

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "repl/urepl.h"
#include "repl/urepl_transport_pty.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UTEST(name) static void name(void)

/* Drive urbi_step a handful of times so the dispatch-drain hook fires
 * and the listener's wake_eventfd is signaled.  Each call yields the
 * kernel scheduler to give the reader thread a slot. */
static void
drive_vm_steps(UVM *vm, int iters)
{
    for (int i = 0; i < iters; ++i) {
        urbi_step(vm, 100, NULL);
        struct timespec ts = { 0, 5 * 1000 * 1000 };  /* 5 ms */
        nanosleep(&ts, NULL);
    }
}

/* Read up to cap bytes from fd with a poll timeout.  Stops at first
 * newline (NDJSON line-delimited).  Returns bytes read. */
static ssize_t
read_with_timeout(int fd, char *buf, size_t cap, int timeout_ms)
{
    size_t total = 0;
    while (total < cap) {
        struct pollfd pf = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pf, 1, timeout_ms);
        if (pr <= 0) break;
        if (!(pf.revents & POLLIN)) break;
        ssize_t n = read(fd, buf + total, cap - total);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) break;
        total += (size_t)n;
        if (memchr(buf, '\n', total) != NULL) break;
    }
    return (ssize_t)total;
}

UTEST(pty_transport_create_destroy)
{
    UPtyState *st = urepl_pty_state_create();
    UASSERT(st != NULL);
    UASSERT(urepl_pty_master_fd(st) >= 0);
    UASSERT(urepl_pty_slave_fd(st) >= 0);
    UASSERT(urepl_pty_master_fd(st) != urepl_pty_slave_fd(st));
    urepl_pty_state_destroy(st);
}

UTEST(pty_transport_accept_once_then_blocks)
{
    UPtyState *st = urepl_pty_state_create();
    UASSERT(st != NULL);
    int fd = -1;
    int rc = UREPL_PTY_TRANSPORT.accept_fn(st, &fd);
    UASSERT_EQ(rc, 0);
    UASSERT_EQ(fd, urepl_pty_slave_fd(st));
    /* Second accept must signal "no more clients" (single-client UART). */
    rc = UREPL_PTY_TRANSPORT.accept_fn(st, &fd);
    UASSERT(rc != 0);
    urepl_pty_state_destroy(st);
}

UTEST(pty_listener_hello_envelope_round_trip)
{
    UVM *vm = (UVM *)calloc(1, sizeof(UVM));
    UASSERT_EQ(urbi_vm_init(vm, NULL, NULL), URBI_OK);

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = -1;   /* TCP disabled — pty-only test */
    int err = 0;
    UReplServer *server = urbi_repl_serve(vm, &cfg, &err);
    UASSERT(server != NULL);

    UPtyState *pty = urepl_pty_state_create();
    UASSERT(pty != NULL);
    int rc = urbi_repl_register_transport(server, &UREPL_PTY_TRANSPORT, pty);
    UASSERT_EQ(rc, URBI_OK);

    /* Give the listener thread a moment to enter its accept loop. */
    struct timespec ts50 = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts50, NULL);

    /* Drive a few steps so the listener accepts + the reader flushes
     * the hello envelope to the slave fd, which the master sees. */
    drive_vm_steps(vm, 20);

    char buf[1024] = {0};
    ssize_t n = read_with_timeout(urepl_pty_master_fd(pty),
                                  buf, sizeof(buf) - 1, 1000);
    UASSERT(n > 0);
    buf[n] = '\0';
    UASSERT(strstr(buf, "\"kind\":\"hello\"") != NULL);
    UASSERT(strstr(buf, "\"version\":\"v0.9.1\"") != NULL);
    UASSERT(strstr(buf, "\"lobby\":") != NULL);
    UASSERT(strstr(buf, "\"auth_required\":false") != NULL);

    urbi_repl_stop(server);
    urepl_pty_state_destroy(pty);
    urbi_vm_destroy(vm);
    free(vm);
}

UTEST(pty_listener_eval_op_round_trip)
{
    UVM *vm = (UVM *)calloc(1, sizeof(UVM));
    UASSERT_EQ(urbi_vm_init(vm, NULL, NULL), URBI_OK);

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = -1;
    UReplServer *server = urbi_repl_serve(vm, &cfg, NULL);
    UASSERT(server != NULL);

    UPtyState *pty = urepl_pty_state_create();
    UASSERT(pty != NULL);
    UASSERT_EQ(urbi_repl_register_transport(server,
                                            &UREPL_PTY_TRANSPORT, pty),
               URBI_OK);

    struct timespec ts50 = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts50, NULL);

    /* Drain the hello envelope first. */
    drive_vm_steps(vm, 20);
    char buf[2048] = {0};
    int master = urepl_pty_master_fd(pty);
    ssize_t n = read_with_timeout(master, buf, sizeof(buf) - 1, 1000);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "\"kind\":\"hello\"") != NULL);

    /* Write an eval request on the master side. */
    const char *req = "{\"id\":1,\"op\":\"eval\",\"code\":\"1+2\"}\n";
    ssize_t w = write(master, req, strlen(req));
    UASSERT_EQ(w, (ssize_t)strlen(req));

    drive_vm_steps(vm, 50);

    memset(buf, 0, sizeof(buf));
    n = 0;
    /* Multi-line read — both "result" and "done" envelopes. */
    for (int attempt = 0;
         attempt < 5 && n < (ssize_t)(sizeof(buf) - 16);
         ++attempt) {
        ssize_t r = read_with_timeout(master, buf + n,
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

    urbi_repl_stop(server);
    urepl_pty_state_destroy(pty);
    urbi_vm_destroy(vm);
    free(vm);
}

void
test_repl_uart_pty_suite(void)
{
    printf("test_repl_uart_pty\n");
    utest_run("pty_transport_create_destroy",
              pty_transport_create_destroy);
    utest_run("pty_transport_accept_once_then_blocks",
              pty_transport_accept_once_then_blocks);
    utest_run("pty_listener_hello_envelope_round_trip",
              pty_listener_hello_envelope_round_trip);
    utest_run("pty_listener_eval_op_round_trip",
              pty_listener_eval_op_round_trip);
}

#else  /* !URBI_ENABLE_REPL */

void test_repl_uart_pty_suite(void) { /* skipped: URBI_ENABLE_REPL=0 */ }

#endif
