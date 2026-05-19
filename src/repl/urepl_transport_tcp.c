/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_transport_tcp.c - POSIX TCP listener for the REPL service
 *
 * IPv4-only at v0.9.1.  Non-blocking accept/read/write.  SO_REUSEADDR is
 * set so tests can rebind the kernel-assigned loopback port immediately
 * after a previous server shut down.  MSG_NOSIGNAL on send() avoids
 * SIGPIPE when the peer half-closes mid-write.
 *
 * Return-code conventions for the UTransport vtable (mirrors
 * urepl_buffer_transport.c):
 *
 *   accept_fn — returns 0 on success (out_client_fd set); -1 on EAGAIN
 *               (no client waiting; listener thread loops); negative
 *               errno on hard error.
 *   read_fn   — returns byte count (>= 0; 0 = EOF, reader thread
 *               should close); -1 on EAGAIN; negative errno on hard
 *               error.
 *   write_fn  — same shape as read_fn for partial writes.
 *
 * Only compiled when URBI_ENABLE_REPL=1. */
#include "repl/urepl_transport_tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int
tcp_accept(void *listener_state, int *out_client_fd)
{
    UTcpListener *l = (UTcpListener *)listener_state;
    if (l == NULL || out_client_fd == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    int fd = accept(l->listen_fd, NULL, NULL);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return -1;  /* "no client waiting" — listener thread loops */
        }
        return -errno;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) {
        (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    /* Disable Nagle so single-NDJSON-line responses round-trip without
     * the 40 ms coalescing delay.  Best-effort — not fatal if absent. */
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    *out_client_fd = fd;
    return 0;
}

static int
tcp_read(int client_fd, void *buf, size_t n)
{
    ssize_t r = recv(client_fd, buf, n, 0);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return -1;
        }
        return -errno;
    }
    return (int)r;  /* 0 = EOF (peer closed) */
}

static int
tcp_write(int client_fd, const void *buf, size_t n)
{
    ssize_t w = send(client_fd, buf, n, MSG_NOSIGNAL);
    if (w < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return -1;
        }
        return -errno;
    }
    return (int)w;
}

static void
tcp_close(int client_fd)
{
    if (client_fd >= 0) {
        (void)close(client_fd);
    }
}

static int
tcp_pollable_fd(int client_fd)
{
    return client_fd;
}

const UTransport UREPL_TCP_TRANSPORT = {
    .name           = "tcp",
    .accept_fn      = tcp_accept,
    .read_fn        = tcp_read,
    .write_fn       = tcp_write,
    .close_fn       = tcp_close,
    .pollable_fd_fn = tcp_pollable_fd
};

UTcpListener *
urepl_tcp_listener_create(const char *bind_addr, int port)
{
    if (port < 0 || port > 65535) {
        return NULL;
    }
    UTcpListener *l = (UTcpListener *)calloc(1, sizeof(*l));
    if (l == NULL) {
        return NULL;
    }
    l->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (l->listen_fd < 0) {
        free(l);
        return NULL;
    }

    int one = 1;
    (void)setsockopt(l->listen_fd, SOL_SOCKET, SO_REUSEADDR,
                     &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (bind_addr == NULL || bind_addr[0] == '\0') {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        (void)close(l->listen_fd);
        free(l);
        return NULL;
    }

    if (bind(l->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        (void)close(l->listen_fd);
        free(l);
        return NULL;
    }
    if (listen(l->listen_fd, 16) < 0) {
        (void)close(l->listen_fd);
        free(l);
        return NULL;
    }

    int fl = fcntl(l->listen_fd, F_GETFL, 0);
    if (fl >= 0) {
        (void)fcntl(l->listen_fd, F_SETFL, fl | O_NONBLOCK);
    }

    /* Read back the kernel-assigned port when caller passed 0. */
    socklen_t alen = (socklen_t)sizeof(addr);
    if (getsockname(l->listen_fd, (struct sockaddr *)&addr, &alen) == 0) {
        l->port = ntohs(addr.sin_port);
    } else {
        l->port = (uint16_t)port;
    }
    return l;
}

void
urepl_tcp_listener_destroy(UTcpListener *l)
{
    if (l == NULL) {
        return;
    }
    if (l->listen_fd >= 0) {
        (void)close(l->listen_fd);
        l->listen_fd = -1;
    }
    free(l);
}
