/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_buffer_transport.h - in-process loopback transport
 *
 * A pair of byte buffers (client → server, server → client) wrapped in
 * the UTransport vtable.  Used by unit + integration tests to drive
 * the dispatcher end-to-end without opening sockets.
 *
 * Single-client per state object.  accept_fn returns once with a sentinel
 * fd (always 0) and then signals "would block" on subsequent calls. */
#ifndef UREPL_BUFFER_TRANSPORT_H
#define UREPL_BUFFER_TRANSPORT_H

#include "urbi/repl.h"

#include <stddef.h>
#include <stdint.h>

typedef struct UBufferTransportState UBufferTransportState;

/* Allocate a buffer-transport pair.  Returns NULL on OOM. */
UBufferTransportState *urepl_buffer_transport_create(void);

/* Free the transport state. */
void urepl_buffer_transport_destroy(UBufferTransportState *st);

/* The transport vtable.  Pass &UREPL_BUFFER_TRANSPORT (plus the state)
 * to urbi_repl_register_transport. */
extern const UTransport UREPL_BUFFER_TRANSPORT;

/* ---- Client-side helpers --------------------------------------------- */

/* The "client" half of the loopback.  Tests write what the client would
 * send over the wire; the server's read_fn returns those bytes. */
size_t urepl_buffer_client_write(UBufferTransportState *st,
                                 const void *bytes, size_t n);

/* The "client" half of the loopback.  After the dispatcher calls
 * write_fn on the server's fd, the bytes appear here for the test to
 * read. */
size_t urepl_buffer_client_read(UBufferTransportState *st,
                                void *buf, size_t cap);

/* Reset accept_fn — the next call returns the sentinel fd one more
 * time.  Useful when tests want to drive multiple accept cycles. */
void urepl_buffer_transport_reset_accept(UBufferTransportState *st);

#endif /* UREPL_BUFFER_TRANSPORT_H */
