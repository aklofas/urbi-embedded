/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi-server — the NDJSON eval service over TCP and Unix sockets, on
 * one thread.  Sockets are polled here; every accepted client becomes
 * one UTransport the library's cooperative service sweeps.  The library
 * never sees a socket. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>
#include "urbi/urbi.h"
#include "urbi/repl.h"

#define MAX_CLIENTS_DEFAULT 16
#define AUTH_LINE_MAX 1024

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* One accepted socket.  `closed` is set by the library's close callback;
 * the main loop then frees the slot.  `blocked` records that the last
 * write was short, so the loop also waits for the socket to drain. */
typedef struct Client { int fd; bool closed; bool blocked; } Client;

/* --- the transport the library sweeps -------------------------------- */
static int client_read(void *ctx, void *buf, size_t n)
{
    Client *c = (Client *)ctx;
    ssize_t r = recv(c->fd, buf, n, MSG_DONTWAIT);
    if (r > 0) return (int)r;
    if (r == 0) return -1;                       /* end of stream */
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}
static int client_write(void *ctx, const void *buf, size_t n)
{
    Client *c = (Client *)ctx;
    ssize_t r = send(c->fd, buf, n, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (r >= 0) { c->blocked = (size_t)r < n; return (int)r; }
    if (errno == EAGAIN || errno == EWOULDBLOCK) { c->blocked = true; return 0; }
    return -1;
}
static void client_close(void *ctx)
{
    Client *c = (Client *)ctx;
    if (!c->closed) { close(c->fd); c->closed = true; }
}

/* --- hello and auth, spoken by the tool before the library sees the client --- */
static bool send_all(int fd, const char *s, size_t n, int timeout_ms)
{
    while (n > 0) {
        struct pollfd p = { fd, POLLOUT, 0 };
        if (poll(&p, 1, timeout_ms) <= 0) return false;
        ssize_t w = send(fd, s, n, MSG_NOSIGNAL);
        if (w <= 0) return false;
        s += w; n -= (size_t)w;
    }
    return true;
}
/* Reads one line (up to AUTH_LINE_MAX) with a deadline; false on timeout,
 * overflow or disconnect.  `buf` is NUL-terminated either way.  Reads a
 * byte at a time so whatever follows the line stays in the socket for
 * the library.  Only used for the auth handshake. */
static bool read_line(int fd, char *buf, size_t cap, int timeout_ms)
{
    size_t n = 0;
    buf[0] = '\0';
    while (n + 1 < cap) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, timeout_ms) <= 0) break;
        ssize_t r = recv(fd, buf + n, 1, 0);
        if (r <= 0) break;
        if (buf[n] == '\n') { buf[n] = '\0'; return true; }
        n++;
    }
    buf[n] = '\0';
    return false;
}
/* The comparison takes the same time whatever bytes the client sent: it
 * walks the whole token and never stops at the first mismatch. */
static bool token_matches(const char *line, const char *token)
{
    const char *k = strstr(line, "\"token\":\"");
    if (!k) return false;
    k += 9;
    size_t tl = strlen(token), kl = strlen(k);
    volatile unsigned char acc = (unsigned char)(kl <= tl);
    for (size_t i = 0; i < tl; i++) {
        unsigned char got = i < kl ? (unsigned char)k[i] : 0u;
        acc |= (unsigned char)(got ^ (unsigned char)token[i]);
    }
    return acc == 0 && k[tl] == '"';
}
static unsigned long extract_id(const char *line)
{
    const char *k = strstr(line, "\"id\":");
    return k ? strtoul(k + 5, NULL, 10) : 0;
}

/* --- listeners -------------------------------------------------------- */
static int listen_tcp(const char *host, int port, int *out_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1; (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    if (strcmp(host, "localhost") == 0) host = "127.0.0.1";
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { close(fd); return -1; }
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 8) < 0) { close(fd); return -1; }
    socklen_t len = sizeof a;
    if (getsockname(fd, (struct sockaddr *)&a, &len) == 0) *out_port = ntohs(a.sin_port);
    return fd;
}
static int listen_unix(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a; memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    size_t pl = strlen(path);
    if (pl >= sizeof a.sun_path) { close(fd); return -1; }
    memcpy(a.sun_path, path, pl + 1);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 8) < 0) { close(fd); return -1; }
    return fd;
}
static bool is_loopback(const char *host)
{
    return strcmp(host, "localhost") == 0 || strncmp(host, "127.", 4) == 0;
}

