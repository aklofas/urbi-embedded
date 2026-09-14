/* SPDX-License-Identifier: BSD-3-Clause */
/* <urbi/repl.h> — the cooperative NDJSON eval service.
 *
 * WHAT THIS IS.  One line of JSON in, one or more lines of JSON out, over
 * a byte stream the host supplies.  Each registered transport is one
 * session: its own realm, its own globals, its own output.  The service
 * never starts a thread and never owns a socket — the host calls
 * urbi_repl_serve_step from the same loop it calls urbi_step from, which
 * is what makes it usable on a microcontroller with no scheduler of its
 * own and what keeps every VM touch on one thread.
 *
 * WHAT THIS IS NOT, yet.  The networked server — the listener, the
 * per-connection threads, auth, rate limiting, the TCP/Unix/UART
 * transports — is parked during the core re-foundation and returns in a
 * later phase.  `UTransport` here is the minimal four-member vtable that
 * phase will build on, not the accept-capable one the old server used.
 *
 * THE WIRE.  Requests:
 *
 *   {"id":1,"op":"eval","code":"1+2"}
 *   {"id":2,"op":"introspect","what":"coros"}
 *
 * Responses, one JSON document per line:
 *
 *   {"id":1,"kind":"output","channel":"clog","msg":"..."}   zero or more
 *   {"id":1,"kind":"result","value":"3"}                    or "error"
 *   {"id":1,"kind":"done"}                                  eval only
 *
 * An eval's value is rendered the way the REPL prints a value and handed
 * over as a JSON STRING.  An introspect's value is the JSON the primitive
 * built, inline.  Output written after the `done` — by a watcher or a
 * timer the eval armed — carries no id. */

#ifndef URBI_REPL_H
#define URBI_REPL_H

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility push(default)
#endif

#ifdef URBI_BYTECODE_ONLY
#  error "the REPL eval service requires the compiler frontend; it cannot be used with URBI_BYTECODE_ONLY"
#endif

#include <stddef.h>
#include <stdint.h>

#include <urbi/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct URealm;

/* Per-server configuration.  A zeroed struct is valid and means "the
 * defaults", which is what passing NULL does.
 *
 *   output_buf_cap — per-session output staging, in bytes.  0 = 64 KiB.
 *                    A response that does not fit is dropped and the
 *                    client is told so, rather than blocking the VM.
 *   default_budget — compile limits applied to every session's realm.
 *                    Zero in a field disables that limit.  The text a
 *                    session compiles comes from outside, so a host that
 *                    cares about a hostile client sets these. */
typedef struct UReplConfig {
    size_t         output_buf_cap;
    UCompileBudget default_budget;
} UReplConfig;

/* A byte stream, already connected.  The service never accepts, resolves
 * or dials: it reads, writes and closes what it is handed.
 *
 *   read  — up to n bytes.  Positive is the count; ZERO means nothing is
 *           available this instant and the stream is still open, which is
 *           the ordinary answer in a polling loop; NEGATIVE means the
 *           stream has ended or failed, and the session is closed.
 *   write — up to n bytes; returns the count written, which may be less
 *           than n.  The remainder is re-offered on the next step.  A
 *           negative return ends the session the same way.
 *   close — releases whatever ctx owns.  Called exactly once per
 *           registered transport, at shutdown or at end of stream. */
typedef struct UTransport {
    void *ctx;
    int (*read)(void *ctx, void *buf, size_t n);
    int (*write)(void *ctx, const void *buf, size_t n);
    void (*close)(void *ctx);
} UTransport;

typedef struct UReplServer UReplServer;

/* Creates the service.  No realm and no session exists yet; each arrives
 * with a transport.  URBI_OK, or URBI_ERR_INVALID_ARG / URBI_ERR_OOM. */
int  urbi_repl_serve_init(struct UVM *vm, const UReplConfig *cfg,
                          UReplServer **out_server);

/* Adopts one connected stream as one session, which allocates its realm.
 * The UTransport is COPIED; its ctx is borrowed and stays the caller's
 * until close is called on it.  URBI_OK, or URBI_ERR_INVALID_ARG /
 * URBI_ERR_OOM. */
int  urbi_repl_register_transport(UReplServer *server, const UTransport *transport);

/* One non-blocking sweep over every session: read what is there, run the
 * complete requests, write back what fits.  A session whose stream has
 * ended is closed, which runs its `handleDisconnect` and frees its realm.
 *
 * `timeout_us` is accepted and ignored: the sweep never waits, so pacing
 * an idle loop is the caller's business (it already owns the clock).
 * Always URBI_OK unless `server` is NULL. */
int  urbi_repl_serve_step(UReplServer *server, uint64_t timeout_us);

/* Closes every session and frees the service.  NULL-tolerant. */
void urbi_repl_serve_shutdown(UReplServer *server);

#ifdef __cplusplus
}
#endif

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility pop
#endif
#endif /* URBI_REPL_H */
