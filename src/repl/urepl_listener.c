/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_listener.c - listener pthread + per-connection readers
 *
 * Spec §3.1 thread model.  See urepl_listener.h for the lifecycle
 * contract and shutdown choreography.
 *
 * Only compiled when URBI_ENABLE_REPL=1. */
/* _POSIX_C_SOURCE=200809L exposes clock_gettime, CLOCK_MONOTONIC, nanosleep. */
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200809L
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#include "repl/urepl_listener.h"
#include "repl/urepl_auth.h"
#include "repl/urepl_dispatch.h"
#include "repl/urepl_ndjson.h"
#include "repl/urepl_queue.h"
#include "repl/urepl_transport_pty.h"
#include "repl/urepl_transport_tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---- Helpers --------------------------------------------------------- */

/* Drain (and discard) any pending bytes on an eventfd. */
static void
eventfd_drain(int fd)
{
    uint64_t v;
    while (read(fd, &v, sizeof(v)) == (ssize_t)sizeof(v)) {
        /* keep reading until EAGAIN */
    }
}

/* Signal an eventfd (best-effort; ignore EAGAIN — caller already had
 * a pending wake). */
static void
eventfd_signal(int fd)
{
    if (fd < 0) {
        return;
    }
    uint64_t v = 1;
    ssize_t w = write(fd, &v, sizeof(v));
    (void)w;
}

/* Drain a session's output ringbuf to the client socket.  Returns true
 * on clean drain (or short write — caller wakes us again), false on
 * a hard send error (caller should close the connection). */
static bool
flush_session_output(UReplReader *r)
{
    UReplSession *s = r->session;
    if (s == NULL) {
        return false;
    }
    /* Fixed-size pump buffer.  4 KB matches typical NDJSON envelope
     * sizes; multiple iterations cover larger bursts. */
    char buf[4096];
    for (;;) {
        size_t n = urepl_ringbuf_read(&s->output, buf, sizeof(buf));
        if (n == 0U) {
            return true;
        }
        size_t off = 0;
        while (off < n) {
            int w = r->transport->write_fn(r->client_fd,
                                           buf + off, n - off);
            if (w > 0) {
                off += (size_t)w;
                continue;
            }
            if (w == -1) {
                /* EAGAIN — short-write.  We dropped these bytes from
                 * the ringbuf; spec §3.5 already documents "oldest-byte
                 * loss" as the overflow policy.  For an EAGAIN here we
                 * could re-buffer, but in practice loopback TCP at
                 * v0.9.1 line rates rarely sees this.  Sleep briefly +
                 * retry.  TODO(v1.0-rc): proper POLLOUT-driven retry. */
                struct timespec ts = { 0, (long)1000 * 1000 };  /* 1 ms */
                nanosleep(&ts, NULL);
                continue;
            }
            /* hard error — peer reset / pipe closed. */
            return false;
        }
    }
}

/* ---- Reader subthread ------------------------------------------------ */

/* Process accumulated read-buffer for newline-delimited NDJSON lines.
 * On each '\n', parse the [start..pos) substring and push as a job.
 * Returns the new "start" index (bytes consumed); the caller compacts
 * the buffer if start > 0 on entry/exit. */
static size_t
reader_parse_lines(UReplReader *r, const char *buf, size_t fill)
{
    size_t start = 0;
    for (size_t i = 0; i < fill; ++i) {
        if (buf[i] != '\n') {
            continue;
        }
        size_t line_len = i - start;
        if (line_len > 0U) {
            /* Allocate + parse */
            UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
            if (job != NULL) {
                int rc = urepl_ndjson_parse(buf + start, line_len, &job->req);
                if (rc == 0) {
                    job->session_id = r->session->session_id;
                    if (urepl_queue_push(r->server->job_queue, job) != URBI_OK) {
                        urepl_ndjson_free_req(&job->req);
                        free(job);
                    }
                } else {
                    free(job);
                }
            }
        }
        start = i + 1;
    }
    return start;
}

