/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi-send — one-shot NDJSON client for the v0.9.1 REPL service.
 *
 * Pure POSIX sockets + libc.  No liburbi dependency — this binary stays
 * usable from build hosts that don't have the runtime archive linked in.
 *
 * Exit status:
 *   0 — op completed and returned a non-error result
 *   1 — server returned an error envelope (kind=error)
 *   2 — usage / network / auth / parse error
 *
 * Token precedence: --token flag > URBI_REPL_TOKEN env > none.
 *
 * JSON escaping for `eval` code:  the v0.9.1 minimal escape handles
 * embedded " and \\ only.  Strings containing literal newlines or other
 * control characters are NOT supported by this client — emit them via
 * urbiscript escape sequences (e.g. "a\\nb").  Full JSON-safe escaping
 * is a v1.x followup. */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static void usage(FILE *out) {
    fputs(
        "Usage: urbi-send [options] OP [ARGS...]\n"
        "\n"
        "One-shot NDJSON client for urbi-server / urbi --listen.\n"
        "\n"
        "Options:\n"
        "  --host HOST[:PORT]   server address (default 127.0.0.1:54000)\n"
        "  --token TOK          bearer token (env URBI_REPL_TOKEN also used)\n"
        "  --lobby ID           target lobby id (default: server-assigned)\n"
        "  --tail               keep reading responses after `done`\n"
        "  --help, -h           show this help and exit\n"
        "\n"
        "Ops:\n"
        "  eval CODE            evaluate urbiscript source\n"
        "  introspect WHAT      runtime introspection (coros|tags|watchers|\n"
        "                         events|profile|gc|lobbies|stack|slots)\n"
        "  cancel TAG           stop a tag\n"
        "  lobby-new            allocate a new lobby (v0.9.1: stubbed)\n"
        "  lobby-close ID       close a lobby (v0.9.1: stubbed no-op)\n"
        "  tail                 open the stream and dump everything\n"
        "\n"
        "Exit status: 0 = ok, 1 = eval/server error, 2 = usage/network/auth.\n",
        out);
}

/* --- helpers --- */

static int parse_host_port(const char *arg, char *host_out, size_t host_cap,
                           int *port_out) {
    const char *colon = strrchr(arg, ':');
    if (colon) {
        size_t n = (size_t)(colon - arg);
        if (n == 0) {
            /* ":PORT" — default host */
            strncpy(host_out, "127.0.0.1", host_cap - 1);
            host_out[host_cap - 1] = '\0';
        } else {
            if (n >= host_cap) n = host_cap - 1;
            memcpy(host_out, arg, n);
            host_out[n] = '\0';
        }
        *port_out = atoi(colon + 1);
        if (*port_out <= 0) return -1;
    } else {
        /* No colon: treat whole arg as host, keep default port. */
        strncpy(host_out, arg, host_cap - 1);
        host_out[host_cap - 1] = '\0';
    }
    return 0;
}

/* Write all bytes or return -1.  Loops over EINTR. */
static ssize_t write_all(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t left = n;
    while (left > 0) {
        ssize_t w = send(fd, p, left, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        p    += (size_t)w;
        left -= (size_t)w;
    }
    return (ssize_t)n;
}

/* Read until we see a complete NDJSON line (i.e. a newline).  Returns
 * the number of bytes accumulated in `buf` (NUL-terminated).  -1 on
 * EOF/error before any newline. */
static ssize_t read_one_line(int fd, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        ssize_t r = recv(fd, buf + n, cap - 1 - n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) {
            if (n == 0) return -1;
            break;
        }
        n += (size_t)r;
        buf[n] = '\0';
        if (memchr(buf, '\n', n) != NULL) break;
    }
    return (ssize_t)n;
}

/* Minimal JSON escape: " and \\.  All other bytes copied verbatim.
 * v1.x: extend for \n, \r, \t, control chars. */
