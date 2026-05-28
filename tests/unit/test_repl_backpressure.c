/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_backpressure.c
 *
 * Regression tests for the flush_session_output backpressure fix.
 *
 * Background
 * ----------
 * When the client's socket send buffer is full, the reader pthread's
 * write_fn call returns -1 (EAGAIN).  The original code handled this
 * with a spin-wait inside flush_session_output:
 *
 *   while (off < n) {
 *       w = write_fn(fd, buf + off, n - off);
 *       if (w == -1) { nanosleep(1ms); continue; }
 *       ...
 *   }
 *
 * Because flush_session_output never returns while EAGAIN persists, the
 * reader pthread cannot:
 *   - check its stop_requested flag,
 *   - handle the wake_eventfd, or
 *   - service new POLLIN data from the client.
 *
 * The fix (W2.2 + W2.3):
 *   - flush_session_output stages destructively-read ringbuf bytes in
 *     the session's coop_outbuf staging buffer and returns a tri-state
 *     (FLUSH_DONE / FLUSH_WOULD_BLOCK / FLUSH_ERROR) instead of looping.
 *   - reader_main arms POLLOUT on the client fd when staging is non-empty,
 *     and resumes flushing when POLLOUT fires.
 *
 * Test suite
 * ----------
 *
 * Test 1: backpressure_reader_stops_promptly_on_stop_request
 *   Directly verifiable, timing-independent: while the socket is full
 *   (permanent EAGAIN), signal stop_requested + wake_eventfd to the
 *   reader.  With the old code the reader never returns from
 *   flush_session_output and therefore never checks stop_requested —
 *   the session's needs_teardown flag stays clear after 50 ms, causing
 *   the assertion to fail.  With the fix the reader exits
 *   flush_session_output on the first WOULD_BLOCK, re-enters its poll()
 *   loop, sees the wake signal, checks stop_requested, and sets
 *   needs_teardown within a few milliseconds.
 *
 * Test 2: backpressure_full_payload_survives_eagain
 *   End-to-end correctness: a 64 KiB recognisable payload is delivered
 *   byte-for-byte to the client side even after many EAGAIN cycles.
 *   Verifies that the staging buffer preserves bytes across WOULD_BLOCK
 *   returns and that order is maintained. */

#include "utest.h"

#ifdef URBI_ENABLE_REPL

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "repl/urepl.h"
#include "repl/urepl_dispatch.h"
#include "repl/urepl_listener.h"
#include "repl/urepl_queue.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define UTEST(name) static void name(void)

/* ---- Socketpair-backed pollable test transport ------------------------- */

/* Single-test pattern: sp_accept stores sv[0] here so the vtable
 * functions can find their state without a real fd→state table. */
static __thread int g_sp_consumed_sv0 = -1;

static int
sp_accept(void *listener_state, int *out_client_fd)
{
    int *sv = (int *)listener_state;
    if (sv == NULL || out_client_fd == NULL || sv[0] < 0) return -1;
    *out_client_fd      = sv[0];
    g_sp_consumed_sv0  = sv[0];
    sv[0]              = -2;   /* mark consumed; next call returns -1 */
    return 0;
}

static int
sp_read(int fd, void *buf, size_t n)
{
    ssize_t r = recv(fd, buf, n, 0);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return -1;
        return -errno;
    }
    return (int)r;
}

static int
sp_write(int fd, const void *buf, size_t n)
{
    ssize_t w = send(fd, buf, n, MSG_NOSIGNAL);
    if (w < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return -1;
        return -errno;
    }
    return (int)w;
}

static void sp_close(int fd) { if (fd >= 0) close(fd); }

/* Marking the transport as pollable causes spawn_reader to create a
 * real reader pthread (r->cooperative = false) and register an
 * eventfd wake channel — exactly the code path we are testing. */
static int sp_pollable_fd(int fd) { return fd; }

static const UTransport SP_TRANSPORT = {
    .name           = "sp_test",
    .accept_fn      = sp_accept,
    .read_fn        = sp_read,
    .write_fn       = sp_write,
    .close_fn       = sp_close,
    .pollable_fd_fn = sp_pollable_fd
};

/* ---- Shared setup helpers ---------------------------------------------- */

static void
fill_payload(char *buf, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        buf[i] = (char)(unsigned char)(i & 0xFFu);
}

static long
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void
sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000L, (ms % 1000L) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Signal an eventfd (best-effort, ignore EAGAIN). */
static void
signal_eventfd(int fd)
{
    if (fd < 0) return;
    uint64_t v = 1;
    ssize_t w = write(fd, &v, sizeof(v));
    (void)w;
}

