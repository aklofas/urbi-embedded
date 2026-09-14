/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_buffer_transport.h — a stream made of two byte queues.
 *
 * The in-process loopback the tests drive the service through: what the
 * "client" writes is what the service reads, and what the service writes
 * is what the client reads back.  No socket, no file descriptor, no
 * blocking — which is what lets a .chk fixture be a deterministic list of
 * requests and expected responses.
 *
 * It is also the reference implementation of the four-member UTransport
 * vtable: read returns 0 when the queue is empty AND the client has not
 * finished, and negative once it has, which is exactly the "nothing now"
 * versus "end of stream" distinction a UART or a socket makes. */

#ifndef UREPL_BUFFER_TRANSPORT_H
#define UREPL_BUFFER_TRANSPORT_H

#include "urbi/repl.h"

#include <stddef.h>

typedef struct UBufferTransport UBufferTransport;

/* NULL on OOM.  Free with urepl_buffer_transport_destroy, which is safe
 * to call after the service has closed the transport. */
UBufferTransport *urepl_buffer_transport_create(void);
void              urepl_buffer_transport_destroy(UBufferTransport *bt);

/* Fills `out` with the vtable pointing at `bt`. */
void urepl_buffer_transport_vtable(UBufferTransport *bt, UTransport *out);

/* Client side: queue bytes for the service to read.  Returns the count
 * queued, or 0 on OOM. */
size_t urepl_buffer_client_write(UBufferTransport *bt, const void *bytes, size_t n);

/* Client side: take bytes the service has written.  Returns the count. */
size_t urepl_buffer_client_read(UBufferTransport *bt, void *buf, size_t cap);

/* Client side: declare that no more requests are coming.  The service's
 * next read past the end of the queued bytes reports end of stream, which
 * is what makes the disconnect path reachable from a test. */
void urepl_buffer_client_finish(UBufferTransport *bt);

#endif /* UREPL_BUFFER_TRANSPORT_H */