static size_t json_escape(const char *src, char *dst, size_t cap) {
    size_t n = 0;
    for (const char *p = src; *p && n + 2 < cap; ++p) {
        if (*p == '"' || *p == '\\') {
            dst[n++] = '\\';
        }
        dst[n++] = *p;
    }
    if (n < cap) dst[n] = '\0';
    return n;
}

/* Build the op-request JSON envelope; returns bytes written (excluding NUL)
 * or -1 on overflow / bad args. */
static int build_request(char *req, size_t cap, const char *op,
                         const char *arg, const char *lobby) {
    const char *lobby_pre = lobby ? ",\"lobby\":\"" : "";
    const char *lobby_val = lobby ? lobby : "";
    const char *lobby_post = lobby ? "\"" : "";
    int n;
    if (strcmp(op, "eval") == 0) {
        if (!arg) return -1;
        char esc[8192];
        json_escape(arg, esc, sizeof esc);
        n = snprintf(req, cap,
                     "{\"id\":2,\"op\":\"eval\",\"code\":\"%s\"%s%s%s}\n",
                     esc, lobby_pre, lobby_val, lobby_post);
    } else if (strcmp(op, "introspect") == 0) {
        if (!arg) return -1;
        n = snprintf(req, cap,
                     "{\"id\":2,\"op\":\"introspect\",\"what\":\"%s\"%s%s%s}\n",
                     arg, lobby_pre, lobby_val, lobby_post);
    } else if (strcmp(op, "cancel") == 0) {
        if (!arg) return -1;
        n = snprintf(req, cap,
                     "{\"id\":2,\"op\":\"cancel\",\"tag\":\"%s\"%s%s%s}\n",
                     arg, lobby_pre, lobby_val, lobby_post);
    } else if (strcmp(op, "lobby-new") == 0 || strcmp(op, "lobby_new") == 0) {
        /* Wire protocol uses underscored op names (see urepl_ndjson.c). */
        n = snprintf(req, cap,
                     "{\"id\":2,\"op\":\"lobby_new\"}\n");
    } else if (strcmp(op, "lobby-close") == 0 || strcmp(op, "lobby_close") == 0) {
        if (!arg) return -1;
        n = snprintf(req, cap,
                     "{\"id\":2,\"op\":\"lobby_close\",\"lobby\":\"%s\"}\n",
                     arg);
    } else if (strcmp(op, "tail") == 0) {
        /* No request — caller will keep the socket open. */
        if (cap > 0) req[0] = '\0';
        return 0;
    } else {
        return -1;
    }
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

/* --- main --- */

int main(int argc, char **argv) {
    const char *host_arg = getenv("URBI_REPL_HOST");
    const char *token    = getenv("URBI_REPL_TOKEN");
    const char *lobby    = NULL;
    bool tail = false;

    int i;
    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout); return 0;
        } else if (!strcmp(a, "--host") && i + 1 < argc) {
            host_arg = argv[++i];
        } else if (!strcmp(a, "--token") && i + 1 < argc) {
            token = argv[++i];
        } else if (!strcmp(a, "--lobby") && i + 1 < argc) {
            lobby = argv[++i];
        } else if (!strcmp(a, "--tail")) {
            tail = true;
        } else if (a[0] != '-') {
            break;  /* positional op */
        } else {
            fprintf(stderr, "urbi-send: unknown arg: %s\n", a);
            usage(stderr); return 2;
        }
    }
    if (i >= argc) { usage(stderr); return 2; }

    const char *op  = argv[i++];
    const char *arg = (i < argc) ? argv[i] : NULL;

    /* If op is "tail", set the flag and skip the request. */
    if (!strcmp(op, "tail")) tail = true;

    /* Parse host. */
    char host[256]; int port = 54000;
    strncpy(host, "127.0.0.1", sizeof host - 1);
    host[sizeof host - 1] = '\0';
    if (host_arg) {
        if (parse_host_port(host_arg, host, sizeof host, &port) < 0) {
            fprintf(stderr, "urbi-send: bad --host: %s\n", host_arg);
            return 2;
        }
    }

    /* Ignore SIGPIPE — broken pipe = read EOF. */
    signal(SIGPIPE, SIG_IGN);

    /* Connect. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("urbi-send: socket"); return 2; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "urbi-send: bad host: %s\n", host);
        close(fd); return 2;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "urbi-send: connect %s:%d: %s\n",
                host, port, strerror(errno));
        close(fd); return 2;
    }

    /* Read hello line. */
    char hello[2048];
    ssize_t hn = read_one_line(fd, hello, sizeof hello);
    if (hn <= 0) {
        fprintf(stderr, "urbi-send: no hello envelope from server\n");
        close(fd); return 2;
    }
    fputs(hello, stderr);
    if (hello[hn - 1] != '\n') fputc('\n', stderr);

    /* Auth if token set. */
    if (token && token[0]) {
        char authbuf[1024];
        int an = snprintf(authbuf, sizeof authbuf,
                          "{\"id\":1,\"op\":\"auth\",\"token\":\"%s\"}\n", token);
        if (an < 0 || (size_t)an >= sizeof authbuf) {
            fprintf(stderr, "urbi-send: token too long\n");
            close(fd); return 2;
        }
        if (write_all(fd, authbuf, (size_t)an) < 0) {
            fprintf(stderr, "urbi-send: write auth: %s\n", strerror(errno));
            close(fd); return 2;
        }
        char ackbuf[2048];
        ssize_t ackn = read_one_line(fd, ackbuf, sizeof ackbuf);
        if (ackn <= 0) {
            fprintf(stderr, "urbi-send: no auth response\n");
            close(fd); return 2;
        }
        if (strstr(ackbuf, "\"kind\":\"error\"") != NULL) {
            fprintf(stderr, "urbi-send: auth failed: %s", ackbuf);
            if (ackbuf[ackn - 1] != '\n') fputc('\n', stderr);
            close(fd); return 2;
        }
        /* Print ack envelope (typically kind:auth_ok). */
        fputs(ackbuf, stderr);
        if (ackbuf[ackn - 1] != '\n') fputc('\n', stderr);
    }

    /* Build + send the op request (skip for `tail`). */
    if (strcmp(op, "tail") != 0) {
        char req[16384];
        int rn = build_request(req, sizeof req, op, arg, lobby);
        if (rn < 0) {
            fprintf(stderr, "urbi-send: bad op or missing arg: %s\n", op);
            close(fd); return 2;
        }
        if (write_all(fd, req, (size_t)rn) < 0) {
            fprintf(stderr, "urbi-send: write op: %s\n", strerror(errno));
            close(fd); return 2;
        }
    }

    /* Read response lines.  `eval` ends with an explicit `kind:done`
     * envelope; other ops (introspect, cancel, lobby-new, lobby-close)
     * end after a single `result` (or `error`).  --tail / `tail` op
     * suppresses the early exit and keeps streaming until EOF. */
    bool eval_op  = (strcmp(op, "eval") == 0);
    int  exit_code = 0;
    char line[16384];
    for (;;) {
        ssize_t ln = read_one_line(fd, line, sizeof line);
        if (ln <= 0) break;
        fputs(line, stdout);
        if (line[ln - 1] != '\n') fputc('\n', stdout);
        fflush(stdout);
        if (!tail && strstr(line, "\"kind\":\"error\"") != NULL) {
            exit_code = 1;
            break;
        }
        if (!tail && eval_op
            && strstr(line, "\"kind\":\"done\"") != NULL) {
            break;
        }
        if (!tail && !eval_op
            && strstr(line, "\"kind\":\"result\"") != NULL) {
            break;
        }
    }
    close(fd);
    return exit_code;
}