/* Drain (and discard) bytes from `fd` with a per-read timeout. */
static size_t
drain_fd(int fd, char *dst, size_t want, int timeout_ms)
{
    size_t got = 0;
    while (got < want) {
        struct pollfd pf = { .fd = fd, .events = POLLIN, .revents = 0 };
        if (poll(&pf, 1, timeout_ms) <= 0) break;
        if (!(pf.revents & POLLIN)) break;
        ssize_t r = recv(fd, dst + got, want - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
        timeout_ms = 5;     /* tight window after first byte */
    }
    return got;
}

/* ---- Common test harness ----------------------------------------------- */

typedef struct {
    int         sv[2];    /* sv[0] = server side; sv[1] = client side */
    UVM        *vm;
    UReplServer *server;
    UReplSession *session;
    UReplReader  *reader;
} BpHarness;

/* Allocate a minimally-buffered socketpair, create VM + REPL server,
 * register the SP_TRANSPORT, trigger accept → spawn_reader, and wait
 * for the reader pthread to reach its poll() wait.
 * Returns true on success. */
static bool
bp_harness_create(BpHarness *h)
{
    memset(h, 0, sizeof(*h));
    h->sv[0] = h->sv[1] = -1;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, h->sv) != 0) return false;

    int fl0 = fcntl(h->sv[0], F_GETFL, 0);
    int fl1 = fcntl(h->sv[1], F_GETFL, 0);
    if (fl0 < 0 || fl1 < 0) return false;
    fcntl(h->sv[0], F_SETFL, fl0 | O_NONBLOCK);
    fcntl(h->sv[1], F_SETFL, fl1 | O_NONBLOCK);

    /* Minimise kernel buffers so EAGAIN fires quickly.  Linux rounds
     * SO_SNDBUF up to 2 × page = 8 KiB minimum, still well below
     * our test payloads. */
    int buf4k = 4096;
    setsockopt(h->sv[0], SOL_SOCKET, SO_SNDBUF, &buf4k, sizeof(buf4k));
    setsockopt(h->sv[1], SOL_SOCKET, SO_RCVBUF, &buf4k, sizeof(buf4k));

    h->vm = (UVM *)calloc(1, sizeof(UVM));
    if (!h->vm) return false;
    if (urbi_vm_init(h->vm, NULL, NULL) != URBI_OK) return false;

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = -1;
    h->server = urbi_repl_serve(h->vm, &cfg, NULL);
    if (!h->server) return false;

    /* Register SP_TRANSPORT: sp_accept will consume sv[0] on first call. */
    int sv_state[2] = { h->sv[0], h->sv[1] };
    if (urbi_repl_register_transport(h->server, &SP_TRANSPORT, sv_state)
        != URBI_OK)
        return false;

    /* Trigger accept → spawn_reader → reader pthread.
     * urepl_accept_sweep_nonpollable calls drain_transport_accepts (our
     * sp_accept → pushes sv[0] onto the queue), then
     * urepl_listener_drain_accepts pops + calls spawn_reader.
     * spawn_reader: pollable_fd_fn(sv[0]) >= 0 → creates pthread. */
    urepl_accept_sweep_nonpollable(h->server);
    urepl_listener_drain_accepts(h->server);

    /* Wait up to 30 ms for the reader pthread to start. */
    for (int i = 0; i < 30 && h->server->readers_head == NULL; ++i)
        sleep_ms(1);
    h->reader = h->server->readers_head;
    if (!h->reader) return false;

    h->session = h->server->sessions_head;
    if (!h->session) return false;

    /* Discard the hello envelope so only test-injected bytes are in the
     * ring / socket. */
    {
        char d[512];
        while (urepl_ringbuf_fill(&h->session->output) > 0)
            urepl_ringbuf_read(&h->session->output, d, sizeof(d));
        drain_fd(h->sv[1], d, sizeof(d), 30);
    }

    /* Give the reader 20 ms to settle in its poll() wait. */
    sleep_ms(20);
    return true;
}

static void
bp_harness_destroy(BpHarness *h)
{
    if (h->sv[1] >= 0) { close(h->sv[1]); h->sv[1] = -1; }
    if (h->server)      urbi_repl_stop(h->server);
    if (h->vm)          urbi_vm_destroy(h->vm);
    free(h->vm);
}

/* ---- Test 1: stop_requested is serviced promptly under backpressure ---- */

