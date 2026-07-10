/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_listener.h - listener pthread + per-connection readers
 *
 * Internal-only.  Started by urbi_repl_serve when transports are
 * registered (or via the helper urbi_repl_start_listener used by the
 * `urbi --listen` CLI path).  Joined cleanly by urbi_repl_stop.
 *
 * The listener thread runs `poll()` on the transport's pollable fd
 * (from accept_fn's accepted state) plus a stop eventfd written by
 * urbi_repl_stop.  Per-accept the listener allocates a UReplSession,
 * emits a hello envelope into its output ringbuf, and spawns a reader
 * pthread.
 *
 * The reader pthread runs another `poll()` loop on the client fd plus
 * its own wake eventfd (poked by the dispatch drain hook from the VM
 * thread).  Inbound: NDJSON line-buffered parse + queue.push(job).
 * Outbound: drain session->output → send() to socket.
 *
 * Both threads stop on:
 *   - explicit shutdown via urbi_repl_stop_listener (writes stop_eventfd
 *     and closes all client fds, forcing reader recv() to return 0/EOF).
 *   - peer disconnect (recv() returns 0 — reader exits, listener leaves
 *     the reader on readers_head until urbi_repl_stop reaps it).
 *
 * Only compiled when URBI_ENABLE_REPL=1. */
#ifndef UREPL_LISTENER_H
#define UREPL_LISTENER_H

#include "repl/urepl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn the listener pthread on server.  No-op if already running or
 * if no transports are registered.  Returns URBI_OK / URBI_ERR_*. */
int  urepl_listener_start(UReplServer *server);

/* Signal the listener (and all readers) to stop, then join everything.
 * Idempotent.  Called from urbi_repl_stop. */
void urepl_listener_stop_and_join(UReplServer *server);

/* Wake every active reader so it flushes session->output to the socket.
 * Called by the dispatch-drain hook after each VM-thread drain. */
void urepl_listener_wake_all_readers(UReplServer *server);

/* Drain the pending-accept queue on the VM thread.  For each item the
 * listener thread pushed (one per accepted client), this allocates a
 * session (which boots a realm — VM-touching) and spawns the per-
 * connection reader subthread.  MUST run on the VM thread; called by
 * urepl_dispatch_drain_if_active. */
void urepl_listener_drain_accepts(UReplServer *server);

/* v0.9.4: cooperative accept sweep for non-pollable transports
 * (Pico USB CDC, UART).  Iterates server->transports and on each
 * entry whose pollable_fd_fn returns -1 attempts one non-blocking
 * accept_fn drain; accepted clients are enqueued + immediately drained
 * via spawn_reader on the calling (VM) thread.  Pollable transports
 * are owned by the listener pthread and skipped here.  Returns the
 * number of new sessions accepted (informational; zero is normal). */
int  urepl_accept_sweep_nonpollable(UReplServer *server);

/* v0.9.4: cooperative read sweep.  For every session whose transport
 * is non-pollable (Pico USB CDC, UART), attempts one non-blocking
 * read_fn into the session's persistent inbound parse buffer.  Each
 * completed NDJSON line is pushed onto server->job_queue using the
 * same line-framing path that the reader pthread uses, so the
 * v0.9.1 dispatcher sees an identical job stream.  Sessions that
 * observe a clean EOF (or a hard transport error) have their
 * needs_teardown flag set for the close sweep to reap.
 *
 * Returns the total number of bytes consumed across all sessions
 * (informational).  Pollable sessions are skipped — owned by the
 * listener / reader pthread. */
int  urepl_read_sweep_nonpollable(UReplServer *server);

/* v0.9.4: cooperative write sweep.  For every non-pollable session
 * with pending output (either staged from a prior partial write, or
 * fresh bytes in session->output), attempts one non-blocking write_fn
 * call against the transport.  Bytes that the write_fn cannot accept
 * (EAGAIN / short write) stay in the per-session staging buffer for
 * the next sweep — order is preserved.  Sessions that observe a hard
 * transport error are marked needs_teardown for the close
 * sweep to reap.
 *
 * Returns the total bytes written across all sessions (informational;
 * zero is normal when there's nothing pending).  Pollable sessions
 * are skipped — owned by the reader pthread's flush_session_output. */
int  urepl_write_sweep_nonpollable(UReplServer *server);

/* v0.9.4: cooperative disconnect / teardown sweep (Phase D).  Walks
 * server->sessions_head and tears down any session whose needs_teardown
 * flag was set by the read or write sweeps (clean EOF on read_fn, hard
 * transport error on read_fn / write_fn).  For each reaped session:
 *
 *   - Calls the transport's close_fn(client_fd) exactly once.
 *   - Unlinks the paired UReplReader.  On pollable transports
 *     spawn_reader created a pthread; the close sweep pthread_joins
 *     it.  On non-pollable transports (v0.9.4) spawn_reader skipped
 *     the pthread_create entirely (cooperative readers are driven by
 *     urbi_repl_serve_step), and the close sweep skips the join via
 *     the reader->started guard.
 *   - Closes the reader's wake_eventfd (only allocated for pollable
 *     readers) and frees the reader struct.
 *   - Calls urepl_session_destroy, which fires the v0.9.1 disconnect-
 *     cleanup sequence (handleDisconnect / unregister / realm destroy /
 *     ringbuf + coop_inbuf/coop_outbuf free).
 *
 * Returns the count of teardowns (informational).  Pollable sessions
 * with needs_teardown set would be unusual (the reader pthread owns
 * their teardown via reader_main's exit path) but are handled the same
 * way for safety. */
int  urepl_disconnect_sweep(UReplServer *server);

#ifdef __cplusplus
}
#endif

#endif /* UREPL_LISTENER_H */