static void *
reader_main(void *arg)
{
    UReplReader *r = (UReplReader *)arg;
    UReplServer *server = r->server;
    char rbuf[8192];      /* inbound NDJSON parse buffer */
    size_t fill = 0;

    int pollable = r->transport->pollable_fd_fn != NULL
                   ? r->transport->pollable_fd_fn(r->client_fd)
                   : r->client_fd;

    /* If the transport is not pollable (e.g. buffer transport), we
     * don't have a usable read fd — bail.  The buffer-transport unit
     * tests don't spawn a reader thread; this is defense-in-depth. */
    if (pollable < 0) {
        return NULL;
    }

    while (!UREPL_ATOMIC_LOAD_BOOL(&r->stop_requested)
           && !UREPL_ATOMIC_LOAD_BOOL(&server->shutting_down)) {
        struct pollfd pfds[2];
        int nfds = 0;
        pfds[nfds].fd = pollable;
        pfds[nfds].events = POLLIN;
        nfds++;
        if (r->wake_eventfd >= 0) {
            pfds[nfds].fd = r->wake_eventfd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        /* 1-second timeout so a missed wake-signal still drains within
         * a second.  In steady state the eventfd carries every wake so
         * the timeout is rarely hit. */
        int pr = poll(pfds, (nfds_t)nfds, 1000);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Always flush any pending output (we may have been woken by
         * the wake_eventfd OR be a periodic wake). */
        if (r->wake_eventfd >= 0 && (pfds[1].revents & POLLIN)) {
            eventfd_drain(r->wake_eventfd);
        }
        if (!flush_session_output(r)) {
            break;
        }

        /* Inbound socket data. */
        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            int rc = r->transport->read_fn(r->client_fd,
                                           rbuf + fill,
                                           sizeof(rbuf) - fill);
            if (rc == 0) {
                /* EOF — peer closed. */
                break;
            }
            if (rc < 0 && rc != -1) {
                /* hard error */
                break;
            }
            if (rc > 0) {
                fill += (size_t)rc;
                size_t consumed = reader_parse_lines(r, rbuf, fill);
                if (consumed > 0U) {
                    if (consumed < fill) {
                        memmove(rbuf, rbuf + consumed, fill - consumed);
                    }
                    fill -= consumed;
                }
                /* Overflow guard: if a single line exceeds buffer, drop
                 * the partial.  Spec §6 line cap is 1 MiB but the v0.9.1
                 * 8 KiB cap matches typical urbiscript expressions. */
                if (fill >= sizeof(rbuf)) {
                    fill = 0;
                }
            }
        }
    }

    /* Final flush attempt — don't strand any responses pending in the
     * ringbuf if the peer is still readable. */
    (void)flush_session_output(r);

    /* Close the client fd here so the listener doesn't need to track
     * which readers still own theirs.  close_fn is idempotent on -1. */
    if (r->transport->close_fn != NULL) {
        r->transport->close_fn(r->client_fd);
    }
    r->client_fd = -1;

    /* Tear down the session.  After this point the dispatcher will no
     * longer find this session_id (urepl_session_destroy unlinks it),
     * so any jobs already on the queue addressed to this session are
     * dropped cleanly by urepl_dispatch_job's "unknown session" path. */
    pthread_mutex_lock(&server->sessions_mutex);
    /* Detach session-side back-pointer before destroy so the wake-all
     * walk in urepl_listener_wake_all_readers doesn't dereference us
     * after free. */
    if (r->session != NULL) {
        r->session->reader = NULL;
    }
    pthread_mutex_unlock(&server->sessions_mutex);

    if (r->session != NULL) {
        urepl_session_destroy(server, r->session);
        r->session = NULL;
    }

    return NULL;
}

/* ---- Listener thread ------------------------------------------------- */

