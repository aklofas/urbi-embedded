/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl.c — the cooperative service loop behind <urbi/repl.h>.
 *
 * One sweep per call, four phases per session: read what the transport
 * has, run every COMPLETE request in it, write back what the transport
 * will take, and close the session if its stream has ended and its
 * output has drained.  Nothing blocks and nothing is queued, because the
 * caller's thread is the only thread and a request is finished before
 * the next one is looked at.
 *
 * That ordering is the contract a client depends on: responses to
 * request N are all in the stream before any response to request N+1,
 * and an eval's own output precedes its result. */

#include "repl/urepl_dispatch.h"

#include <stdlib.h>
#include <string.h>

/* One read per session per sweep.  Big enough that a typical request
 * arrives whole, small enough to be a stack buffer. */
#define UREPL_READ_CHUNK 4096u

/* ---- inbound line assembly --------------------------------------------- */

/* Grows the accumulator to hold `need` more bytes, up to the framing cap.
 * false means the line is over the cap, which the caller turns into a
 * discard-until-newline rather than an allocation failure. */
static bool inbuf_reserve(UReplSession *s, size_t need)
{
    if (s->in_fill + need <= s->in_cap) return true;
    if (s->in_fill + need > UREPL_MAX_LINE) return false;
    size_t cap = s->in_cap ? s->in_cap : 1024u;
    while (cap < s->in_fill + need) cap *= 2u;
    if (cap > UREPL_MAX_LINE) cap = UREPL_MAX_LINE;
    char *n = (char *)realloc(s->in, cap);
    if (n == NULL) return false;
    s->in = n;
    s->in_cap = cap;
    return true;
}

/* Runs every complete line held in the accumulator and keeps the partial
 * tail for the next sweep. */
static void drain_lines(UReplServer *server, UReplSession *s)
{
    size_t start = 0;
    for (;;) {
        const char *nl = (const char *)memchr(s->in + start, '\n', s->in_fill - start);
        if (nl == NULL) break;
        size_t len = (size_t)(nl - (s->in + start));
        if (s->in_discard) {
            /* The newline that ends the oversized line is the resync
             * point; everything before it was already thrown away. */
            s->in_discard = false;
        } else {
            /* Tolerate CRLF from a line-oriented client. */
            size_t l = len;
            if (l > 0 && s->in[start + l - 1] == '\r') l--;
            if (l > 0) {
                UReplNdjsonReq req;
                if (urepl_ndjson_parse(s->in + start, l, &req) == 0) {
                    urepl_dispatch(server, s, &req);
                    urepl_ndjson_free_req(&req);
                } else {
                    urepl_dispatch_parse_error(s);
                }
            }
        }
        start += len + 1;
        if (start >= s->in_fill) break;
    }
    if (start > 0) {
        memmove(s->in, s->in + start, s->in_fill - start);
        s->in_fill -= start;
    }
}

static void read_sweep(UReplServer *server, UReplSession *s)
{
    if (s->ended || s->transport.read == NULL) return;
    char chunk[UREPL_READ_CHUNK];
    int rc = s->transport.read(s->transport.ctx, chunk, sizeof chunk);
    if (rc < 0) { s->ended = true; return; }
    if (rc == 0) return;

    size_t n = (size_t)rc;
    if (!inbuf_reserve(s, n)) {
        /* Over the framing cap.  Drop what is held, refuse the rest of
         * this line, and tell the client — a silent truncation would be
         * indistinguishable from a request that ran. */
        s->in_fill = 0;
        s->in_discard = true;
        urepl_dispatch_line_too_long(s);
        /* Still scan the chunk for the newline that ends the bad line, so
         * a request packed behind it is not lost. */
        const char *nl = (const char *)memchr(chunk, '\n', n);
        if (nl != NULL) {
            size_t after = n - (size_t)(nl - chunk) - 1u;
            s->in_discard = false;
            if (after > 0 && inbuf_reserve(s, after)) {
                memcpy(s->in + s->in_fill, nl + 1, after);
                s->in_fill += after;
            }
        }
    } else {
        memcpy(s->in + s->in_fill, chunk, n);
        s->in_fill += n;
    }
    drain_lines(server, s);
}

static void write_sweep(UReplSession *s)
{
    UReplOutBuf *o = &s->out;
    if (o->dropped && o->fill == o->off) {
        /* Report the gap only once the buffer has room again, so the
         * report itself cannot be the thing that is dropped. */
        o->dropped = false;
        o->fill = 0;
        o->off = 0;
        urepl_dispatch_output_dropped(s);
    }
    while (o->off < o->fill) {
        if (s->transport.write == NULL) { o->off = o->fill; break; }
        int rc = s->transport.write(s->transport.ctx, o->buf + o->off, o->fill - o->off);
        if (rc < 0) { s->ended = true; return; }
        if (rc == 0) return;            /* the transport is full for now */
        o->off += (size_t)rc;
    }
    if (o->off >= o->fill) { o->fill = 0; o->off = 0; }
}

/* ---- public surface ---------------------------------------------------- */

int urbi_repl_serve_init(UVM *vm, const UReplConfig *cfg, UReplServer **out_server)
{
    if (vm == NULL || out_server == NULL) return URBI_ERR_INVALID_ARG;
    *out_server = NULL;
    UReplServer *server = (UReplServer *)calloc(1, sizeof *server);
    if (server == NULL) return URBI_ERR_OOM;
    server->vm = vm;
    if (cfg != NULL) server->cfg = *cfg;
    *out_server = server;
    return URBI_OK;
}

int urbi_repl_register_transport(UReplServer *server, const UTransport *transport)
{
    if (server == NULL || transport == NULL) return URBI_ERR_INVALID_ARG;
    if (transport->read == NULL || transport->write == NULL) return URBI_ERR_INVALID_ARG;
    return urepl_session_create(server, transport) != NULL ? URBI_OK : URBI_ERR_OOM;
}

int urbi_repl_serve_step(UReplServer *server, uint64_t timeout_us)
{
    (void)timeout_us;   /* the sweep never waits; the caller owns the clock */
    if (server == NULL) return URBI_ERR_INVALID_ARG;
    UReplSession *s = server->sessions;
    while (s != NULL) {
        /* Captured before the sweep: a session that ends inside it is
         * freed at the bottom of the loop. */
        UReplSession *next = s->next;
        read_sweep(server, s);
        write_sweep(s);
        /* A closing session keeps its place until everything it has
         * already answered is on the wire. */
        if (s->ended && s->out.off >= s->out.fill) urepl_session_destroy(server, s);
        s = next;
    }
    return URBI_OK;
}

void urbi_repl_serve_shutdown(UReplServer *server)
{
    if (server == NULL) return;
    while (server->sessions != NULL) urepl_session_destroy(server, server->sessions);
    free(server);
}
