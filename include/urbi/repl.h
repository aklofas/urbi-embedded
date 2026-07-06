/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi/repl.h - public REPL service API (v0.9.1+)
 *
 * Opt-in via URBI_ENABLE_REPL=1 at build time. Provides a networked
 * NDJSON line-protocol REPL service over pluggable transports
 * (TCP / Unix sockets / UART / in-process buffers).
 *
 * Default-secure: a non-loopback bind without an auth token is rejected
 * with URBI_ERR_INSECURE_CONFIG at urbi_repl_serve.
 *
 * This header is included automatically by <urbi/urbi.h> when the
 * URBI_ENABLE_REPL macro is defined at configuration time. */
#ifndef URBI_REPL_H
#define URBI_REPL_H

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility push(default)   /* v1.0: export only public-header symbols */
#endif

#ifdef URBI_BYTECODE_ONLY
#  error "URBI_ENABLE_REPL requires the compiler frontend; cannot use with URBI_BYTECODE_ONLY"
#endif

#include <stddef.h>
#include <stdint.h>

#include <urbi/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct URealm;

/* Per-server configuration.  Populate before urbi_repl_serve.  Fields:
 *
 *   bind_addr           — TCP bind address; NULL or "127.0.0.1"/"::1"
 *                         treated as loopback for the default-secure
 *                         check; "/path/to/sock" (leading '/') treated
 *                         as Unix-socket and considered loopback.
 *   tcp_port            — TCP port; -1 disables TCP transport.
 *   unix_path           — Unix-socket path; NULL disables.
 *   auth_token          — bearer token; NULL disables (loopback only).
 *   max_clients         — per-server connection cap (default 16 if 0).
 *   output_ringbuf_cap  — per-session output ringbuf cap in bytes
 *                         (default 64 * 1024 if 0).
 *   default_budget      — per-lobby compile budget; applied to each new
 *                         session's realm.  Zero-fields => unlimited;
 *                         caller may copy URBI_DEFAULT_REPL_BUDGET here. */
typedef struct UReplConfig {
    const char    *bind_addr;
    int            tcp_port;
    const char    *unix_path;
    const char    *auth_token;
    int            max_clients;
    size_t         output_ringbuf_cap;
    UCompileBudget default_budget;
    /* === Per-source job rate limit ===
     * Maximum NDJSON jobs accepted per session per second.  0 = unlimited
     * (default).  When a session exceeds this burst, the connection is
     * terminated with a rate_limit_exceeded error envelope.  Counts all
     * ops (auth, eval, cancel, introspect); does not discriminate by op
     * type.  The counter resets each clock-second (wall clock, not relative).
     * Only enforced when URBI_ENABLE_REPL=1 and not URBI_REPL_COOPERATIVE_ONLY
     * (freestanding targets have no network threat model). */
    int            rate_limit_per_second;
} UReplConfig;

typedef struct UReplServer UReplServer;

/* Pluggable transport vtable.  An accept_fn returning non-zero means
 * "no client waiting" (treat as URBI_ERR_WOULD_BLOCK).  read_fn /
 * write_fn return number of bytes transferred, or negative URBI_ERR_*
 * on error. */
typedef struct UTransport {
    const char *name;
    int   (*accept_fn)      (void *listener_state, int *out_client_fd);
    int   (*read_fn)        (int client_fd, void *buf, size_t n);
    int   (*write_fn)       (int client_fd, const void *buf, size_t n);
    void  (*close_fn)       (int client_fd);
    int   (*pollable_fd_fn) (int client_fd);
} UTransport;

/* === Lifecycle ===
 *
 * urbi_repl_serve creates the server and (for transports that spawn one,
 * Phase 3+) starts the listener thread.  Returns NULL on failure with
 * *out_err set to URBI_ERR_*; out_err may be NULL.
 *
 * urbi_repl_stop signals shutdown, joins worker threads, frees per-
 * session state, and frees the server itself.  Idempotent on NULL.
 *
 * Phase 2 (v0.9.1) ships the data-plane primitives — queue / ringbuf /
 * dispatcher / NDJSON codec / in-process buffer transport — but no
 * networked listener yet.  serve_step is the host's manual drive hook;
 * Phase 3 connects it to a real accept/read loop. */
UReplServer *urbi_repl_serve    (struct UVM *vm, const UReplConfig *cfg, int *out_err);
void          urbi_repl_stop     (UReplServer *server);

int  urbi_repl_serve_init    (struct UVM *vm, const UReplConfig *cfg, UReplServer **out_server);

/* === Cooperative drive (v0.9.4+) ===
 *
 * urbi_repl_serve_step drives the data plane for transports whose
 * pollable_fd_fn returns -1 (Pi Pico USB CDC + UART, ESP-IDF UART,
 * FreeRTOS UART, in-process buffer).  Each call performs four
 * non-blocking sweeps over the registered transports + active
 * sessions:
 *
 *   1. accept  — one accept_fn attempt per non-pollable transport
 *   2. read    — one read_fn attempt per active session; complete
 *                NDJSON lines hit the dispatcher
 *   3. write   — drain pending output via write_fn (partial OK)
 *   4. close   — tear down sessions whose read_fn returned 0
 *
 * Pollable transports (TCP, Unix sockets) continue to use the
 * listener pthread — serve_step does NOT touch them.  Hosted
 * applications may mix both: pthread handles TCP, serve_step
 * handles a debug USB CDC link.
 *
 * The timeout_us argument is currently advisory on the cooperative
 * path: the sweep is best-effort non-blocking, and the embedder is
 * expected to __wfi() / sleep between calls.  Callers on pollable
 * transports treat timeout_us as a hint for the dispatcher idle
 * wait — see v0.9.1 dispatcher semantics. */
int  urbi_repl_serve_step    (UReplServer *server, uint64_t timeout_us);
void urbi_repl_serve_shutdown(UReplServer *server);

int  urbi_repl_register_transport(UReplServer *server,
                                  const UTransport *transport,
                                  void *listener_state);

#ifdef __cplusplus
}
#endif

/* === URBI_REPL_COOPERATIVE_ONLY link-time guard (audit-1 F2, roadmap F7) ===
 *
 * Cooperative-only builds change struct layouts in urepl_threading.h.
 * Embedders MUST link with the same URBI_REPL_COOPERATIVE_ONLY setting
 * the library was compiled with; a mismatch produces memory corruption
 * or silent feature failure.  This reference forces a link-time
 * undefined-symbol error instead, with a diagnostic name like:
 *   urbi_abi_requires_repl_cooperative_only (undefined)
 *
 * Suppressed when URBI_INTERNAL_GUARD_REF=1 (uabi_guards.c defines
 * the symbols and must not also reference them). */
#ifndef URBI_INTERNAL_GUARD_REF
#  ifdef URBI_REPL_COOPERATIVE_ONLY
extern const int urbi_abi_requires_repl_cooperative_only;
static const int *urbi_abi_repl_guard_ref __attribute__((unused)) =
    &urbi_abi_requires_repl_cooperative_only;
#  else
extern const int urbi_abi_requires_repl_pthread;
static const int *urbi_abi_repl_guard_ref __attribute__((unused)) =
    &urbi_abi_requires_repl_pthread;
#  endif
#endif /* !URBI_INTERNAL_GUARD_REF */


#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility pop
#endif
#endif /* URBI_REPL_H */