/* Allocate + spawn a reader subthread for an accepted client. */
static int
spawn_reader(UReplServer *server, const UTransport *transport,
             int client_fd, uint32_t peer_id)
{
    UReplReader *r = (UReplReader *)calloc(1, sizeof(*r));
    if (r == NULL) {
        if (transport->close_fn != NULL) {
            transport->close_fn(client_fd);
        }
        return URBI_ERR_OOM;
    }
    r->client_fd      = client_fd;
    r->transport      = transport;
    r->server         = server;
    r->wake_eventfd   = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    /* Create the per-connection session.  urepl_session_create grabs
     * sessions_mutex internally; safe to call from any thread. */
    UReplSession *session = urepl_session_create(server);
    if (session == NULL) {
        if (r->wake_eventfd >= 0) close(r->wake_eventfd);
        if (transport->close_fn != NULL) transport->close_fn(client_fd);
        free(r);
        return URBI_ERR_OOM;
    }
    r->session       = session;
    session->reader  = r;
    session->peer_id = peer_id;

    /* Link into the server's reader list under sessions_mutex. */
    pthread_mutex_lock(&server->sessions_mutex);
    r->next = server->readers_head;
    server->readers_head = r;
    pthread_mutex_unlock(&server->sessions_mutex);

    /* Emit the hello envelope into the session's output ringbuf.  The
     * reader thread will flush it on its first iteration.  Spec §6
     * defines the hello shape; auth_required is true iff a token was
     * configured. */
    char hello_env[256];
    size_t hello_n = 0;
    bool auth_required = (server->cfg.auth_token != NULL
                          && server->cfg.auth_token[0] != '\0');
    if (urepl_ndjson_emit_hello(hello_env, sizeof(hello_env),
                                session->lobby_id_hex,
                                /* synclines */ true,
                                auth_required,
                                &hello_n) == 0) {
        urepl_ringbuf_write(&session->output, hello_env, hello_n);
    }

    if (pthread_create(&r->thread, NULL, reader_main, r) != 0) {
        /* Spawn failed — tear down what we wired up.  The reader_main
         * path normally owns session destroy; we mirror that here. */
        pthread_mutex_lock(&server->sessions_mutex);
        UReplReader **cur = &server->readers_head;
        while (*cur != NULL) {
            if (*cur == r) { *cur = r->next; break; }
            cur = &(*cur)->next;
        }
        pthread_mutex_unlock(&server->sessions_mutex);
        /* `session` was non-NULL at this point (urepl_session_create
         * success returned above). */
        session->reader = NULL;
        urepl_session_destroy(server, session);
        if (r->wake_eventfd >= 0) close(r->wake_eventfd);
        if (transport->close_fn != NULL) transport->close_fn(client_fd);
        free(r);
        return URBI_ERR_OOM;
    }
    r->started = true;
    /* Wake the dispatcher / VM thread so it knows new output is
     * pending in the session's ringbuf — flushes the hello envelope
     * promptly via urepl_listener_wake_all_readers on the next step. */
    eventfd_signal(r->wake_eventfd);
    return URBI_OK;
}

/* Map a registered transport to its listener-side fd.  Returns -1 for
 * transports that don't have a kernel fd (in-process buffer transport),
 * which the listener thread will skip.
 *
 * v0.9.1 supports three transports with a real listen fd: TCP (a real
 * AF_INET listen socket), pty (the slave fd of an openpty() pair —
 * single-client, see urepl_transport_pty.c), plus the in-process buffer
 * transport which is unpollable and driven manually by unit tests.
 * The pluggable UTransport vtable does not surface a "listener-side
 * pollable fd" hook (its pollable_fd_fn is per-client), so we lift the
 * listen fd out by type-discriminating on the transport pointer.  Phase
 * 7 of v0.9.1 will land the UTransport.listener_pollable_fn extension;
 * for now the lock-in here is intentional and narrow. */
static int
listener_pollable_fd(const UReplTransportEntry *e)
{
    if (e == NULL || e->transport == NULL || e->listener_state == NULL) {
        return -1;
    }
    if (e->transport == &UREPL_TCP_TRANSPORT) {
        const UTcpListener *l = (const UTcpListener *)e->listener_state;
        return l->listen_fd;
    }
    if (e->transport == &UREPL_PTY_TRANSPORT) {
        return urepl_pty_slave_fd((const UPtyState *)e->listener_state);
    }
    /* Buffer transport and anything else without a kernel listen fd. */
    return -1;
}

/* Drain accept_fn on one transport, queueing each new client.  Bounded
 * by the inner accept_fn returning -1 (would-block / EAGAIN) — for
 * single-client transports (pty, UART) that's after the first call;
 * for multi-client transports (TCP) it's when accept() drains. */
