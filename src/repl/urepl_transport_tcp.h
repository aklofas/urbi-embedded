/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_transport_tcp.h - POSIX TCP loopback transport (v0.9.1)
 *
 * IPv4 only at v0.9.1 (IPv6 deferred to v2; mTLS deferred to v2 — see
 * spec §7.5).  Non-blocking accept/read/write.  SO_REUSEADDR set so the
 * test loopback can rebind immediately after teardown.
 *
 * A `UTcpListener` owns a single AF_INET listen socket + the kernel-
 * assigned port (readable after create even when port=0 was requested).
 * The listener_state pointer passed into `urbi_repl_register_transport`
 * is the `UTcpListener *`; per-connection state is just the accepted fd
 * returned by accept_fn (round-trips through read_fn/write_fn/close_fn).
 *
 * Only compiled when URBI_ENABLE_REPL=1. */
#ifndef UREPL_TRANSPORT_TCP_H
#define UREPL_TRANSPORT_TCP_H

#include "urbi/repl.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct UTcpListener {
    int      listen_fd;
    uint16_t port;       /* kernel-assigned when caller passed 0 */
} UTcpListener;

/* Create a non-blocking IPv4 listener.  bind_addr is "127.0.0.1" (or
 * any dotted-quad), NULL defaults to loopback.  port=0 lets the kernel
 * pick; read it back from the returned struct's `port` field.  Returns
 * NULL on socket()/bind()/listen() failure. */
UTcpListener *urepl_tcp_listener_create(const char *bind_addr, int port);

/* Close + free.  Idempotent on NULL. */
void          urepl_tcp_listener_destroy(UTcpListener *l);

/* The transport vtable.  Pass &UREPL_TCP_TRANSPORT (plus the listener
 * pointer) to urbi_repl_register_transport. */
extern const UTransport UREPL_TCP_TRANSPORT;

#ifdef __cplusplus
}
#endif

#endif /* UREPL_TRANSPORT_TCP_H */
