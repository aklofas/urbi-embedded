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

/* Drive a single job.  Looks up the session, runs the op handler,
 * writes response envelopes to the session's output ringbuf, and
 * frees the job + its req strings.  Errors (unknown session, auth
 * required) emit an error envelope but do not propagate up. */
void urepl_dispatch_job(UReplServer *server, UReplJob *job);

/* Drain the server's job queue and dispatch each.  Convenience wrapper
 * for the listener thread (Phase 3). */
void urepl_dispatch_drain(UReplServer *server);

/* Step-driver hook (Phase 3).  Called from urbi_step at the top of
 * every host-thread driver invocation: if vm->repl_server is non-NULL,
 * drains its job queue + dispatches each job, then signals all session
 * reader subthreads to flush their output ringbufs to socket.
 *
 * The src/vm/ustep.c side declares this as a weak symbol so the default
 * build (URBI_ENABLE_REPL=0) links cleanly even though no urepl_*.o
 * archives are present.  The weak fallback is a no-op. */
void urepl_dispatch_drain_if_active(struct UVM *vm);

#endif /* SRC_REPL_UREPL_DISPATCH_H */
