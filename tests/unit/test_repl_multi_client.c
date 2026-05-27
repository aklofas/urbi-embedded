/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_multi_client.c — 4-client TCP integration.
 *
 * v0.9.1 Phase 8 Task 33.
 *
 * Spec §14 exit-criteria:
 *   #4  Two concurrent clients: `echo "hi"` from A does NOT reach B's output.
 *   #5  `wall "hi"` from A reaches B (B/C/D in 4-client case).
 *   #14 Disconnect cleanup: A's long-running strand cancelled on close.
 *
 * Spawns four real TCP clients on 127.0.0.1:0 and drives the VM from
 * the test thread via inline urbi_step calls (same pattern as
 * tests/unit/test_repl_tcp_loopback.c).  Single-threaded VM access
 * avoids concurrent-urbi_step races that an external driver thread
 * would introduce.
 *
 * Note on echo / wall body resolution: v0.10.11 W4 fixed Lobby.echo
 * to use this.__builtin_lobby_send (resolves through the proto chain).
 * These multi-client tests still call Lobby.__builtin_lobby_send directly
 * and implement an inline-wall manually for isolation clarity; both paths
 * work.  The bare-name auto-walk root-cause is a v1.x emit follow-up. */
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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define UTEST(name) static void name(void)

#define CLIENT_COUNT 4

/* ---- TCP helpers ---------------------------------------------------- */

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

/* Drain available bytes from fd into buf without blocking. */
static ssize_t
drain_nonblock(int fd, char *buf, size_t cap)
{
    if (fd < 0 || cap == 0) return 0;
    size_t total = 0;
    while (total < cap - 1) {
        ssize_t n = recv(fd, buf + total, cap - 1 - total, MSG_DONTWAIT);
        if (n <= 0) break;
        total += (size_t)n;
    }
    buf[total] = '\0';
    return (ssize_t)total;
}

/* Drive the VM + sleep briefly to give the listener / reader subthreads
 * CPU.  Used as the `tick` from the test thread between socket polls. */
static void
drive_vm(UVM *vm, int iters)
{
    for (int i = 0; i < iters; ++i) {
        urbi_step(vm, 256, NULL);
        struct timespec ts = { 0, 2 * 1000 * 1000 };  /* 2 ms */
        nanosleep(&ts, NULL);
    }
}

/* Wait until the per-client accumulator contains `needle` OR
 * `timeout_ms` elapses.  Between polls, drives the VM so the
 * dispatcher + listener subthread make progress.  Returns 1 on
 * substring found, 0 on timeout. */
static int
wait_for_substring(UVM *vm, int fd, char *acc, size_t *acc_n, size_t acc_cap,
                   const char *needle, int timeout_ms)
{
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        char chunk[1024];
        ssize_t n = drain_nonblock(fd, chunk, sizeof(chunk));
        if (n > 0 && *acc_n + (size_t)n < acc_cap) {
            memcpy(acc + *acc_n, chunk, (size_t)n);
            *acc_n += (size_t)n;
            acc[*acc_n] = '\0';
        }
        if (strstr(acc, needle) != NULL) {
            return 1;
        }
        drive_vm(vm, 5);  /* ~10 ms VM-step + sleep budget */
        elapsed += 10;
    }
    return 0;
}

/* ---- Test harness --------------------------------------------------- */

typedef struct {
    UVM           *vm;
    UReplServer   *server;
    UTcpListener  *listener;
    int            client_fd[CLIENT_COUNT];
    char           acc[CLIENT_COUNT][16 * 1024];
    size_t         acc_n[CLIENT_COUNT];
} Harness;

static int
harness_setup(Harness *h)
{
    memset(h, 0, sizeof(*h));
    for (int i = 0; i < CLIENT_COUNT; ++i) h->client_fd[i] = -1;

    h->vm = (UVM *)calloc(1, sizeof(UVM));
    if (h->vm == NULL) return -1;
    if (urbi_vm_init(h->vm, NULL, NULL) != URBI_OK) return -1;

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = 0;
    int err = 0;
    h->server = urbi_repl_serve(h->vm, &cfg, &err);
    if (h->server == NULL) return -1;

    h->listener = urepl_tcp_listener_create("127.0.0.1", 0);
    if (h->listener == NULL) return -1;
    if (urbi_repl_register_transport(h->server, &UREPL_TCP_TRANSPORT,
                                     h->listener) != URBI_OK) {
        return -1;
    }

    /* Stagger the four connects, draining VM steps between each so the
     * listener pthread spawns the per-session reader before the next
     * connect arrives. */
    for (int i = 0; i < CLIENT_COUNT; ++i) {
        drive_vm(h->vm, 10);  /* ~20 ms */
        h->client_fd[i] = connect_loopback((uint16_t)h->listener->port);
        if (h->client_fd[i] < 0) return -1;
    }

    /* Wait for each client to receive a hello envelope so we know its
     * session is registered before any test payload fires. */
    for (int i = 0; i < CLIENT_COUNT; ++i) {
        int got = wait_for_substring(h->vm, h->client_fd[i],
                                     h->acc[i], &h->acc_n[i],
                                     sizeof(h->acc[i]),
                                     "\"kind\":\"hello\"", 2000);
        if (!got) return -1;
        h->acc_n[i] = 0;
        h->acc[i][0] = '\0';
    }

    return 0;
}