static void
drain_transport_accepts(UReplServer *server,
                        const UReplTransportEntry *te)
{
    for (;;) {
        int client_fd = -1;
        int ar = te->transport->accept_fn(te->listener_state,
                                          &client_fd);
        if (ar == -1) return;           /* EAGAIN / no more clients */
        if (ar != 0)  return;           /* hard error */

        /* Task 18: extract peer id from accepted socket.  TCP →
         * IPv4 in_addr_t in network byte order; non-TCP (Unix /
         * pty / buffer) leaves it as 0. */
        uint32_t peer_id = 0;
        if (te->transport == &UREPL_TCP_TRANSPORT) {
            struct sockaddr_in sa;
            socklen_t slen = (socklen_t)sizeof(sa);
            if (getpeername(client_fd,
                            (struct sockaddr *)&sa, &slen) == 0
                && sa.sin_family == AF_INET) {
                peer_id = sa.sin_addr.s_addr;
            }
        }

        /* Task 18: per-IP rate-limit check.  Locked-out peers
         * get the connection closed immediately, without ever
         * seeing a hello envelope. */
        if (server->auth_limiter != NULL) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now_us = (uint64_t)ts.tv_sec * 1000000ULL
                              + (uint64_t)ts.tv_nsec / 1000ULL;
            pthread_mutex_lock(&server->auth_limiter_mutex);
            bool allowed = urepl_auth_limiter_check(
                (UReplAuthLimiter *)server->auth_limiter,
                peer_id, now_us);
            pthread_mutex_unlock(&server->auth_limiter_mutex);
            if (!allowed) {
                if (te->transport->close_fn != NULL) {
                    te->transport->close_fn(client_fd);
                }
                continue;
            }
        }

        /* Hand off to the VM thread.  spawn_reader does
         * VM-touching work (urepl_session_create allocates a
         * realm + boots stdlib via urbi_run_chunk → urbi_step)
         * and MUST NOT run on the listener thread.  Push onto
         * the accept queue; urepl_listener_drain_accepts on
         * the VM thread (called from the dispatch drain hook)
         * pops and calls spawn_reader. */
        UReplAcceptItem *item =
            (UReplAcceptItem *)calloc(1, sizeof(*item));
        if (item == NULL) {
            if (te->transport->close_fn != NULL) {
                te->transport->close_fn(client_fd);
            }
            continue;
        }
        item->client_fd = client_fd;
        item->peer_id   = peer_id;
        item->transport = te->transport;
        pthread_mutex_lock(&server->accept_queue_mutex);
        if (server->accept_tail != NULL) {
            server->accept_tail->next = item;
        } else {
            server->accept_head = item;
        }
        server->accept_tail = item;
        pthread_mutex_unlock(&server->accept_queue_mutex);
    }
}

static void *
listener_main(void *arg)
{
    UReplServer *server = (UReplServer *)arg;

    /* Build a pollfd set: one entry per transport's pollable listener
     * fd + one for stop_eventfd.  Max transports is small (1-2 at
     * v0.9.1: TCP + maybe Unix); 8 is a generous cap. */
    enum { MAX_TRANSPORTS = 8 };
    struct pollfd pfds[MAX_TRANSPORTS + 1];
    const UReplTransportEntry *entries[MAX_TRANSPORTS];

    /* Pre-poll eager accept pass for single-client pre-connected
     * transports (pty / future UART variants).  Their listen fd is
     * either absent or already-readable would-block on POLLIN until
     * the peer writes, so polling them first would deadlock the hello
     * envelope behind the master's first write.  accept_fn is
     * idempotent — it returns -1 after the single accept, so this
     * pass is a no-op on subsequent iterations.  TCP also goes
     * through this path on the first iteration but its accept_fn
     * returns -1 immediately (no pending connections at startup). */
    for (UReplTransportEntry *e = server->transports;
         e != NULL; e = e->next) {
        drain_transport_accepts(server, e);
    }

    while (!UREPL_ATOMIC_LOAD_BOOL(&server->shutting_down)) {
        int nfds = 0;
        UReplTransportEntry *e = server->transports;
        while (e != NULL && nfds < MAX_TRANSPORTS) {
            int listen_fd = listener_pollable_fd(e);
            if (listen_fd >= 0) {
                pfds[nfds].fd = listen_fd;
                pfds[nfds].events = POLLIN;
                entries[nfds] = e;
                nfds++;
            }
            e = e->next;
        }
        int stop_idx = nfds;
        pfds[stop_idx].fd = server->stop_eventfd;
        pfds[stop_idx].events = POLLIN;
        nfds++;

        /* Timeout 250 ms so we re-check shutting_down even on a quiet
         * listener (defense against a missed stop_eventfd signal). */
        int pr = poll(pfds, (nfds_t)nfds, 250);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfds[stop_idx].revents & POLLIN) {
            eventfd_drain(server->stop_eventfd);
            break;
        }

        /* Accept any waiting clients on each transport with POLLIN. */
        for (int i = 0; i < stop_idx; ++i) {
            if (!(pfds[i].revents & POLLIN)) continue;
            drain_transport_accepts(server, entries[i]);
        }
    }

    return NULL;
}

/* ---- Public ---------------------------------------------------------- */