/* --- boot script ------------------------------------------------------ */
static int run_boot(UVM *vm, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "urbi-server: cannot open %s\n", path); return 1; }
    char *src = NULL; size_t len = 0, cap = 0;
    for (;;) {
        if (len + 4096 > cap) {
            size_t ncap = cap ? cap * 2 : 8192;
            char *grown = realloc(src, ncap);
            if (!grown) { free(src); fclose(fp); return 1; }
            src = grown; cap = ncap;
        }
        size_t r = fread(src + len, 1, cap - len, fp);
        len += r; if (r == 0) break;
    }
    fclose(fp);
    UValue out = urbi_make_nil(); char err[256] = { 0 };
    int rc = urbi_run(vm, urbi_realm_main(vm), src, len, path, &out, err, sizeof err);
    free(src);
    if (rc != URBI_OK) { fprintf(stderr, "urbi-server: %s: %s\n", path, err[0] ? err : "uncaught exception"); return 1; }
    return 0;
}

/* realloc-shaped allocator and monotonic clock, as the urbi CLI installs. */
static void *server_alloc(void *p, size_t n, void *ud)
{
    (void)ud;
    if (n == 0) { free(p); return NULL; }
    return realloc(p, n);
}
static uint64_t host_clock_us(void *ud)
{
    (void)ud;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

static void usage(FILE *out)
{
    fputs("Usage: urbi-server [--tcp HOST:PORT] [--unix PATH] [--token TOK] [--boot FILE.u]\n"
          "                   [--budget N] [--max-clients N] [--quiet]\n"
          "A non-loopback --tcp without --token (or URBI_REPL_TOKEN) is refused.\n", out);
}

int main(int argc, char **argv)
{
    const char *tcp = NULL, *unix_path = NULL, *boot = NULL;
    const char *token = getenv("URBI_REPL_TOKEN");
    unsigned budget = 1024; int max_clients = MAX_CLIENTS_DEFAULT; bool quiet = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        else if (!strcmp(a, "--tcp") && i + 1 < argc) tcp = argv[++i];
        else if (!strcmp(a, "--unix") && i + 1 < argc) unix_path = argv[++i];
        else if (!strcmp(a, "--token") && i + 1 < argc) token = argv[++i];
        else if (!strcmp(a, "--boot") && i + 1 < argc) boot = argv[++i];
        else if (!strcmp(a, "--budget") && i + 1 < argc) budget = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--max-clients") && i + 1 < argc) max_clients = atoi(argv[++i]);
        else if (!strcmp(a, "--quiet")) quiet = true;
        else { fprintf(stderr, "urbi-server: unknown argument %s\n", a); usage(stderr); return 2; }
    }
    if (!tcp && !unix_path) { tcp = "127.0.0.1:54000"; }
    char host[64] = "127.0.0.1"; int port = 54000;
    if (tcp) {
        const char *colon = strrchr(tcp, ':');
        if (!colon) { fprintf(stderr, "urbi-server: --tcp wants HOST:PORT\n"); return 2; }
        size_t hl = (size_t)(colon - tcp); if (hl >= sizeof host) hl = sizeof host - 1;
        memcpy(host, tcp, hl); host[hl] = '\0'; port = atoi(colon + 1);
        if (!is_loopback(host) && (!token || !token[0])) {
            fprintf(stderr, "urbi-server: refused: --tcp %s is non-loopback and no token is set\n", host);
            return 1;
        }
    }
    if (max_clients < 1 || max_clients > 1024) max_clients = MAX_CLIENTS_DEFAULT;

    /* Handlers go in before anything is bound, so a signal that arrives
     * as soon as the banner is out still takes the orderly exit. */
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

    UVM *vm = urbi_open(server_alloc, NULL, NULL);
    if (!vm) { fprintf(stderr, "urbi-server: urbi_open failed\n"); return 1; }
    urbi_set_clock(vm, host_clock_us, NULL);
    if (boot && run_boot(vm, boot) != 0) { urbi_close(vm); return 1; }
    UReplServer *srv = NULL;
    if (urbi_repl_serve_init(vm, NULL, &srv) != URBI_OK) { urbi_close(vm); return 1; }

    int rc = 1;
    int ltcp = -1, lunix = -1, bound_port = port;
    Client *clients = NULL;
    struct pollfd *pfds = NULL;
    if (tcp && (ltcp = listen_tcp(host, port, &bound_port)) < 0) {
        fprintf(stderr, "urbi-server: cannot bind %s:%d\n", host, port);
        goto out;
    }
    if (unix_path && (lunix = listen_unix(unix_path)) < 0) {
        fprintf(stderr, "urbi-server: cannot bind %s\n", unix_path);
        goto out;
    }
    clients = calloc((size_t)max_clients, sizeof *clients);
    pfds = calloc((size_t)max_clients + 2, sizeof *pfds);
    if (!clients || !pfds) goto out;
    for (int i = 0; i < max_clients; i++) clients[i].fd = -1;
    if (!quiet) {
        if (ltcp >= 0) fprintf(stderr, "urbi-server listening on %s:%d%s\n", host, bound_port, (token && token[0]) ? " (auth required)" : "");
        if (lunix >= 0) fprintf(stderr, "urbi-server listening on %s\n", unix_path);
        fflush(stderr);
    }

    char hello[128];
    snprintf(hello, sizeof hello, "{\"kind\":\"hello\",\"version\":\"%s\"}\n", urbi_version());

    while (!g_stop) {
        /* Accept.  A new client gets the hello, passes auth if a token is
         * set, and only then becomes a transport. */
        int listeners[2] = { ltcp, lunix };
        for (int li = 0; li < 2; li++) {
            if (listeners[li] < 0) continue;
            struct pollfd lp = { listeners[li], POLLIN, 0 };
            while (poll(&lp, 1, 0) > 0) {
                int fd = accept(listeners[li], NULL, NULL);
                if (fd < 0) break;
                int slot = -1;
                for (int i = 0; i < max_clients; i++) if (clients[i].fd < 0) { slot = i; break; }
                if (slot < 0) { close(fd); continue; }            /* over the cap: no hello */
                if (!send_all(fd, hello, strlen(hello), 1000)) { close(fd); continue; }
                if (token && token[0]) {
                    char line[AUTH_LINE_MAX];
                    bool ok = read_line(fd, line, sizeof line, 5000) && strstr(line, "\"op\":\"auth\"") && token_matches(line, token);
                    unsigned long id = extract_id(line);
                    char reply[160];
                    if (ok) snprintf(reply, sizeof reply, "{\"id\":%lu,\"kind\":\"auth_ok\"}\n", id);
                    else    snprintf(reply, sizeof reply, "{\"id\":%lu,\"kind\":\"error\",\"message\":\"auth required\"}\n", id);
                    (void)send_all(fd, reply, strlen(reply), 1000);
                    if (!ok) { close(fd); continue; }
                }
                clients[slot].fd = fd; clients[slot].closed = false; clients[slot].blocked = false;
                UTransport t = { &clients[slot], client_read, client_write, client_close };
                if (urbi_repl_register_transport(srv, &t) != URBI_OK) { close(fd); clients[slot].fd = -1; }
            }
        }
        /* Serve, run, serve: a request's output reaches the socket in the
         * same turn the eval produced it. */
        urbi_repl_serve_step(srv, 0);
        uint64_t wake_us = 0;
        int st = urbi_step(vm, budget, &wake_us);
        urbi_repl_serve_step(srv, 0);
        /* A session the library closed this turn gives its slot back
         * before the poll set is rebuilt, so a closed fd is never polled. */
        for (int i = 0; i < max_clients; i++) if (clients[i].fd >= 0 && clients[i].closed) clients[i].fd = -1;
        /* Sleep on the sockets until the VM's next deadline. */
        int timeout_ms = 1000;
        if (st == URBI_STEP_IDLE_UNTIL) {
            uint64_t now_us = host_clock_us(NULL);
            timeout_ms = wake_us > now_us ? (int)((wake_us - now_us) / 1000u) : 0;
            if (timeout_ms > 1000) timeout_ms = 1000;
        } else if (st != URBI_STEP_QUIESCENT) {
            timeout_ms = 0;
        }
        int n = 0;
        if (ltcp >= 0)  { pfds[n].fd = ltcp;  pfds[n].events = POLLIN; n++; }
        if (lunix >= 0) { pfds[n].fd = lunix; pfds[n].events = POLLIN; n++; }
        for (int i = 0; i < max_clients; i++) {
            if (clients[i].fd < 0) continue;
            pfds[n].fd = clients[i].fd;
            pfds[n].events = (short)(POLLIN | (clients[i].blocked ? POLLOUT : 0));
            n++;
        }
        (void)poll(pfds, (nfds_t)n, timeout_ms);
    }
    rc = 0;
out:
    urbi_repl_serve_shutdown(srv);
    if (ltcp >= 0) close(ltcp);
    if (lunix >= 0) { close(lunix); unlink(unix_path); }
    free(clients); free(pfds);
    urbi_close(vm);
    return rc;
}