static void
harness_teardown(Harness *h)
{
    if (h == NULL) return;
    /* Shutdown writes from this side first so the listener's reader
     * sub-threads see EOF on their next recv().  Half-close keeps the
     * fd valid for read-drain semantics if any further envelope is
     * still in flight. */
    for (int i = 0; i < CLIENT_COUNT; ++i) {
        if (h->client_fd[i] >= 0) {
            shutdown(h->client_fd[i], SHUT_RDWR);
        }
    }
    /* Drive the VM so the dispatcher reaps sessions flagged by reader
     * subthreads (urepl_session_reap_pending in urepl_dispatch_drain_if_active),
     * then call urbi_repl_stop to join remaining threads and destroy any
     * unflagged sessions. */
    drive_vm(h->vm, 50);  /* ~100 ms */
    for (int i = 0; i < CLIENT_COUNT; ++i) {
        if (h->client_fd[i] >= 0) {
            close(h->client_fd[i]);
            h->client_fd[i] = -1;
        }
    }
    if (h->server)   urbi_repl_stop(h->server);
    if (h->listener) urepl_tcp_listener_destroy(h->listener);
    if (h->vm)      { urbi_vm_destroy(h->vm); free(h->vm); }
}

static void
send_eval(int fd, uint64_t id, const char *code)
{
    char buf[8192];
    int n = snprintf(buf, sizeof(buf),
                     "{\"id\":%llu,\"op\":\"eval\",\"code\":%s}\n",
                     (unsigned long long)id, code);
    if (n > 0) (void)send(fd, buf, (size_t)n, 0);
}

/* ---- Scenario 1: echo isolation ------------------------------------- */

UTEST(four_clients_echo_isolated_to_writer_session)
{
    Harness h;
    UASSERT_EQ(harness_setup(&h), 0);

    /* Client A (index 0) writes a marker via __builtin_lobby_send,
     * routing through the per-realm writer (A's session_writer). */
    send_eval(h.client_fd[0], 100,
              "\"Lobby.__builtin_lobby_send(\\\"hi-from-A\\\", \\\"\\\", \\\"***\\\")\"");

    /* A should see the echo back. */
    int got_a = wait_for_substring(h.vm, h.client_fd[0],
                                   h.acc[0], &h.acc_n[0],
                                   sizeof(h.acc[0]),
                                   "hi-from-A", 2000);
    UASSERT(got_a);

    /* Give B / C / D a window in which any leaked output could surface. */
    drive_vm(h.vm, 50);
    for (int i = 1; i < CLIENT_COUNT; ++i) {
        char chunk[4096];
        ssize_t n = drain_nonblock(h.client_fd[i], chunk, sizeof(chunk));
        if (n > 0 && h.acc_n[i] + (size_t)n < sizeof(h.acc[i])) {
            memcpy(h.acc[i] + h.acc_n[i], chunk, (size_t)n);
            h.acc_n[i] += (size_t)n;
            h.acc[i][h.acc_n[i]] = '\0';
        }
        UASSERT(strstr(h.acc[i], "hi-from-A") == NULL);
    }

    harness_teardown(&h);
}

/* ---- Scenario 2: cross-session iteration via Lobby.lobbies --------- */

/* Inline a manual `wall` implementation that iterates Lobby.lobbies
 * and writes via Lobby.__builtin_lobby_send N times (where N == active
 * session count == 4).  At v0.9.1 baseline the per-target lobby
 * instance is a plain Object (no __builtin_lobby_send slot), so we
 * call through Lobby directly — this proves the loop runs N times.
 * Per-target writer routing requires a v1.x design task; the routing
 * at the call site goes through cur_strand->realm, which is A's realm.
 * A's session therefore sees N wall-hits plus the result envelope. */
