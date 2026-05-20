/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl.h - internal REPL service types (v0.9.1)
 *
 * Only compiled when URBI_ENABLE_REPL=1.  Public REPL types live in
 * <urbi/repl.h>; this header pulls those in and extends with internal-
 * only types (sessions, transport list, etc.). */
#ifndef SRC_REPL_UREPL_H
#define SRC_REPL_UREPL_H

#include "urbi/repl.h"
#include "urbi/urbi.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations — full definitions in private TUs. */
typedef struct UReplSession UReplSession;
typedef struct UReplQueue UReplQueue;
typedef struct UReplRingbuf UReplRingbuf;
typedef struct UReplJob UReplJob;
typedef struct UReplTransportEntry UReplTransportEntry;
typedef struct UReplAcceptItem UReplAcceptItem;

/* Pending-accept item.  Listener thread accepts a connection on the
 * kernel side (socket fd + peer id) and pushes one of these onto the
 * server's accept queue.  VM thread drains the queue at the dispatch
 * boundary and does the *VM-touching* work — session create (allocates
 * a realm + stdlib boot) + reader-pthread spawn — on the VM thread,
 * preserving the spec §3.1 invariant that VM state is single-threaded.
 *
 * Without this hand-off the listener thread would call
 * urbi_realm_create_repl → urbi_run_chunk → urbi_step, racing with the
 * host's own urbi_step on the VM. */
struct UReplAcceptItem {
    int                      client_fd;
    uint32_t                 peer_id;
    const UTransport        *transport;
    struct UReplAcceptItem  *next;
};

/* Cross-thread bool helpers.  shutting_down (server) and stop_requested
 * (reader) are set by one thread and polled by another; the bare bool
 * would be a data race even though the semantics are benign (one-shot
 * transition false→true, eventfd carries the wake-up).  Acquire/release
 * pairing makes the transition observable to helgrind/tsan and gives a
 * happens-before edge between the write and a subsequent read. */
#define UREPL_ATOMIC_LOAD_BOOL(p)  __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define UREPL_ATOMIC_STORE_BOOL(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

/* Transport list entry.  Each call to urbi_repl_register_transport
 * appends one of these to the server's transport chain. */
struct UReplTransportEntry {
    const UTransport          *transport;
    void                      *listener_state;
    struct UReplTransportEntry *next;
};

/* The server is opaque to embedders; this is the full layout used by
 * src/repl/* TUs.
 *
 * Threading (Phase 3):
 *   - VM thread (caller of urbi_repl_serve / urbi_step) owns the VM
 *     state.  Drains job_queue + dispatches each job + signals each
 *     session's reader wake_eventfd to flush output.
 *   - Listener pthread (when transports are registered) polls all
 *     transports + stop_eventfd; on accept allocates a session +
 *     spawns a per-connection reader subthread.
 *   - Per-session reader pthread polls client_fd + wake_eventfd;
 *     reads NDJSON lines + pushes jobs to job_queue, drains
 *     session->output to socket on wake.
 *
 * Mutex discipline:
 *   - sessions_mutex protects sessions_head + next_session_id and
 *     readers_head linked-list mutations.  All threads acquire it
 *     for the duration of list scans.
 *   - auth_limiter_mutex protects the auth limiter table only; held
 *     for the duration of check/record (microseconds). */
struct UReplServer {
    struct UVM              *vm;
    UReplConfig              cfg;
    UReplTransportEntry     *transports;
    UReplQueue              *job_queue;
    UReplSession            *sessions_head;
    uint32_t                 next_session_id;
    pthread_mutex_t          sessions_mutex;
    bool                     shutting_down;

    /* Phase 3 — listener + reader pthread machinery. */
    pthread_t                listener_thread;
    bool                     listener_running;
    int                      stop_eventfd;     /* -1 = not initialized */

    /* Per-session reader threads.  Indexed by session_id via the
     * sessions list; reader joins happen at urbi_repl_stop. */
    struct UReplReader      *readers_head;

    /* Pending-accept queue (listener producer / VM-thread consumer).
     * Protected by accept_queue_mutex; the listener pushes each new
     * accepted fd here, the VM thread drains in the dispatch hook. */
    UReplAcceptItem         *accept_head;
    UReplAcceptItem         *accept_tail;
    pthread_mutex_t          accept_queue_mutex;

    /* Phase 3 — per-IP auth-fail rate-limiter (Task 18 plugs the
     * impl).  void* keeps the auth-internal struct private to the
     * urepl_auth.c TU.  NULL when auth is disabled or before Task 18
     * lands. */
    void                    *auth_limiter;
    pthread_mutex_t          auth_limiter_mutex;
};

/* Per-connection reader subthread.  Created on accept, owned by the
 * listener thread until joined at urbi_repl_stop.  Each carries one
 * client fd + a wake eventfd used by the VM thread (via the dispatch
 * drain hook) to signal "output ready, please flush to socket". */
typedef struct UReplReader {
    pthread_t          thread;
    int                client_fd;
    int                wake_eventfd;       /* -1 = none */
    bool               started;
    bool               stop_requested;     /* set by shutdown path */
    /* v0.9.4: true when the reader has NO pthread — spawned for a
     * non-pollable transport (Pico USB CDC + UART, ESP-IDF UART,
     * FreeRTOS UART, in-process buffer).  The cooperative
     * urbi_repl_serve_step sweep drives accept/read/write/close
     * for these sessions instead of a per-connection reader thread.
     * Reap paths (urepl_listener_stop_and_join, close-sweep) gate
     * pthread_join on `started`, so cooperative readers are skipped
     * naturally.  The flag exists for documentation + to gate the
     * shutdown(client_fd, SHUT_RDWR) force-EOF path which is a
     * socket-only operation. */
    bool               cooperative;
    const UTransport  *transport;
    UReplSession      *session;
    UReplServer       *server;
    struct UReplReader *next;              /* server->readers_head chain */
} UReplReader;

#endif /* SRC_REPL_UREPL_H */