int
urepl_listener_start(UReplServer *server)
{
    if (server == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    if (server->listener_running) {
        return URBI_OK;
    }
    if (server->transports == NULL) {
        return URBI_OK;  /* nothing to listen on */
    }

    /* Only spin up the listener pthread if at least one registered
     * transport has a real listener fd (kernel pollable).  The
     * in-process buffer transport returns -1 from listener_pollable_fd
     * and is driven by unit tests manually — no thread needed. */
    bool has_pollable = false;
    for (UReplTransportEntry *e = server->transports;
         e != NULL; e = e->next) {
        if (listener_pollable_fd(e) >= 0) {
            has_pollable = true;
            break;
        }
    }
    if (!has_pollable) {
        return URBI_OK;
    }

    if (server->stop_eventfd < 0) {
        server->stop_eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (server->stop_eventfd < 0) {
            return URBI_ERR_OOM;
        }
    }
    UREPL_ATOMIC_STORE_BOOL(&server->shutting_down, false);
    if (pthread_create(&server->listener_thread, NULL,
                       listener_main, server) != 0) {
        close(server->stop_eventfd);
        server->stop_eventfd = -1;
        return URBI_ERR_OOM;
    }
    server->listener_running = true;
    return URBI_OK;
}

void
urepl_listener_stop_and_join(UReplServer *server)
{
    if (server == NULL) {
        return;
    }
    /* Tell the listener thread + all readers to exit. */
    UREPL_ATOMIC_STORE_BOOL(&server->shutting_down, true);

    if (server->listener_running && server->stop_eventfd >= 0) {
        eventfd_signal(server->stop_eventfd);
        pthread_join(server->listener_thread, NULL);
        server->listener_running = false;
    }

    /* Force-close every reader's client_fd so any blocked recv()
     * returns 0/EOF and the reader exits its loop.  Then signal the
     * wake_eventfd in case the reader was sleeping in poll. */
    pthread_mutex_lock(&server->sessions_mutex);
    UReplReader *r = server->readers_head;
    while (r != NULL) {
        UREPL_ATOMIC_STORE_BOOL(&r->stop_requested, true);
        if (r->client_fd >= 0 && r->transport != NULL
            && r->transport->close_fn != NULL) {
            /* Half-close the socket so the reader's recv() returns 0
             * promptly without us also closing the fd (the reader's
             * own teardown calls close_fn after the loop).  Use
             * shutdown(SHUT_RDWR) on POSIX sockets.  For non-socket
             * fds (UART; v0.9.1 Phase 7) the equivalent is fd close
             * from this side. */
            shutdown(r->client_fd, SHUT_RDWR);
        }
        eventfd_signal(r->wake_eventfd);
        r = r->next;
    }
    pthread_mutex_unlock(&server->sessions_mutex);

    /* Join + free each reader.  readers_head mutates under the
     * sessions_mutex; pop one at a time to keep the lock short. */
    for (;;) {
        pthread_mutex_lock(&server->sessions_mutex);
        UReplReader *head = server->readers_head;
        if (head != NULL) {
            server->readers_head = head->next;
        }
        pthread_mutex_unlock(&server->sessions_mutex);
        if (head == NULL) break;

        if (head->started) {
            pthread_join(head->thread, NULL);
        }
        if (head->wake_eventfd >= 0) {
            close(head->wake_eventfd);
            head->wake_eventfd = -1;
        }
        /* client_fd already closed by reader_main + session destroyed
         * (by reader_main's exit path).  Defensive: if reader_main
         * never ran the session is still attached; clean up. */
        if (head->session != NULL) {
            head->session->reader = NULL;
            urepl_session_destroy(server, head->session);
            head->session = NULL;
        }
        free(head);
    }

    if (server->stop_eventfd >= 0) {
        close(server->stop_eventfd);
        server->stop_eventfd = -1;
    }
}

void
urepl_listener_wake_all_readers(UReplServer *server)
{
    if (server == NULL) {
        return;
    }
    pthread_mutex_lock(&server->sessions_mutex);
    UReplReader *r = server->readers_head;
    while (r != NULL) {
        eventfd_signal(r->wake_eventfd);
        r = r->next;
    }
    pthread_mutex_unlock(&server->sessions_mutex);
}

void
urepl_listener_drain_accepts(UReplServer *server)
{
    if (server == NULL) {
        return;
    }
    /* Detach the entire queue under the accept_queue_mutex, then
     * process outside the lock.  Keeps the listener thread's push
     * path unblocked while spawn_reader runs (slow — bootstraps a
     * realm). */
    pthread_mutex_lock(&server->accept_queue_mutex);
    UReplAcceptItem *head = server->accept_head;
    server->accept_head = NULL;
    server->accept_tail = NULL;
    pthread_mutex_unlock(&server->accept_queue_mutex);

    while (head != NULL) {
        UReplAcceptItem *next = head->next;
        (void)spawn_reader(server, head->transport,
                           head->client_fd, head->peer_id);
        free(head);
        head = next;
    }
}