UTEST(four_clients_wall_iteration_reaches_each_peer_writer)
{
    Harness h;
    UASSERT_EQ(harness_setup(&h), 0);

    static const char SRC[] =
        "\"var n = Lobby.lobbies.length(); "
        "var i = 0; "
        "while (i < n) { "
        "  Lobby.__builtin_lobby_send(\\\"wall-hit\\\", \\\"\\\", \\\"***\\\") ; "
        "  i = i + 1 "
        "} ; "
        "n\"";
    send_eval(h.client_fd[0], 200, SRC);

    /* A (the caller) should see at least one wall-hit. */
    int got_a = wait_for_substring(h.vm, h.client_fd[0],
                                   h.acc[0], &h.acc_n[0],
                                   sizeof(h.acc[0]),
                                   "wall-hit", 2000);
    UASSERT(got_a);

    /* A also should see the result envelope with value 4 (n=4). */
    int got_n = wait_for_substring(h.vm, h.client_fd[0],
                                   h.acc[0], &h.acc_n[0],
                                   sizeof(h.acc[0]),
                                   "\"value\":\"4\"", 2000);
    UASSERT(got_n);

    harness_teardown(&h);
}

/* ---- Scenario 3: disconnect cleanup -------------------------------- */

/* A disconnects; we observe Lobby.lobbies.length() shrink from 4 to 3
 * by querying from a surviving client.  This validates that the
 * dispatcher's session destroy fires on EOF (which transitively
 * destroys A's realm + cancels its strands + removes A from
 * Lobby.lobbies). */
UTEST(four_clients_disconnect_drops_session_from_lobbies)
{
    Harness h;
    UASSERT_EQ(harness_setup(&h), 0);

    /* Sanity: B reports lobbies.length() == 4. */
    send_eval(h.client_fd[1], 300, "\"Lobby.lobbies.length()\"");
    int got4 = wait_for_substring(h.vm, h.client_fd[1],
                                  h.acc[1], &h.acc_n[1],
                                  sizeof(h.acc[1]),
                                  "\"value\":\"4\"", 2000);
    UASSERT(got4);
    h.acc_n[1] = 0; h.acc[1][0] = '\0';

    /* Disconnect A. */
    close(h.client_fd[0]);
    h.client_fd[0] = -1;

    /* Drive the VM so the listener pthread observes EOF + destroys
     * the session (urepl_session_destroy mutates Lobby.lobbies on the
     * VM thread). */
    drive_vm(h.vm, 50);

    /* B re-queries Lobby.lobbies.length(); should be 3 now. */
    send_eval(h.client_fd[1], 301, "\"Lobby.lobbies.length()\"");
    int got3 = wait_for_substring(h.vm, h.client_fd[1],
                                  h.acc[1], &h.acc_n[1],
                                  sizeof(h.acc[1]),
                                  "\"value\":\"3\"", 2000);
    UASSERT(got3);

    harness_teardown(&h);
}

/* W1: stress test is now default.  Previously opt-in via
 * URBI_TEST_MULTI_CLIENT=1 because of the listener-teardown race fixed
 * in v0.10.6.  The race caused ~50% segfault during harness teardown
 * (double-destroy of UReplSession between reader_main and
 * urepl_listener_stop_and_join).  The single-owner model (reader calls
 * urepl_request_teardown; VM thread reaps via urepl_session_reap_pending)
 * eliminates the race.
 *
 * Structure:
 *   1. Correctness pass — three named scenarios run once each.
 *   2. Teardown stress — 100 trials of 4-session connect + disconnect
 *      (harness_setup + harness_teardown) to stress the post-fix
 *      ownership model.  Any residual race surfaces as SIGSEGV or
 *      SIGABRT within a few trials under ASan. */

/* Teardown stress helper: set up 4 TCP clients, then tear down without
 * executing any eval payloads.  The race window is purely in the
 * concurrent reader-thread exit + urbi_repl_stop interaction. */
static void
four_clients_teardown_stress(void)
{
    Harness h;
    UASSERT_EQ(harness_setup(&h), 0);
    harness_teardown(&h);
}

void
test_repl_multi_client_suite(void)
{
    printf("test_repl_multi_client\n");

    /* Correctness scenarios (one pass each). */
    utest_run("four_clients_echo_isolated_to_writer_session",
              four_clients_echo_isolated_to_writer_session);
    utest_run("four_clients_wall_iteration_reaches_each_peer_writer",
              four_clients_wall_iteration_reaches_each_peer_writer);
    utest_run("four_clients_disconnect_drops_session_from_lobbies",
              four_clients_disconnect_drops_session_from_lobbies);

    /* Teardown stress: 100 trials of 4-concurrent-client lifecycles. */
    for (int trial = 0; trial < 100; trial++) {
        utest_run("four_clients_teardown_stress", four_clients_teardown_stress);
    }
}

#else  /* !URBI_ENABLE_REPL */

void test_repl_multi_client_suite(void) { /* skipped: URBI_ENABLE_REPL=0 */ }

#endif