UTEST(backpressure_reader_stops_promptly_on_stop_request)
{
    /* This test is timing-independent: it verifies a boolean invariant
     * (needs_teardown is set) rather than measuring elapsed time.  The
     * 50 ms window is generous; with the fix the reader exits in < 5 ms.
     *
     * Failure mode with the old code:
     *   reader_main calls flush_session_output which enters its
     *   nanosleep-retry loop and never returns — so reader_main never
     *   checks stop_requested, never sets needs_teardown. */

    BpHarness h;
    UASSERT(bp_harness_create(&h));

    /* Fill the socket send buffer so EAGAIN fires on the very first
     * write attempt.  Write a recognisable chunk larger than the 8 KiB
     * kernel buffer. */
    char payload[16 * 1024];
    fill_payload(payload, sizeof(payload));
    urepl_ringbuf_write(&h.session->output, payload, sizeof(payload));

    /* Wake the reader so it calls flush_session_output. */
    signal_eventfd(h.reader->wake_eventfd);

    /* Give the reader 10 ms to enter flush_session_output and fill the
     * socket buffer, reaching the EAGAIN path. */
    sleep_ms(10);

    /* With old code: reader is now stuck in the nanosleep-retry loop
     * inside flush_session_output.  It cannot observe stop_requested.
     * With the fix: flush returns WOULD_BLOCK, reader is in poll(). */

    /* Signal stop — without calling urbi_repl_stop (which would call
     * shutdown(sv[0]) and break the EAGAIN loop via hard error). */
    UREPL_ATOMIC_STORE_BOOL(&h.reader->stop_requested, true);
    signal_eventfd(h.reader->wake_eventfd);

    /* Wait 50 ms.  The fix makes the reader exit in < 5 ms.
     * The old code's reader is stuck; needs_teardown stays false. */
    sleep_ms(50);

    bool stopped = UREPL_ATOMIC_LOAD_BOOL(&h.session->needs_teardown);

    /* Drain sv[1] and call full stop before asserting so we don't
     * leak threads on test failure. */
    {
        char d[4096];
        drain_fd(h.sv[1], d, sizeof(d), 5);
    }
    close(h.sv[1]);
    h.sv[1] = -1;
    bp_harness_destroy(&h);

    /* The core assertion: reader must have exited and set needs_teardown.
     * FAILS with old code, PASSES with the fix. */
    UASSERT(stopped);
}

/* ---- Test 2: full payload is delivered correctly under backpressure ---- */

UTEST(backpressure_full_payload_survives_eagain)
{
    /* End-to-end correctness: 64 KiB payload → session output → reader
     * pthread flushes through backpressure → client side receives every
     * byte in order.  This exercises the staging-buffer preservation
     * property introduced by W2.2. */

    BpHarness h;
    UASSERT(bp_harness_create(&h));

#define BP2_PAYLOAD_SIZE (64U * 1024U)
    char *payload  = (char *)malloc(BP2_PAYLOAD_SIZE);
    char *received = (char *)calloc(1, BP2_PAYLOAD_SIZE + 1U);
    UASSERT(payload != NULL && received != NULL);
    fill_payload(payload, BP2_PAYLOAD_SIZE);

    /* Inject 64 KiB into session->output in one shot.
     * The default ringbuf cap is 64 KiB (UREPL_DEFAULT_OUTPUT_CAP). */
    size_t written = urepl_ringbuf_write(&h.session->output,
                                         payload, BP2_PAYLOAD_SIZE);
    UASSERT_EQ(written, BP2_PAYLOAD_SIZE);

    /* Wake the reader and interleave vm_steps with client-side draining
     * so the socket buffer never stays full for more than a few ms.
     * 300 ms total budget; delivery should complete well under 100 ms
     * with the fix. */
    size_t total_rx = 0;
    long   deadline = now_ms() + 300L;
    while (total_rx < BP2_PAYLOAD_SIZE && now_ms() < deadline) {
        urbi_step(h.vm, 50, NULL);      /* triggers wake_all_readers */
        signal_eventfd(h.reader->wake_eventfd);
        size_t got = drain_fd(h.sv[1], received + total_rx,
                              BP2_PAYLOAD_SIZE - total_rx, 10);
        total_rx += got;
    }

    free(payload);
    close(h.sv[1]);
    h.sv[1] = -1;

    bool ok_size  = (total_rx == BP2_PAYLOAD_SIZE);
    bool ok_order = ok_size &&
                    (memcmp(received, payload - BP2_PAYLOAD_SIZE /* freed! */,
                            0) == 0);  /* placeholder check after free */
    /* Re-check order properly by re-generating the expected pattern. */
    bool ok_bytes = true;
    for (size_t i = 0; i < total_rx && ok_bytes; ++i)
        ok_bytes = (received[i] == (char)(unsigned char)(i & 0xFFu));

    free(received);
    bp_harness_destroy(&h);

    UASSERT_EQ(total_rx, BP2_PAYLOAD_SIZE);
    UASSERT(ok_bytes);
#undef BP2_PAYLOAD_SIZE
}

#endif /* URBI_ENABLE_REPL */

void
test_repl_backpressure_suite(void)
{
#ifdef URBI_ENABLE_REPL
    printf("test_repl_backpressure\n");
    utest_run("backpressure_reader_stops_promptly_on_stop_request",
              backpressure_reader_stops_promptly_on_stop_request);
    utest_run("backpressure_full_payload_survives_eagain",
              backpressure_full_payload_survives_eagain);
#else
    printf("test_repl_backpressure (URBI_ENABLE_REPL=0, skipped)\n");
#endif
}
