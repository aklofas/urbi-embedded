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
#ifndef SRC_REPL_UREPL_LISTENER_H
#define SRC_REPL_UREPL_LISTENER_H

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

#ifdef __cplusplus
}
#endif

#endif /* SRC_REPL_UREPL_LISTENER_H */
