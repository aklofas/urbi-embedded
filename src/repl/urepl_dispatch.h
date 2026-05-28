/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_dispatch.h - REPL job dispatcher + session machinery
 *
 * The dispatcher takes a parsed UReplJob, looks up its session, and
 * routes it to a per-op handler (auth / eval / cancel / introspect /
 * lobby_new / lobby_close).  Handlers emit NDJSON response envelopes
 * into the session's output ringbuf.
 *
 * Per-realm output is routed via urbi_realm_set_writer(session_writer)
 * at session creation; streaming output inside an eval frame is id-
 * correlated to the eval's id (current_eval_id), and post-done output
 * (e.g. from watchers spawned by the eval) is lobby-scoped (no id). */
#ifndef SRC_REPL_UREPL_DISPATCH_H
#define SRC_REPL_UREPL_DISPATCH_H

#include "repl/urepl.h"
#include "repl/urepl_queue.h"
#include "urbi/urbi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Per-connection lobby session.  Owned by the server's sessions_head
 * list; protected by server->sessions_mutex (for list mutations).
 * Per-session fields are touched only by the dispatcher thread (plus
 * read-only access by introspection callers). */
struct UReplSession {
    uint32_t              session_id;
    struct UVM           *vm;
    struct URealm        *realm;        /* the lobby's realm */
    UReplRingbuf          output;
    bool                  authed;
    char                  lobby_id_hex[10];  /* "a3f2" */
    UReplServer          *server;
    /* Live correlation id during a dispatch_eval frame; 0 outside an
     * eval (so session_writer routes streaming output as lobby-scoped). */
    uint64_t              current_eval_id;
    /* Phase 3 — Back-pointer to the reader subthread driving this
     * session's socket I/O.  NULL for sessions created outside the
     * listener path (e.g. unit-test sessions; buffer transport). */
    struct UReplReader   *reader;
    /* Task 18: peer identifier captured at accept() time.  For TCP this
     * is sockaddr_in.sin_addr.s_addr (network byte order); for Unix
     * sockets the listener stores the peer's pid (cast).  0 = unknown
     * (unit-test sessions; buffer transport).  Used by dispatch_auth
     * to bump the per-source rate-limiter on each auth_failed. */
    uint32_t              peer_id;
    /* v0.9.4 cooperative data plane: per-session inbound NDJSON parse
     * buffer.  Reader-pthread sessions keep their parse state on the
     * stack inside reader_main; non-pollable transports (Pico USB CDC,
     * UART) drive read in single-shot sweeps from urbi_repl_serve_step
     * and therefore must hold partial-line bytes across calls. */
    char                 *coop_inbuf;
    size_t                coop_inbuf_cap;
    size_t                coop_inbuf_fill;
    /* Write staging buffer, shared between the cooperative and the
     * reader-pthread output paths (mutually exclusive: the cooperative
     * write sweep filters out pollable sessions, so only one path ever
     * uses these fields at a time).
     *
     * Cooperative path (v0.9.4): urepl_ringbuf_read is destructive —
     * once bytes leave session->output the ringbuf no longer owns them.
     * Non-pollable transports pull a chunk into this buffer and advance
     * coop_outbuf_off on each (possibly partial) write_fn call.  When
     * off == fill the staging is drained and the next sweep refills
     * from the ringbuf.
     *
     * Reader-pthread path (W2.2): flush_session_output uses the same
     * fields to stage a destructively-read ringbuf chunk.  On EAGAIN
     * it returns FLUSH_WOULD_BLOCK (instead of spinning) and leaves
     * the unwritten bytes at [coop_outbuf_off, coop_outbuf_fill).
     * reader_main arms POLLOUT and resumes on the next writable event. */
    char                 *coop_outbuf;
    size_t                coop_outbuf_cap;
    size_t                coop_outbuf_fill;
    size_t                coop_outbuf_off;
    /* v0.9.4: set by the cooperative read sweep on a clean EOF (peer
     * disconnect, read_fn == 0).  Task 4.5's close sweep reaps these. */
    bool                  needs_teardown;
    /* === W4: per-session job rate limit (rate_limit_per_second) ===
     * Counts jobs dispatched in the current clock-second.  Resets when
     * rate_window_sec advances.  Only checked when server->cfg.rate_limit_per_second > 0. */
    int                   rate_jobs_this_sec;
    int64_t               rate_window_sec;   /* seconds since epoch of current window */
    struct UReplSession  *next;
};

/* Create a new session.  Allocates a URealm via urbi_realm_create_repl,
 * sizes its output ringbuf from server->cfg.output_ringbuf_cap, installs
 * the session_writer on the realm, and links into server->sessions_head.
 * Returns NULL on OOM. */
UReplSession *urepl_session_create(UReplServer *server);

/* Find a session by id (linear scan; OK at v0.9.1 client counts).
 * Returns NULL if not found. */
UReplSession *urepl_session_find(UReplServer *server, uint32_t session_id);

/* Find a session by hex lobby id.  Returns NULL if not found. */
UReplSession *urepl_session_find_by_lobby(UReplServer *server, const char *lobby_hex);

/* Unlink + free a session.  Also destroys the underlying URealm. */
void urepl_session_destroy(UReplServer *server, UReplSession *session);

/* === W1: single-owner teardown contract ===
 * Reader threads MUST NOT call urepl_session_destroy directly.  Instead
 * they call urepl_request_teardown(s) which sets needs_teardown atomically.
 * The VM-thread dispatcher reaps flagged sessions at the start of each
 * urepl_dispatch_drain call via urepl_session_reap_pending.  After this
 * call returns the caller owns no further reference to s.  Idempotent. */
void urepl_request_teardown(UReplSession *s);

/* Reap any sessions with needs_teardown set.  Called by the VM thread
 * at the head of urepl_dispatch_drain_if_active before job dispatch.
 * Only safe to call from the VM thread. */
void urepl_session_reap_pending(UReplServer *server);

/* Drive a single job.  Looks up the session, runs the op handler,
 * writes response envelopes to the session's output ringbuf, and
 * frees the job + its req strings.  Errors (unknown session, auth
 * required) emit an error envelope but do not propagate up. */
void urepl_dispatch_job(UReplServer *server, UReplJob *job);

/* Drain the server's job queue and dispatch each.  Convenience wrapper
 * for the listener thread (Phase 3). */
void urepl_dispatch_drain(UReplServer *server);

/* Step-driver hook (Phase 3).  Called from urbi_step at the top of
 * every host-thread driver invocation: if vm->repl (and vm->repl->server)
 * is non-NULL, drains its job queue + dispatches each job, then signals
 * all session reader subthreads to flush their output ringbufs to socket.
 *
 * The src/vm/ustep.c side declares this as a weak symbol so the default
 * build (URBI_ENABLE_REPL=0) links cleanly even though no urepl_*.o
 * archives are present.  The weak fallback is a no-op. */
void urepl_dispatch_drain_if_active(struct UVM *vm);

#endif /* SRC_REPL_UREPL_DISPATCH_H */
